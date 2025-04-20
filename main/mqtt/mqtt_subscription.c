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
#include "medication/medication_storage.h" // Incluir el encabezado de gestión de medicamentos
#include "medication/medication_dispenser.h"
#include "../ntp_func.h"  // Para acceder a las funciones de tiempo NTP

static const char *TAG = "MQTT_SUB";

// Definir para usar respuestas ultra rápidas a ping
#define MQTT_USE_FAST_PING_RESPONSE true

// Declaración externa para la función de procesamiento de comandos LED
extern void process_led_command(char command);

// Buffer para IP del dispositivo
static char device_ip_buffer[16] = "0.0.0.0";

// Reemplazar la función mqtt_sub_get_device_ip

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
    // Garantizar terminación NULL
    esp_ip4addr_ntoa(&ip_info.ip, device_ip_buffer, sizeof(device_ip_buffer) - 1);
    device_ip_buffer[sizeof(device_ip_buffer) - 1] = '\0';
    
    return device_ip_buffer;
}

void process_json_command(const char* json_str) {
    if (!json_str) {
        ESP_LOGE(TAG, "JSON string is null");
        return;
    }
    
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Error parsing JSON: %s", cJSON_GetErrorPtr());
        return;
    }
    
    // Validar timestamp para asegurarnos de que NTP está sincronizado
    int64_t current_time = get_time_ms();
    if (current_time < 1577836800000) { // 01/01/2020 como mínimo
        ESP_LOGW(TAG, "Tiempo no sincronizado correctamente, comandos pueden ser rechazados");
    }
    
    // Reemplazar la sección de detección de ping

#if MQTT_USE_FAST_PING_RESPONSE
    if (strstr(json_str, "\"type\":\"ping\"") != NULL) {
        ESP_LOGI(TAG, "Ping detectado, respondiendo rápidamente");
        
        // Usar cJSON para extraer el clientId de forma más robusta
        cJSON *ping_obj = cJSON_Parse(json_str);
        if (ping_obj) {
            cJSON *client_id_obj = cJSON_GetObjectItem(ping_obj, "clientId");
            const char *client_id = "";
            
            if (client_id_obj && cJSON_IsString(client_id_obj)) {
                client_id = client_id_obj->valuestring;
            }
            
            char pong_buffer[256];
            snprintf(pong_buffer, sizeof(pong_buffer), 
                    "{\"type\":\"pong\",\"status\":\"online\",\"ip\":\"%s\",\"uptime\":%llu,\"clientId\":\"%s\",\"timestamp\":%llu,\"payload\":{}}", 
                    mqtt_sub_get_device_ip(), 
                    esp_timer_get_time() / 1000000,
                    client_id,
                    esp_timer_get_time() / 1000);
            
            esp_mqtt_client_handle_t client = mqtt_connect_get_client();
            if (client != NULL) {
                ESP_LOGI(TAG, "Enviando pong al tópico: %s", MQTT_TOPIC_DEVICE_STATUS);
                int msg_id = esp_mqtt_client_publish(client, MQTT_TOPIC_DEVICE_STATUS, pong_buffer, 0, 0, false);
                if (msg_id >= 0) {
                    ESP_LOGI(TAG, "Respuesta pong enviada correctamente, msg_id=%d", msg_id);
                } else {
                    ESP_LOGW(TAG, "Error enviando respuesta pong");
                }
            }
            
            cJSON_Delete(ping_obj);
        }
        
        // Continuamos con el procesamiento normal por si hay más comandos
    }
#endif

    // Procesamiento JSON normal
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
            // NUEVO: Procesamiento de comando de sincronización de medicamentos
            else if (strcmp(cmd->valuestring, "syncSchedules") == 0) {
                ESP_LOGI(TAG, "Procesando sincronización de medicamentos");
                
                // Obtener timestamp original si existe
                int64_t timestamp = 0;
                cJSON *ts = cJSON_GetObjectItem(root, "timestamp");
                if (ts && cJSON_IsNumber(ts)) {
                    timestamp = (int64_t)ts->valuedouble;
                }
                
                // Procesar el JSON de medicamentos
                esp_err_t result = medication_storage_process_json(json_str);
                
                // Enviar confirmación según resultado
                if (result == ESP_OK) {
                    mqtt_app_publish_med_confirmation(true, 
                        "Sincronización de medicamentos completada con éxito", 
                        timestamp);
                } else {
                    char error_msg[100];
                    snprintf(error_msg, sizeof(error_msg), 
                        "Error al procesar medicamentos: %s", esp_err_to_name(result));
                    mqtt_app_publish_med_confirmation(false, error_msg, timestamp);
                }
            }
            else if (strcmp(cmd->valuestring, "get_telemetry") == 0) {
                // Solicitud de telemetría bajo demanda
                cJSON *telemetry = cJSON_CreateObject();
                cJSON_AddNumberToObject(telemetry, "uptime_s", esp_timer_get_time() / 1000000);
                cJSON_AddNumberToObject(telemetry, "free_heap", esp_get_free_heap_size());
                cJSON_AddNumberToObject(telemetry, "active_led", mqtt_app_get_active_led());
                mqtt_pub_telemetry(telemetry);
            }
            else if (strcmp(cmd->valuestring, "dispense_medication") == 0) {
                // Comando para dispensar manualmente un medicamento
                cJSON *med_id = cJSON_GetObjectItem(payload, "medication_id");
                cJSON *sched_id = cJSON_GetObjectItem(payload, "schedule_id");
                
                if (med_id && cJSON_IsString(med_id) && sched_id && cJSON_IsString(sched_id)) {
                    ESP_LOGI(TAG, "Dispensando medicamento %s (schedule %s) manualmente", 
                            med_id->valuestring, sched_id->valuestring);
                    
                    // Llamar a la función de dispensación manual
                    esp_err_t result = medication_dispenser_manual_dispense(med_id->valuestring, sched_id->valuestring);
                    
                    // Enviar confirmación
                    if (result == ESP_OK) {
                        mqtt_app_publish_med_confirmation(true, "Medicamento dispensado manualmente", 0);
                    } else {
                        mqtt_app_publish_med_confirmation(false, "Error al dispensar medicamento", 0);
                    }
                } else {
                    ESP_LOGW(TAG, "Faltan parámetros para dispensar medicamento");
                }
            }
            else if (strcmp(cmd->valuestring, "set_auto_dispense") == 0) {
                // Comando para configurar dispensación automática
                cJSON *enabled = cJSON_GetObjectItem(payload, "enabled");
                
                if (enabled && cJSON_IsBool(enabled)) {
                    bool auto_enabled = cJSON_IsTrue(enabled);
                    medication_dispenser_set_auto_dispense(auto_enabled);
                    mqtt_app_publish_med_confirmation(true, 
                        auto_enabled ? "Dispensación automática activada" : "Dispensación automática desactivada", 0);
                } else {
                    ESP_LOGW(TAG, "Parámetro inválido para set_auto_dispense");
                }
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
    
    // También suscribirse al tópico de estado (por si acaso la app envía pings allí)
    if (ret == ESP_OK) {
        ret = mqtt_sub_subscribe(MQTT_TOPIC_DEVICE_STATUS, 1);
    }
    
    // Suscribirse al tópico de medicamentos tomados
    if (ret == ESP_OK) {
        ret = mqtt_sub_subscribe(MQTT_TOPIC_MEDICATION_TAKEN, 1);
    }
    
    return ret;
}