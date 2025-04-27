/* Wi-Fi Provisioning Manager Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_log.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <esp_wifi.h> 
#include <esp_intr_alloc.h>

#include "wifi_provisioning.h"
#include "mqtt/mqtt_app.h"
#include "medication/medication_storage.h"
#include "medication/medication_dispenser.h"
#include "ntp_func.h"
#include "buzzer_driver.h"
#include "nextion_driver.h" // Ensure this header includes the declaration for nextion_time_updater_start

#define LED_GPIO_PIN_A 2
#define LED_GPIO_PIN_B 19
#define LED_GPIO_PIN_C 21
#define RESET_BUTTON_GPIO_PIN 23
#define MAX_WIFI_RETRY_COUNT 5

// Variable para rastrear el estado de los LEDs
static int current_active_led = 0; // 0=ninguno, 1=A, 2=B, 3=C
static char device_ip[16]; // Para almacenar la dirección IP como string

static const char *TAG = "app";
static EventGroupHandle_t wifi_event_group = NULL;

// Añadir variables de estado para el botón
static bool wifi_failed = false;
static int wifi_retry_count = 0;
static bool wifi_connected = false;  // Añadir esta línea

// Agrega estas variables para manejar la interrupción del botón
static QueueHandle_t gpio_evt_queue = NULL;
static bool button_pressed_flag = false;
static bool gpio_interrupt_enabled = true;

// Declarar las funciones de callback primero
static void wifi_connection_callback(char *ip);
static void wifi_failure_callback(void);

// Función para configurar los LEDs
static void configure_leds(void)
{
    ESP_LOGI(TAG, "Configurando pines GPIO para LEDs");
    
    // Configurar pin para LED A
    gpio_reset_pin(LED_GPIO_PIN_A);
    gpio_set_direction(LED_GPIO_PIN_A, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(LED_GPIO_PIN_A, GPIO_PULLDOWN_ONLY);  // Añadir resistencia pull-down

    // Configurar pin para LED B
    gpio_reset_pin(LED_GPIO_PIN_B);
    gpio_set_direction(LED_GPIO_PIN_B, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(LED_GPIO_PIN_B, GPIO_PULLDOWN_ONLY);  // Añadir resistencia pull-down

    // Configurar pin para LED C
    gpio_reset_pin(LED_GPIO_PIN_C);
    gpio_set_direction(LED_GPIO_PIN_C, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(LED_GPIO_PIN_C, GPIO_PULLDOWN_ONLY);  // Añadir resistencia pull-down

    // Inicialmente todos los LEDs apagados
    gpio_set_level(LED_GPIO_PIN_A, 0);
    gpio_set_level(LED_GPIO_PIN_B, 0);
    gpio_set_level(LED_GPIO_PIN_C, 0);
}

// Manejador de la interrupción del botón
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

// Tarea que procesa las interrupciones del botón
static void gpio_button_task(void* arg)
{
    uint32_t gpio_num;
    const uint32_t debounce_time_ms = 200;
    uint64_t last_press_time = 0;
    
    while(1) {
        // Espera a recibir un evento desde la interrupción
        if(xQueueReceive(gpio_evt_queue, &gpio_num, portMAX_DELAY)) {
            // Verificar si ha pasado suficiente tiempo desde la última pulsación (debounce)
            uint64_t current_time = esp_timer_get_time() / 1000;
            if (current_time - last_press_time > debounce_time_ms) {
                last_press_time = current_time;
                
                ESP_LOGI(TAG, "Interrupción detectada en GPIO %lu", gpio_num);
                
                // Agregar un pequeño retraso para que el botón se estabilice
                vTaskDelay(20 / portTICK_PERIOD_MS);
                
                // Verificar el nivel del pin directamente (PULL-UP: 0 cuando está presionado)
                int button_level = gpio_get_level(RESET_BUTTON_GPIO_PIN);
                ESP_LOGI(TAG, "Nivel del botón leído: %d", button_level);
                
                if (button_level == 0) {  // Botón está presionado (nivel bajo)
                    ESP_LOGI(TAG, "Botón de reset presionado, iniciando secuencia de reset");
                    button_pressed_flag = true;
                    
                    // Desactivar temporalmente la interrupción para evitar activaciones múltiples
                    gpio_intr_disable(RESET_BUTTON_GPIO_PIN);
                    
                    // Parpadear LEDs para indicar reinicio
                    for (int i = 0; i < 5; i++) {
                        gpio_set_level(LED_GPIO_PIN_A, 1);
                        gpio_set_level(LED_GPIO_PIN_B, 1);
                        gpio_set_level(LED_GPIO_PIN_C, 1);
                        vTaskDelay(100 / portTICK_PERIOD_MS);
                        gpio_set_level(LED_GPIO_PIN_A, 0);
                        gpio_set_level(LED_GPIO_PIN_B, 0);
                        gpio_set_level(LED_GPIO_PIN_C, 0);
                        vTaskDelay(100 / portTICK_PERIOD_MS);
                    }
                    
                    // Reproducir sonido
                    buzzer_play_pattern(BUZZER_PATTERN_PROVISIONING);
                    
                    // Esperar a que termine el sonido
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    
                    // Reiniciar el proceso de provisioning
                    ESP_LOGI(TAG, "Reiniciando modo provisioning");
                    wifi_provisioning_reset_for_reprovision();
                    
                    // Pequeña espera para asegurar que los recursos se liberaron
                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                    
                    // Reiniciar el ESP32
                    ESP_LOGI(TAG, "Reiniciando el dispositivo para un nuevo provisioning limpio");
                    esp_restart();
                }
                
                // Habilitar la interrupción nuevamente después de un tiempo para evitar rebotes
                vTaskDelay(debounce_time_ms / portTICK_PERIOD_MS);
                gpio_intr_enable(RESET_BUTTON_GPIO_PIN);
            }
        }
    }
}

// Función para procesar comandos de LED desde MQTT
void process_led_command(char command)
{
    ESP_LOGI(TAG, "Procesando comando LED: %c", command);
    
    switch (command) {
        case 'A':
            // Encender LED A, apagar los demás
            gpio_set_level(LED_GPIO_PIN_A, 1);
            gpio_set_level(LED_GPIO_PIN_B, 0);
            gpio_set_level(LED_GPIO_PIN_C, 0);
            current_active_led = 1;
            ESP_LOGI(TAG, "LED A encendido");
            break;
            
        case 'B':
            // Encender LED B, apagar los demás
            gpio_set_level(LED_GPIO_PIN_A, 0);
            gpio_set_level(LED_GPIO_PIN_B, 1);
            gpio_set_level(LED_GPIO_PIN_C, 0);
            current_active_led = 2;
            ESP_LOGI(TAG, "LED B encendido");
            break;
            
        case 'C':
            // Encender LED C, apagar los demás
            gpio_set_level(LED_GPIO_PIN_A, 0);
            gpio_set_level(LED_GPIO_PIN_B, 0);
            gpio_set_level(LED_GPIO_PIN_C, 1);
            current_active_led = 3;
            ESP_LOGI(TAG, "LED C encendido");
            break;
            
        default:
            ESP_LOGW(TAG, "Comando desconocido: %c", command);
            break;
    }
}

// Función para publicar el estado del dispositivo por MQTT
static void publish_device_status(const char* status) {
    if (mqtt_app_is_connected()) {
        mqtt_app_publish_status(status);
        ESP_LOGI(TAG, "Estado del dispositivo publicado: %s", status);
    }
}

// Modificar la función wifi_connected_callback

// Callback para cuando se establece conexión WiFi
static void wifi_connection_callback(char *ip) {
    // Guardar la IP
    strlcpy(device_ip, ip, sizeof(device_ip));
    
    // Actualizar la IP en el módulo MQTT
    mqtt_app_set_ip(ip);
    
    // Actualizar estado de conexión
    wifi_connected = true;
    wifi_failed = false;
    wifi_retry_count = 0;  // Importante: resetear el contador de intentos
    
    // Indicación visual - LED A encendido para mostrar conexión exitosa
    gpio_set_level(LED_GPIO_PIN_A, 1);
    gpio_set_level(LED_GPIO_PIN_B, 0);
    gpio_set_level(LED_GPIO_PIN_C, 0);
    
    // Reproducir sonido de conexión WiFi exitosa
    buzzer_play_pattern(BUZZER_PATTERN_WIFI_CONNECTED);
    
    ESP_LOGI(TAG, "Conexión WiFi establecida con IP: %s", ip);
    
    // Sincronizar NTP con múltiples intentos
    ESP_LOGI(TAG, "Sincronizando hora por NTP");
    bool ntp_success = sync_ntp_time_with_retry("EST4", 3);
    
    if (ntp_success) {
        // Reproducir sonido de sincronización NTP exitosa
        buzzer_play_pattern(BUZZER_PATTERN_NTP_SUCCESS);
    } else {
        // Establecer hora por defecto si falla NTP
        ESP_LOGW(TAG, "No se pudo sincronizar hora con NTP. Algunas funciones pueden no operar correctamente.");
        set_default_time("EST4");
    }
    
    // Crear tarea de sincronización periódica
    static bool sync_task_created = false;
    if (!sync_task_created) {
        char *timezone_param = malloc(5);
        if (timezone_param != NULL) {
            strcpy(timezone_param, "EST4");
            xTaskCreate(ntp_periodic_sync_task, "ntp_sync", 4096, (void*)timezone_param, 3, NULL);
            sync_task_created = true;
            ESP_LOGI(TAG, "Tarea de sincronización NTP periódica iniciada");
        }
    }
    
    // Continuar con la inicialización SIEMPRE
    char time_buf[64];
    format_current_time(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S");
    ESP_LOGI(TAG, "Hora actual (posiblemente aproximada): %s", time_buf);
    
    // Inicializar pantalla Nextion - AÑADIR ESTAS LÍNEAS
    ESP_LOGI(TAG, "Inicializando pantalla Nextion");
    if (!nextion_init()) {
        ESP_LOGE(TAG, "Error al inicializar pantalla Nextion");
    } else {
        // Iniciar tarea de recepción de datos desde Nextion
        nextion_start_rx_task();
        
        // Iniciar actualización periódica de fecha/hora
        nextion_time_updater_start("MediDispenser");  // Puedes cambiar el nombre de usuario
        
        ESP_LOGI(TAG, "Pantalla Nextion inicializada correctamente");
    }
    
    // Inicializar sistemas
    ESP_LOGI(TAG, "Inicializando almacenamiento de medicamentos");
    medication_storage_init();
    
    ESP_LOGI(TAG, "Iniciando MQTT");
    mqtt_app_init();
    
    ESP_LOGI(TAG, "Inicializando dispensador de medicamentos");
    medication_dispenser_init();
    
    // Publicar estado cuando todo esté listo
    publish_device_status("online");
}

// Modificar el callback de WiFi para manejar fallos sin límite de reintentos
static void wifi_failure_callback(void) {
    wifi_retry_count++;
    wifi_connected = false;
    
    // Indicación visual de intento de reconexión: parpadeo del LED B
    static bool led_state = false;
    led_state = !led_state;
    
    gpio_set_level(LED_GPIO_PIN_A, led_state);
    gpio_set_level(LED_GPIO_PIN_B, 0);
    gpio_set_level(LED_GPIO_PIN_C, 0);
    
    // Reproducir sonido de fallo WiFi (solo cada 5 intentos para no molestar)
    if (wifi_retry_count % 5 == 1) {
        buzzer_play_pattern(BUZZER_PATTERN_WIFI_FAILED);
    }
    
    // Simplemente loguear el intento sin detener el WiFi
    ESP_LOGW(TAG, "Fallo de conexión WiFi, intento %d. Continuando reconexión...", wifi_retry_count);
}

// Modificación en app_main para configurar el botón correctamente
void app_main(void)
{
    // Variables e inicialización
    ESP_LOGI(TAG, "Inicializando aplicación...");

    // 1. Configurar LEDs
    configure_leds();
    
    // 2. Inicializar buzzer
    buzzer_init();
    
    // Reproducir secuencia de inicio
    buzzer_play_pattern(BUZZER_PATTERN_STARTUP);
    
    // 2. Configurar botón con interrupción
    // Crear una cola para manejar eventos de interrupción
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    
    // Configurar el pin del botón con pull-up
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RESET_BUTTON_GPIO_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,     // Habilitar resistencia pull-up interna
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,       // Interrupción en flanco descendente (botón presionado)
    };
    gpio_config(&io_conf);
    
    // Instalar el servicio de interrupción con mayor prioridad
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1));
    
    // Conectar el manejador de la interrupción con el GPIO específico
    ESP_ERROR_CHECK(gpio_isr_handler_add(RESET_BUTTON_GPIO_PIN, gpio_isr_handler, (void*) RESET_BUTTON_GPIO_PIN));
    
    // Imprimir estado inicial del botón
    ESP_LOGI(TAG, "Estado inicial del botón: %d", gpio_get_level(RESET_BUTTON_GPIO_PIN));
    
    // Crear tarea para procesar las interrupciones del botón con mayor stack
    xTaskCreate(gpio_button_task, "gpio_button_task", 4096, NULL, 10, NULL);
    
    // 3. Registrar callbacks para eventos WiFi
    wifi_provisioning_set_callback(wifi_connection_callback);
    wifi_provisioning_set_failure_callback(wifi_failure_callback);
    
    // 4. Inicializar WiFi provisioning
    ESP_LOGI(TAG, "Iniciando provisioning WiFi con callbacks personalizados");
    wifi_event_group = wifi_provisioning_init();
    
    // La lógica principal se ejecutará en los callbacks
}