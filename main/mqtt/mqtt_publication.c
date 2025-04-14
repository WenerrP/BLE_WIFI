#include <string.h>         // Para strncpy, strcmp
#include <stdio.h>          // Para printf, sprintf
#include "mqtt_publication.h"
#include "mqtt_connection.h"
#include "mqtt_app.h"       // Para las constantes de tópicos y funciones
#include "esp_log.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "mqtt_client.h"    // Para esp_mqtt_client_handle_t y esp_mqtt_client_publish
#include "sdkconfig.h" 

static const char *TAG = "MQTT_PUB";
static char device_ip[16] = "0.0.0.0"; // Default IP

// Actualizar IP (necesario para los mensajes de estado)
void mqtt_pub_set_ip(const char* ip) {
    if (ip) {
        strncpy(device_ip, ip, sizeof(device_ip) - 1);
        device_ip[sizeof(device_ip) - 1] = '\0'; // Garantizar terminación NULL
        ESP_LOGI(TAG, "IP actualizada: %s", device_ip);
    }
}

esp_err_t mqtt_pub_message(const char *topic, const char *data, int len, int qos, bool retain) {
    esp_mqtt_client_handle_t client = mqtt_connect_get_client();
    
    if (client == NULL || !mqtt_connect_is_connected()) {
        ESP_LOGE(TAG, "Cliente MQTT no inicializado o no conectado");
        return ESP_FAIL;
    }
    
    int msg_id = esp_mqtt_client_publish(client, topic, data, len, qos, retain);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Error publicando mensaje en el tópico %s", topic);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Mensaje publicado con éxito en el tópico %s, msg_id=%lu", topic, (unsigned long)msg_id);
    return ESP_OK;
}

esp_err_t mqtt_pub_status(const char* status) {
    if (!mqtt_connect_is_connected() && strcmp(status, "offline") != 0) {
        return ESP_FAIL;
    }
    
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Error creando objeto JSON");
        return ESP_FAIL;
    }
    
    // Información básica
    cJSON_AddStringToObject(root, "type", MQTT_MSG_TYPE_STATUS);
    cJSON_AddStringToObject(root, "status", status);
    cJSON_AddStringToObject(root, "ip", device_ip);
    cJSON_AddNumberToObject(root, "uptime", esp_timer_get_time() / 1000000); // En segundos
    
    // Información adicional
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "active_led", mqtt_app_get_active_led()); // Usar la función centralizada
    
    // Tiempo desde la última actualización
    static uint32_t last_update_time = 0;
    uint32_t current_time = esp_timer_get_time() / 1000000;
    uint32_t time_since_last = last_update_time > 0 ? current_time - last_update_time : 0;
    cJSON_AddNumberToObject(root, "time_since_last_update", time_since_last);
    last_update_time = current_time;
    
    char *json_str = cJSON_Print(root);
    esp_err_t ret = ESP_FAIL;
    
    if (json_str) {
        // Usamos retain=true para que el último estado esté siempre disponible
        ret = mqtt_pub_message(MQTT_TOPIC_DEVICE_STATUS, json_str, 0, 1, true);
        free(json_str);
    }
    
    cJSON_Delete(root);
    return ret;
}

esp_err_t mqtt_pub_json_message(const char* topic, const char* type, cJSON *payload) {
    if (!mqtt_connect_is_connected() || !payload) {
        return ESP_FAIL;
    }
    
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Error creando objeto JSON");
        return ESP_FAIL;
    }
    
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddItemToObject(root, "payload", payload); // Transfiere propiedad
    
    char *json_str = cJSON_Print(root);
    esp_err_t ret = ESP_FAIL;
    
    if (json_str) {
        ret = mqtt_pub_message(topic, json_str, 0, 1, 0);
        free(json_str);
    }
    
    cJSON_Delete(root);
    return ret;
}

esp_err_t mqtt_pub_telemetry(cJSON *payload) {
    return mqtt_pub_json_message(MQTT_TOPIC_DEVICE_TELEMETRY, 
                               MQTT_MSG_TYPE_TELEMETRY, 
                               payload);
}

esp_err_t mqtt_app_publish_med_confirmation(bool success, const char* message, int64_t timestamp) {
    if (!mqtt_connect_is_connected()) {
        ESP_LOGW(TAG, "No se puede enviar confirmación de medicamentos: MQTT no conectado");
        return ESP_FAIL;
    }
    
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "Error creando objeto JSON para confirmación de medicamentos");
        return ESP_ERR_NO_MEM;
    }
    
    // Información básica del mensaje
    cJSON_AddStringToObject(root, "type", MQTT_MSG_TYPE_MED_CONFIRM);
    cJSON_AddBoolToObject(root, "success", success);
    
    // Añadir mensaje descriptivo
    if (message) {
        cJSON_AddStringToObject(root, "message", message);
    } else {
        cJSON_AddStringToObject(root, "message", success ? "Medicamentos procesados correctamente" : "Error al procesar medicamentos");
    }
    
    // Usar timestamp proporcionado o el actual
    int64_t current_time = (timestamp > 0) ? timestamp : (esp_timer_get_time() / 1000);
    cJSON_AddNumberToObject(root, "timestamp", current_time);
    
    // Información adicional útil
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    
    // Convertir a string JSON
    char *json_str = cJSON_Print(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Error generando string JSON para confirmación de medicamentos");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    
    // Publicar el mensaje con QoS 1 para garantizar entrega
    esp_err_t result = mqtt_pub_message(MQTT_TOPIC_MED_CONFIRMATION, json_str, 0, 1, false);
    
    ESP_LOGI(TAG, "Confirmación de medicamentos enviada: %s (%s)", 
             success ? "ÉXITO" : "ERROR", message ? message : "");
    
    free(json_str);
    cJSON_Delete(root);
    
    return result;
}