#include "mqtt_subscription.h"
#include "mqtt_connection.h"
#include "mqtt_publication.h"
#include "mqtt_app.h" // Para las constantes de tópicos
#include "esp_log.h"
#include "cJSON.h"
#include "sdkconfig.h" 

static const char *TAG = "MQTT_SUB";

// Declaración externa para la función de procesamiento de comandos LED
extern void process_led_command(char command);

esp_err_t mqtt_sub_subscribe(const char *topic, int qos) {
    esp_mqtt_client_handle_t client = mqtt_connect_get_client();
    
    if (client == NULL || !mqtt_connect_is_connected()) {
        ESP_LOGE(TAG, "Cliente MQTT no inicializado o no conectado");
        return ESP_FAIL;
    }
    
    int msg_id = esp_mqtt_client_subscribe(client, topic, qos);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Error suscribiéndose al tópico %s", topic);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Suscrito con éxito al tópico %s, msg_id=%d", topic, msg_id);
    return ESP_OK;
}

esp_err_t mqtt_sub_unsubscribe(const char *topic) {
    esp_mqtt_client_handle_t client = mqtt_connect_get_client();
    
    if (client == NULL || !mqtt_connect_is_connected()) {
        ESP_LOGE(TAG, "Cliente MQTT no inicializado o no conectado");
        return ESP_FAIL;
    }
    
    int msg_id = esp_mqtt_client_unsubscribe(client, topic);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Error cancelando suscripción al tópico %s", topic);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Cancelada suscripción al tópico %s, msg_id=%d", topic, msg_id);
    return ESP_OK;
}

void process_json_command(const char* json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Error al analizar JSON: %s", json_str);
        return;
    }
    
    // Extraer el tipo de mensaje
    cJSON *type_obj = cJSON_GetObjectItem(root, "type");
    if (!type_obj || !cJSON_IsString(type_obj)) {
        ESP_LOGW(TAG, "Mensaje JSON recibido no tiene tipo válido");
        cJSON_Delete(root);
        return;
    }
    
    const char *type = type_obj->valuestring;
    
    // Procesar comandos
    if (strcmp(type, MQTT_MSG_TYPE_COMMAND) == 0) {
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (!payload) {
            ESP_LOGW(TAG, "Comando sin payload");
            cJSON_Delete(root);
            return;
        }
        
        // Procesar comandos específicos
        cJSON *cmd = cJSON_GetObjectItem(payload, "cmd");
        if (cmd && cJSON_IsString(cmd)) {
            ESP_LOGI(TAG, "Comando recibido: %s", cmd->valuestring);
            
            // Comandos LED
            if (strcmp(cmd->valuestring, "led_a") == 0) {
                mqtt_app_process_led_command('A');
            } 
            else if (strcmp(cmd->valuestring, "led_b") == 0) {
                mqtt_app_process_led_command('B');
            }
            else if (strcmp(cmd->valuestring, "led_c") == 0) {
                mqtt_app_process_led_command('C');
            }
            else {
                ESP_LOGW(TAG, "Comando desconocido: %s", cmd->valuestring);
            }
        }
    }
    
    cJSON_Delete(root);
}

esp_err_t mqtt_sub_init(void) {
    ESP_LOGI(TAG, "Inicializando suscripciones MQTT");
    
    if (!mqtt_connect_is_connected()) {
        ESP_LOGW(TAG, "MQTT no conectado, no se pueden inicializar suscripciones");
        return ESP_FAIL;
    }
    
    // Suscribirse a los tópicos necesarios
    esp_err_t ret = mqtt_sub_subscribe(MQTT_TOPIC_DEVICE_COMMANDS, 1);
    
    return ret;
}