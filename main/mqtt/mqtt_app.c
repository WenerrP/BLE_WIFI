#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "mqtt_app.h"
#include "mqtt_connection.h"
#include "mqtt_publication.h"
#include "mqtt_subscription.h"
#include "wifi_provisioning.h"

static const char *TAG = "MQTT_APP";

// Variable para rastrear el LED activo, ahora centralizada en este módulo
static int current_active_led = 0; // 0=ninguno, 1=A, 2=B, 3=C
static bool mqtt_initialized = false;

// Callback para manejar la información del paciente
static void (*patient_info_callback)(const char *patient_name, const char *patient_id) = NULL;

// Variables globales para los tópicos MQTT
char mqtt_topic_device_commands[64];
char mqtt_topic_device_status[64];
char mqtt_topic_device_telemetry[64];
char mqtt_topic_device_response[64];
char mqtt_topic_med_confirmation[64];
char mqtt_topic_medication_taken[64];
char mqtt_topic_patient_info[64];

// Función para registrar el callback
void mqtt_set_patient_info_callback(patient_info_callback_t callback) {
    patient_info_callback = callback;
}

// Función para que mqtt_connection.c pueda invocar el callback
void mqtt_app_process_patient_info(const char *json_str) {
    if (!json_str || !patient_info_callback) {
        ESP_LOGW(TAG, "No se puede procesar info del paciente: %s", 
                 !json_str ? "JSON nulo" : "Callback no registrado");
        return;
    }
    
    ESP_LOGI(TAG, "Procesando JSON de paciente: %s", json_str);
    
    // Parsear el JSON
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Error parseando JSON de paciente");
        return;
    }
    
    // Extraer el nombre y ID
    cJSON *name_obj = cJSON_GetObjectItem(root, "patientName");
    cJSON *id_obj = cJSON_GetObjectItem(root, "patientId");
    
    const char *name = (name_obj && cJSON_IsString(name_obj)) ? name_obj->valuestring : NULL;
    const char *id = (id_obj && cJSON_IsString(id_obj)) ? id_obj->valuestring : NULL;
    
    // Invocar el callback con la información extraída
    if (name) {
        ESP_LOGI(TAG, "LLAMANDO A CALLBACK con nombre: %s", name);
        patient_info_callback(name, id ? id : "N/A");
    } else {
        ESP_LOGW(TAG, "No se encontró campo patientName en el JSON");
    }
    
    cJSON_Delete(root);
}

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

// Función para que mqtt_connection.c pueda acceder al callback:
void mqtt_app_handle_patient_info(const char *patient_name, const char *patient_id) {
    if (patient_info_callback != NULL) {
        patient_info_callback(patient_name, patient_id);
    }
}

void mqtt_app_init_topics(void) {
    const char *device_name = wifi_provisioning_get_device_name();

    snprintf(mqtt_topic_device_commands, sizeof(mqtt_topic_device_commands), "mediwatch/%s/commands", device_name);
    snprintf(mqtt_topic_device_status, sizeof(mqtt_topic_device_status), "mediwatch/%s/status", device_name);
    snprintf(mqtt_topic_device_telemetry, sizeof(mqtt_topic_device_telemetry), "mediwatch/%s/telemetry", device_name);
    snprintf(mqtt_topic_device_response, sizeof(mqtt_topic_device_response), "mediwatch/%s/response", device_name);
    snprintf(mqtt_topic_med_confirmation, sizeof(mqtt_topic_med_confirmation), "mediwatch/%s/med_confirmation", device_name);
    snprintf(mqtt_topic_medication_taken, sizeof(mqtt_topic_medication_taken), "mediwatch/%s/taken", device_name);
    snprintf(mqtt_topic_patient_info, sizeof(mqtt_topic_patient_info), "mediwatch/%s/name", device_name);
}