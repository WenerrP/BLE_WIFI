#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "mqtt_app.h"
#include "mqtt_connection.h"
#include "mqtt_publication.h"
#include "mqtt_subscription.h"

static const char *TAG = "MQTT_APP";

// Variable para rastrear el LED activo, ahora centralizada en este módulo
static int current_active_led = 0; // 0=ninguno, 1=A, 2=B, 3=C
static bool mqtt_initialized = false;

// Declaración externa de la función real en app_main.c
extern void process_led_command(char command);

// Esta función es un wrapper que llama a la implementación real
void mqtt_app_process_led_command(char command) {
    ESP_LOGI(TAG, "MQTT: Reenviando comando LED: %c", command);
    process_led_command(command);
}

// Funciones de la API pública que actúan como wrappers 
// para las implementaciones en los módulos especializados

void mqtt_app_init(void) {
    ESP_LOGI(TAG, "Iniciando aplicación MQTT");
    mqtt_connect_init();
    mqtt_sub_init();
    mqtt_initialized = true;
}

void mqtt_app_deinit(void) {
    ESP_LOGI(TAG, "Deteniendo aplicación MQTT");
    mqtt_connect_deinit();
    mqtt_initialized = false;
}

bool mqtt_app_is_connected(void) {
    return mqtt_connect_is_connected();
}

void mqtt_app_set_ip(const char* ip) {
    mqtt_connect_set_ip(ip);
    mqtt_pub_set_ip(ip);
}

esp_err_t mqtt_app_publish_status(const char* status) {
    return mqtt_pub_status(status);
}

esp_err_t mqtt_app_publish_telemetry(cJSON *payload) {
    return mqtt_pub_telemetry(payload);
}

esp_err_t mqtt_app_publish(const char *topic, const char *data, int len, int qos, bool retain) {
    return mqtt_pub_message(topic, data, len, qos, retain);
}

esp_err_t mqtt_app_subscribe(const char *topic, int qos) {
    return mqtt_sub_subscribe(topic, qos);
}

esp_err_t mqtt_app_unsubscribe(const char *topic) {
    return mqtt_sub_unsubscribe(topic);
}

// Funciones nuevas para manejar el LED activo
int mqtt_app_get_active_led(void) {
    return current_active_led;
}

void mqtt_app_set_active_led(int led_num) {
    current_active_led = led_num;
    ESP_LOGI(TAG, "LED activo cambiado a: %d", led_num);
}

// Función para iniciar MQTT desde app_main.c
void mqtt_app_start(void) {
    if (!mqtt_initialized) {
        ESP_LOGI(TAG, "Iniciando MQTT desde app_main");
        mqtt_app_init();
    } else {
        ESP_LOGW(TAG, "MQTT ya está inicializado");
    }
}