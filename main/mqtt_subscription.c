#include <string.h>         // Para strcmp, strstr
#include <stdio.h>          // Para funciones de E/S
#include "mqtt_subscription.h"
#include "mqtt_connection.h"
#include "mqtt_publication.h"
#include "mqtt_app.h"       // Para las constantes de tópicos
#include "esp_log.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"      // Nuevo API de red
#include "mqtt_client.h"    // Para esp_mqtt_client_handle_t y funciones MQTT

static const char *TAG = "MQTT_SUB";

// Definir para usar respuestas ultra rápidas a ping
#define MQTT_USE_FAST_PING_RESPONSE true

// Declaración externa para la función de procesamiento de comandos LED
extern void process_led_command(char command);

// Buffer para IP del dispositivo
static char device_ip_buffer[16] = "0.0.0.0";

// Función para obtener la IP actual (versión actualizada)
char* mqtt_sub_get_device_ip(void) {
    esp_netif_ip_info_t ip_info;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    
    if (netif == NULL) {
        ESP_LOGW(TAG, "No se pudo obtener el netif para WIFI_STA_DEF");
        return device_ip_buffer;
    }
    
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo obtener la información IP");
        return device_ip_buffer;
    }
    
    // Convertir la IP a cadena utilizando la función de utilidad del ESP-IDF
    esp_ip4addr_ntoa(&ip_info.ip, device_ip_buffer, sizeof(device_ip_buffer));
    return device_ip_buffer;
}

void process_json_command(const char* json_str) {
    // Detección rápida de ping para respuesta inmediata
#if MQTT_USE_FAST_PING_RESPONSE
    if (strstr(json_str, "\"type\":\"ping\"") != NULL) {
        ESP_LOGI(TAG, "Ping detectado, respondiendo rápidamente");
        
        // Crear una respuesta pong mínima pero informativa
        char pong_buffer[128];
        snprintf(pong_buffer, sizeof(pong_buffer), 
                "{\"type\":\"pong\",\"status\":\"online\",\"ip\":\"%s\",\"uptime\":%llu}", 
                mqtt_sub_get_device_ip(), esp_timer_get_time() / 1000000);
        
        esp_mqtt_client_handle_t client = mqtt_connect_get_client();
        if (client != NULL) {
            esp_mqtt_client_publish(client, MQTT_TOPIC_DEVICE_STATUS, pong_buffer, 0, 0, false);
        }
        
        // Aún procesamos el JSON para otros posibles comandos
    }
#endif

    // Procesamiento JSON normal
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
    
    // Respuesta detallada a ping (solo si no estamos usando la respuesta rápida)
#if !MQTT_USE_FAST_PING_RESPONSE
    if (strcmp(type, "ping") == 0) {
        ESP_LOGI(TAG, "Recibido ping, respondiendo con pong");
        
        // Crear mensaje pong completo
        cJSON *pong = cJSON_CreateObject();
        cJSON_AddStringToObject(pong, "type", "pong");
        cJSON_AddStringToObject(pong, "status", "online");
        cJSON_AddStringToObject(pong, "ip", mqtt_sub_get_device_ip());
        cJSON_AddNumberToObject(pong, "uptime", esp_timer_get_time() / 1000000);
        cJSON_AddNumberToObject(pong, "free_heap", esp_get_free_heap_size());
        cJSON_AddNumberToObject(pong, "active_led", mqtt_app_get_active_led());
        
        // Obtener payload del ping si existe
        cJSON *ping_payload = cJSON_GetObjectItem(root, "payload");
        if (ping_payload && cJSON_IsObject(ping_payload)) {
            // Extraer cualquier información relevante del ping
            cJSON *ping_id = cJSON_GetObjectItem(ping_payload, "id");
            if (ping_id && cJSON_IsNumber(ping_id)) {
                cJSON_AddNumberToObject(pong, "ping_id", ping_id->valueint);
            }
            
            cJSON *timestamp = cJSON_GetObjectItem(ping_payload, "timestamp");
            if (timestamp && cJSON_IsNumber(timestamp)) {
                cJSON_AddNumberToObject(pong, "ping_timestamp", timestamp->valueint);
                // Calcular latencia si se proporciona timestamp
                cJSON_AddNumberToObject(pong, "response_time_ms", (esp_timer_get_time() / 1000) - timestamp->valueint);
            }
        }
        
        // Publicar respuesta en el tópico de estado
        char *pong_str = cJSON_Print(pong);
        if (pong_str) {
            esp_mqtt_client_handle_t client = mqtt_connect_get_client();
            if (client != NULL) {
                esp_mqtt_client_publish(client, MQTT_TOPIC_DEVICE_STATUS, pong_str, 0, 0, false);
            }
            free(pong_str);
        }
        cJSON_Delete(pong);
        cJSON_Delete(root);
        return;
    }
#endif
    
    // Procesar comandos normales
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
                process_led_command('A');
            } 
            else if (strcmp(cmd->valuestring, "led_b") == 0) {
                process_led_command('B');
            }
            else if (strcmp(cmd->valuestring, "led_c") == 0) {
                process_led_command('C');
            }
            else if (strcmp(cmd->valuestring, "get_telemetry") == 0) {
                // Solicitud de telemetría bajo demanda
                cJSON *telemetry = cJSON_CreateObject();
                cJSON_AddNumberToObject(telemetry, "uptime_s", esp_timer_get_time() / 1000000);
                cJSON_AddNumberToObject(telemetry, "free_heap", esp_get_free_heap_size());
                cJSON_AddNumberToObject(telemetry, "active_led", mqtt_app_get_active_led());
                mqtt_pub_telemetry(telemetry);
            }
            else {
                ESP_LOGW(TAG, "Comando desconocido: %s", cmd->valuestring);
            }
        }
    }
    
    cJSON_Delete(root);
}

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

esp_err_t mqtt_sub_init(void) {
    ESP_LOGI(TAG, "Inicializando suscripciones MQTT");
    
    if (!mqtt_connect_is_connected()) {
        ESP_LOGW(TAG, "MQTT no conectado, no se pueden inicializar suscripciones");
        return ESP_FAIL;
    }
    
    // Suscribirse a todos los tópicos necesarios
    esp_err_t ret = mqtt_sub_subscribe(MQTT_TOPIC_DEVICE_COMMANDS, 1);
    
    return ret;
}