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

// Incluir nuestros módulos
#include "wifi_provisioning.h"
#include "mqtt/mqtt_app.h"
#include "medication/medication_storage.h"
#include "medication/medication_dispenser.h"
#include "ntp_func.h"

#define LED_GPIO_PIN_A 2
#define LED_GPIO_PIN_B 13
#define LED_GPIO_PIN_C 14
#define RESET_BUTTON_GPIO_PIN 12
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

// Tarea para monitorear el botón de reset
static void button_task(void *pvParameter) {
    // Configurar el pin del botón con resistencia pull-up interna
    gpio_reset_pin(RESET_BUTTON_GPIO_PIN);
    gpio_set_direction(RESET_BUTTON_GPIO_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(RESET_BUTTON_GPIO_PIN, GPIO_PULLUP_ONLY);
    
    // Estado anterior del botón
    int last_level = 1; // Asumimos pull-up, por lo que 1 es el estado sin presionar
    
    while (1) {
        // Leer el estado actual del botón
        int level = gpio_get_level(RESET_BUTTON_GPIO_PIN);
        
        // Detectar flanco descendente (botón presionado)
        if (last_level == 1 && level == 0) {
            ESP_LOGI(TAG, "Botón de reset presionado");
            
            if (wifi_failed) {
                ESP_LOGI(TAG, "Reiniciando modo provisioning después de fallos WiFi");
                
                // Reiniciar el contador de intentos
                wifi_retry_count = 0;
                wifi_failed = false;
                
                // Reiniciar el proceso de provisioning de forma segura
                wifi_provisioning_reset_for_reprovision();
                
                // Esperar un breve momento para asegurar que todos los recursos se hayan liberado
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                
                // Reiniciar el ESP32 en lugar de intentar reiniciar solo el provisioning
                ESP_LOGI(TAG, "Reiniciando el dispositivo para un nuevo provisioning limpio");
                esp_restart();
                
                // El código no llegará aquí debido al reinicio
            }
            
            // Debounce: pequeña pausa para evitar lecturas múltiples
            vTaskDelay(50 / portTICK_PERIOD_MS);
        }
        
        // Actualizar el estado anterior
        last_level = level;
        
        // Retraso para no consumir mucha CPU
        vTaskDelay(50 / portTICK_PERIOD_MS);
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
    
    ESP_LOGI(TAG, "Conexión WiFi establecida con IP: %s", ip);
    
    // Sincronizar NTP con múltiples intentos
    ESP_LOGI(TAG, "Sincronizando hora por NTP");
    bool ntp_success = sync_ntp_time_with_retry("EST4", 3);
    
    // Establecer hora por defecto si falla NTP
    if (!ntp_success) {
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

// Modificar el callback de WiFi para manejar fallos
static void wifi_failure_callback(void) {
    wifi_retry_count++;
    
    wifi_connected = false;  // Añadir esta línea
    
    if (wifi_retry_count >= MAX_WIFI_RETRY_COUNT) {
        ESP_LOGW(TAG, "Máximo número de intentos WiFi alcanzado (%d). Esperando botón de reset.", MAX_WIFI_RETRY_COUNT);
        wifi_failed = true;
        
        // Apagamos el WiFi para ahorrar energía
        esp_wifi_stop();  // Ahora está correctamente declarada con el include
        
        // Encender el LED A para indicar modo de error (opcional)
        gpio_set_level(LED_GPIO_PIN_A, 1);
        gpio_set_level(LED_GPIO_PIN_B, 0);
        gpio_set_level(LED_GPIO_PIN_C, 0);
    } else {
        ESP_LOGW(TAG, "Fallo de conexión WiFi, intento %d de %d", wifi_retry_count, MAX_WIFI_RETRY_COUNT);
    }
}

// Modificación al app_main para crear la tarea de monitoreo del botón
void app_main(void)
{
    // Variables e inicialización
    ESP_LOGI(TAG, "Inicializando aplicación...");

    // 1. Configurar LEDs
    configure_leds();
    
    // 2. Registrar callbacks para eventos WiFi
    wifi_provisioning_set_callback(wifi_connection_callback);
    wifi_provisioning_set_failure_callback(wifi_failure_callback);
    
    // 3. Inicializar WiFi provisioning
    ESP_LOGI(TAG, "Iniciando provisioning WiFi con callbacks personalizados");
    wifi_event_group = wifi_provisioning_init();
    
    // 4. Crear tarea para botón de reset
    xTaskCreate(button_task, "button_task", 2048, NULL, 10, NULL);
    
    // La lógica principal se ejecutará en los callbacks
}