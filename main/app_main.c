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
#include <esp_wifi.h>  // Para esp_wifi_stop()

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
    
    // No iniciamos MQTT aquí, se hará en el flujo principal
    ESP_LOGI(TAG, "Conexión WiFi establecida con IP: %s", ip);
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
    bool ntp_synced = false;

    // 1. Inicializar WiFi y esperar conexión
    ESP_LOGI(TAG, "Iniciando WiFi provisioning");
    // IMPORTANTE: Mantener esta línea - inicializa el provisioning WiFi y devuelve el grupo de eventos
    EventGroupHandle_t wifi_event_group = wifi_provisioning_init();
    
    // Registrar los callbacks para notificación de conexión/fallo
    wifi_provisioning_set_callback(wifi_connection_callback);
    wifi_provisioning_set_failure_callback(wifi_failure_callback);
    
    // Opcional: Esperar a que se establezca la conexión (si es necesario en el flujo)
    // wifi_provisioning_wait_for_connection(wifi_event_group);

    // 2. Sincronizar la hora con NTP (solo después de tener conexión WiFi)
    ESP_LOGI(TAG, "Sincronizando hora por NTP");
    bool ntp_success = sync_ntp_time("EST4");
    if (!ntp_success) {
        ESP_LOGW(TAG, "No se pudo sincronizar hora con NTP. Algunas funciones pueden no operar correctamente.");
        // Intentar otra vez después de un tiempo
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        ntp_success = sync_ntp_time("EST4");
        if (ntp_success) {
            ntp_synced = true;
        } else {
            ESP_LOGE(TAG, "Fallo crítico en sincronización NTP después de reintentos");
        }
    } else {
        ntp_synced = true;
        char time_buf[64];
        format_current_time(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S");
        ESP_LOGI(TAG, "Hora actual: %s", time_buf);
    }
    
    // 3. Inicializar almacenamiento de medicamentos
    ESP_LOGI(TAG, "Inicializando almacenamiento de medicamentos");
    medication_storage_init();
    
    // 4. Inicializar MQTT después de tener conexión WiFi (solo una vez)
    ESP_LOGI(TAG, "Iniciando MQTT");
    mqtt_app_init();  // Usar esta función en lugar de mqtt_app_start()
    
    // 5. Inicializar dispensador de medicamentos
    ESP_LOGI(TAG, "Inicializando dispensador de medicamentos");
    if (ntp_synced) {
        medication_dispenser_init();
    } else {
        ESP_LOGW(TAG, "Dispensador no iniciado por falta de sincronización de tiempo");
    }

    // Bucle principal con manejo de errores
    ESP_LOGI(TAG, "Entrando en bucle principal");
    const int HEARTBEAT_INTERVAL_MS = 10000;
    int64_t last_heartbeat = 0;

    while (1) {
        // Obtener tiempo actual
        int64_t now = esp_timer_get_time() / 1000;
        
        // Si WiFi falló, esperar a que el botón maneje la reconexión
        if (wifi_failed) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        
        // Enviar heartbeat para mantener el estado visible
        if (now - last_heartbeat >= HEARTBEAT_INTERVAL_MS) {
            if (mqtt_app_is_connected()) {
                char status[64];
                snprintf(status, sizeof(status), 
                        "{\"status\":\"online\",\"ip\":\"%s\",\"led\":%d}", 
                        device_ip, current_active_led);
                
                mqtt_app_publish_status(status);
                last_heartbeat = now;
            }
        }
        
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}