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

// Incluir nuestros módulos
#include "wifi_provisioning.h"
#include "mqtt/mqtt_app.h"

#define LED_GPIO_PIN_A 2
#define LED_GPIO_PIN_B 13
#define LED_GPIO_PIN_C 14

// Variable para rastrear el estado de los LEDs
static int current_active_led = 0; // 0=ninguno, 1=A, 2=B, 3=C
static char device_ip[16]; // Para almacenar la dirección IP como string

static const char *TAG = "app";
static EventGroupHandle_t wifi_event_group = NULL;
static bool mqtt_initialized = false;

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

void app_main(void)
{
    // Configurar los LEDs antes de iniciar la conexión
    configure_leds();
    
    // Inicializar el módulo de WiFi Provisioning
    wifi_event_group = wifi_provisioning_init();
    
    // Registrar callback para ser notificado cuando se establezca la conexión WiFi
    wifi_provisioning_set_callback(wifi_connected_callback);
    
    // Esperar a que se establezca la conexión WiFi
    wifi_provisioning_wait_for_connection(wifi_event_group);
    
    // Bucle principal con publicación periódica de estado
    const int HEARTBEAT_INTERVAL_MS = 10000; // 10 segundos
    int64_t last_heartbeat = 0;
    
    while (1) {
        int64_t now = esp_timer_get_time() / 1000; // Tiempo actual en ms
        
        // Publicar heartbeat periódicamente
        if (now - last_heartbeat >= HEARTBEAT_INTERVAL_MS) {
            if (mqtt_app_is_connected()) {
                // Crear un mensaje de estado que incluya el LED activo
                char status[64];
                snprintf(status, sizeof(status), 
                        "{\"status\":\"online\",\"ip\":\"%s\",\"led\":%d}", 
                        device_ip, current_active_led);
                
                publish_device_status(status);
                last_heartbeat = now;
            }
        }
        
        // Esperar antes del siguiente ciclo
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}