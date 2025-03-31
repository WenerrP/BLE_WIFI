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
static bool mqtt_initialized = false;

// Añadir variables de estado para el botón
static bool wifi_failed = false;
static int wifi_retry_count = 0;

// Declarar las funciones de callback primero
static void wifi_connected_callback(char *ip);
static void wifi_connection_failed_callback(void);

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

// Callback para cuando se establece conexión WiFi
static void wifi_connected_callback(char *ip) {
    // Guardar la IP
    strlcpy(device_ip, ip, sizeof(device_ip));
    
    // Actualizar la IP en el módulo MQTT
    mqtt_app_set_ip(ip);
    
    // Iniciar MQTT
    mqtt_app_start();
    
    ESP_LOGI(TAG, "Conexión WiFi establecida con IP: %s", ip);
}

// Modificar el callback de WiFi para manejar fallos
static void wifi_connection_failed_callback(void) {
    wifi_retry_count++;
    
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
    // Configurar los LEDs antes de iniciar la conexión
    configure_leds();
    
    // Crear tarea para monitorear el botón de reset
    xTaskCreate(button_task, "button_task", 2048, NULL, 10, NULL);
    
    // Inicializar el módulo de WiFi Provisioning
    wifi_event_group = wifi_provisioning_init();
    
    // Registrar callbacks para conexión y fallo
    wifi_provisioning_set_callback(wifi_connected_callback);
    wifi_provisioning_set_failure_callback(wifi_connection_failed_callback);
    
    // Bucle principal
    while (1) {
        // Si WiFi falló, no intentes esperar por conexión
        if (!wifi_failed) {
            // Esperar a que se establezca la conexión WiFi
            wifi_provisioning_wait_for_connection(wifi_event_group);
            
            // Si llegamos aquí, tenemos conexión WiFi
            wifi_retry_count = 0; // Resetear contador si nos conectamos
            
            // Bucle de heartbeat, igual que antes
            const int HEARTBEAT_INTERVAL_MS = 10000;
            int64_t last_heartbeat = 0;
            
            while (!wifi_failed) {
                int64_t now = esp_timer_get_time() / 1000;
                
                if (now - last_heartbeat >= HEARTBEAT_INTERVAL_MS) {
                    if (mqtt_app_is_connected()) {
                        char status[64];
                        snprintf(status, sizeof(status), 
                                "{\"status\":\"online\",\"ip\":\"%s\",\"led\":%d}", 
                                device_ip, current_active_led);
                        
                        publish_device_status(status);
                        last_heartbeat = now;
                    }
                }
                
                vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
        } else {
            // Si WiFi falló, simplemente espera - el botón manejará la reconexión
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }
}