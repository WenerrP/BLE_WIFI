#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "mqtt_app.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "cJSON.h"

// Constantes para tipos de mensajes MQTT
#define MQTT_MSG_TYPE_COMMAND        "command"
#define MQTT_MSG_TYPE_STATUS         "status"
#define MQTT_MSG_TYPE_TELEMETRY      "telemetry"
#define MQTT_MSG_TYPE_RESPONSE       "response"

// Tópicos MQTT estándar
#define MQTT_TOPIC_DEVICE_COMMANDS   "/device/commands"
#define MQTT_TOPIC_DEVICE_STATUS     "/device/status" 
#define MQTT_TOPIC_DEVICE_TELEMETRY  "/device/telemetry"
#define MQTT_TOPIC_DEVICE_RESPONSE   "/device/response"

static const char *TAG = "MQTT_APP";
static esp_mqtt_client_handle_t client = NULL;
static esp_timer_handle_t reconnect_timer = NULL;
static int mqtt_retry_count = 0;
static bool mqtt_connected = false;
static char device_ip[16] = "0.0.0.0"; // Default IP
static int current_active_led = 0; // Default active LED

// Constantes para la gestión de MQTT
#define MQTT_RECONNECT_TIMEOUT_MS 5000
#define MQTT_MAX_RETRY_COUNT 5
#define MQTT_NETWORK_TIMEOUT_MS 10000

// Declaraciones adelantadas para las funciones privadas
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void mqtt_reconnect_timer_callback(void* arg);
static uint32_t exponential_backoff(uint8_t retry_count);
static void handle_mqtt_error(esp_mqtt_event_handle_t event);
static void log_error_if_nonzero(const char *message, int error_code);
static esp_err_t publish_json_status(const char* status);
static esp_err_t publish_json_message(const char* topic, const char* type, cJSON *payload);
static void process_json_command(const char* json_str);

// Declaración externa para la función de procesamiento de comandos LED
extern void process_led_command(char command);

// Función para crear un ID de cliente único
static char* generate_client_id(void) {
    uint8_t mac[6];
    char *client_id = malloc(20);
    if (!client_id) {
        return NULL;
    }
    
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(client_id, 20, "esp32_%02x%02x%02x%02x%02x%02x", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    return client_id;
}

// Función para registrar errores no cero
static void log_error_if_nonzero(const char *message, int error_code) {
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last %s: 0x%x", message, error_code);
    }
}

// Manejo de errores MQTT
static void handle_mqtt_error(esp_mqtt_event_handle_t event) {
    ESP_LOGE(TAG, "MQTT Error occurred");
    if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        log_error_if_nonzero("reported from esp-tls", 
                            event->error_handle->esp_tls_last_esp_err);
        log_error_if_nonzero("reported from tls stack",
                            event->error_handle->esp_tls_stack_err);
        log_error_if_nonzero("captured as transport's socket errno",
                            event->error_handle->esp_transport_sock_errno);
    }
}

// Cálculo de backoff exponencial para reconexiones
static uint32_t exponential_backoff(uint8_t retry_count) {
    uint32_t delay = MQTT_RECONNECT_TIMEOUT_MS * (1 << retry_count);
    return (delay > 300000) ? 300000 : delay; // Máximo 5 minutos
}

// Callback del timer de reconexión
static void mqtt_reconnect_timer_callback(void* arg) {
    if (client) {
        ESP_LOGI(TAG, "Reintentando conexión MQTT (intento %d de %d)...", 
                 mqtt_retry_count + 1, MQTT_MAX_RETRY_COUNT);
        esp_mqtt_client_start(client);
    }
}

// Manejador de eventos MQTT simplificado
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    
    // Implementación alternativa que evita usar formatos problemáticos
    switch (event->event_id) {
        case MQTT_EVENT_BEFORE_CONNECT:
            ESP_LOGI(TAG, "MQTT iniciando conexión");
            break;
            
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT conectado al broker");
            mqtt_retry_count = 0;
            mqtt_connected = true;
            
            // Suscribirnos a los tópicos relevantes utilizando nuestra nomenclatura estandarizada
            esp_mqtt_client_subscribe(client, MQTT_TOPIC_DEVICE_COMMANDS, 1);
            
            // Publicar estado online con JSON
            publish_json_status("online");
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT desconectado");
            mqtt_connected = false;  // Set flag to false
            
            // Manejar reconexión sin usar sprintf para el delay
            if (mqtt_retry_count < MQTT_MAX_RETRY_COUNT) {
                uint32_t delay = exponential_backoff(mqtt_retry_count);
                // Evitamos imprimir el valor de delay
                ESP_LOGI(TAG, "Programando reconexión");
                esp_timer_start_once(reconnect_timer, delay * 1000);
                mqtt_retry_count++;
            } else {
                ESP_LOGE(TAG, "Número máximo de intentos alcanzado");
            }
            break;
            
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT subscripción exitosa");
            break;
            
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT cancelación de subscripción exitosa");
            break;
            
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT mensaje publicado exitosamente");
            break;
            
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT datos recibidos");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            
            // Crear una copia terminada en NULL del mensaje
            char *data_copy = malloc(event->data_len + 1);
            if (data_copy) {
                memcpy(data_copy, event->data, event->data_len);
                data_copy[event->data_len] = '\0';
                
                // Procesar como JSON para cualquier tópico relacionado con comandos
                if (strncmp(event->topic, MQTT_TOPIC_DEVICE_COMMANDS, strlen(MQTT_TOPIC_DEVICE_COMMANDS)) == 0) {
                    process_json_command(data_copy);
                }
                
                free(data_copy);
            }
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            handle_mqtt_error(event);
            break;
            
        default:
            ESP_LOGI(TAG, "Otro evento MQTT");
            break;
    }
}

// Update is_connected function
bool mqtt_app_is_connected(void) {
    return (client != NULL && mqtt_connected);
}

void mqtt_app_start(void) {
    ESP_LOGI(TAG, "Iniciando cliente MQTT");
    
    // Si ya existe un cliente, no lo volvemos a crear
    if (client != NULL) {
        ESP_LOGW(TAG, "Cliente MQTT ya inicializado, no se iniciará de nuevo");
        return;
    }
    
    // Generar un ID de cliente único
    char *client_id = generate_client_id();
    if (!client_id) {
        ESP_LOGE(TAG, "Error generando ID de cliente");
        return;
    }
    
    ESP_LOGI(TAG, "MQTT Client ID: %s", client_id);
    
    // Crear el mensaje LWT
    char lwt_message[100];
    snprintf(lwt_message, sizeof(lwt_message), "{\"type\":\"status\",\"status\":\"offline\",\"ip\":\"%s\"}", device_ip);

    // Configurar el cliente MQTT con LWT
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://broker.emqx.io",
        .broker.address.port = 1883,
        .session.keepalive = 30,  // Reducir keepalive para detección más rápida
        .network.timeout_ms = MQTT_NETWORK_TIMEOUT_MS,
        .credentials.client_id = client_id,
        .credentials.username = NULL,
        .session.last_will.topic = MQTT_TOPIC_DEVICE_STATUS,
        .session.last_will.msg = lwt_message,
        .session.last_will.msg_len = strlen(lwt_message),
        .session.last_will.qos = 1,
        .session.last_will.retain = 1
    };
    
    // Inicializar el cliente MQTT
    client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Error inicializando el cliente MQTT");
        free(client_id);
        return;
    }
    
    // Crear timer para la reconexión automática
    esp_timer_create_args_t timer_args = {
        .callback = mqtt_reconnect_timer_callback,
        .name = "mqtt_reconnect"
    };
    
    esp_err_t ret = esp_timer_create(&timer_args, &reconnect_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error creando el timer de reconexión: %s", esp_err_to_name(ret));
        esp_mqtt_client_destroy(client);
        client = NULL;
        free(client_id);
        return;
    }
    
    // Registramos el handler de eventos MQTT
    ret = esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error registrando el handler de eventos MQTT: %s", esp_err_to_name(ret));
        esp_timer_delete(reconnect_timer);
        reconnect_timer = NULL;
        esp_mqtt_client_destroy(client);
        client = NULL;
        free(client_id);
        return;
    }
    
    // Iniciamos el cliente
    mqtt_retry_count = 0;
    ret = esp_mqtt_client_start(client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando el cliente MQTT: %s", esp_err_to_name(ret));
        esp_timer_delete(reconnect_timer);
        reconnect_timer = NULL;
        esp_mqtt_client_destroy(client);
        client = NULL;
    }
    
    free(client_id); // Liberamos la memoria del client_id una vez usado
}

// Update publish function
esp_err_t mqtt_app_publish(const char *topic, const char *data, int len, int qos, bool retain) {
    if (client == NULL || !mqtt_connected) {
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

// Update subscribe function
esp_err_t mqtt_app_subscribe(const char *topic, int qos) {
    if (client == NULL || !mqtt_connected) {
        ESP_LOGE(TAG, "Cliente MQTT no inicializado o no conectado");
        return ESP_FAIL;
    }
    
    int msg_id = esp_mqtt_client_subscribe(client, topic, qos);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Error suscribiéndose al tópico %s", topic);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Suscrito con éxito al tópico %s, msg_id=%lu", topic, (unsigned long)msg_id);
    return ESP_OK;
}

// Update unsubscribe function
esp_err_t mqtt_app_unsubscribe(const char *topic) {
    if (client == NULL || !mqtt_connected) {
        ESP_LOGE(TAG, "Cliente MQTT no inicializado o no conectado");
        return ESP_FAIL;
    }
    
    int msg_id = esp_mqtt_client_unsubscribe(client, topic);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Error cancelando suscripción al tópico %s", topic);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Cancelada suscripción al tópico %s, msg_id=%lu", topic, (unsigned long)msg_id);
    return ESP_OK;
}

// Update stop function
void mqtt_app_stop(void) {
    if (client == NULL) {
        ESP_LOGW(TAG, "Cliente MQTT ya está detenido");
        return;
    }
    
    // Detener timer de reconexión si está activo
    if (reconnect_timer) {
        if (esp_timer_is_active(reconnect_timer)) {
            esp_timer_stop(reconnect_timer);
        }
        esp_timer_delete(reconnect_timer);
        reconnect_timer = NULL;
    }
    
    // Publicar mensaje de desconexión si estamos conectados
    if (mqtt_connected) {
        
        char json_message[100];

        snprintf(json_message, sizeof(json_message), "{\"status\":\"offline\",\"ip\":\"%s\"}", device_ip);
        esp_mqtt_client_publish(client, "/device/status", json_message, strlen(json_message), 1, true);
        esp_mqtt_client_disconnect(client);
    }
    
    // Detener y destruir cliente MQTT
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = NULL;
    
    ESP_LOGI(TAG, "Cliente MQTT detenido y recursos liberados");
}

// Añade esta función en mqtt_app.c
void mqtt_app_set_ip(const char* ip) {
    if (ip) {
        strncpy(device_ip, ip, sizeof(device_ip) - 1);
        device_ip[sizeof(device_ip) - 1] = '\0'; // Garantizar terminación NULL
        ESP_LOGI(TAG, "IP actualizada: %s", device_ip);
    }
}

// Añade estas funciones a mqtt_app.c

// Función para publicar un mensaje de estado en JSON
static esp_err_t publish_json_status(const char* status) {
    if (!mqtt_connected && strcmp(status, "offline") != 0) {
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
    cJSON_AddNumberToObject(root, "active_led", current_active_led);
    
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
        ret = esp_mqtt_client_publish(client, MQTT_TOPIC_DEVICE_STATUS, json_str, 0, 1, true);
        free(json_str);
    }
    
    cJSON_Delete(root);
    return (ret > 0) ? ESP_OK : ESP_FAIL;
}

// Función para publicar cualquier mensaje JSON
static esp_err_t publish_json_message(const char* topic, const char* type, cJSON *payload) {
    if (!mqtt_connected || !payload) {
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
        ret = esp_mqtt_client_publish(client, topic, json_str, 0, 1, 0);
        free(json_str);
    }
    
    cJSON_Delete(root);
    return (ret > 0) ? ESP_OK : ESP_FAIL;
}

// Procesar un comando recibido en formato JSON
static void process_json_command(const char* json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Error al analizar JSON: %s", json_str);
        return;
    }
    
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!type || !cJSON_IsString(type)) {
        ESP_LOGW(TAG, "Mensaje JSON recibido no tiene tipo válido");
        cJSON_Delete(root);
        return;
    }
    
    // Responder a un ping
    if (strcmp(type->valuestring, "ping") == 0) {
        ESP_LOGI(TAG, "Comando ping recibido, respondiendo");
        
        // Crear respuesta de ping
        cJSON *response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "type", "pong");
        cJSON_AddStringToObject(response, "status", "online");
        cJSON_AddStringToObject(response, "ip", device_ip);
        cJSON_AddNumberToObject(response, "uptime", esp_timer_get_time() / 1000000);
        cJSON_AddNumberToObject(response, "free_heap", esp_get_free_heap_size());
        cJSON_AddNumberToObject(response, "active_led", current_active_led);
        
        // Publicar respuesta
        char *json_str = cJSON_Print(response);
        if (json_str) {
            esp_mqtt_client_publish(client, MQTT_TOPIC_DEVICE_RESPONSE, json_str, 0, 1, 0);
            free(json_str);
            ESP_LOGI(TAG, "Respuesta de ping enviada");
        }
        
        cJSON_Delete(response);
        cJSON_Delete(root);
        return;
    }
    
    // Procesar otros tipos de comandos como antes
    if (strcmp(type->valuestring, MQTT_MSG_TYPE_COMMAND) == 0) {
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (!payload) {
            ESP_LOGW(TAG, "Comando sin payload");
            cJSON_Delete(root);
            return;
        }
        
        cJSON *cmd = cJSON_GetObjectItem(payload, "cmd");
        if (cmd && cJSON_IsString(cmd)) {
            ESP_LOGI(TAG, "Comando recibido: %s", cmd->valuestring);
            
            // Proceso de comandos para LEDs
            if (strcmp(cmd->valuestring, "led_a") == 0) {
                process_led_command('A');
            } 
            else if (strcmp(cmd->valuestring, "led_b") == 0) {
                process_led_command('B');
            }
            else if (strcmp(cmd->valuestring, "led_c") == 0) {
                process_led_command('C');
            }
            
            // Envía una respuesta confirmando el comando
            cJSON *response = cJSON_CreateObject();
            cJSON_AddStringToObject(response, "cmd_received", cmd->valuestring);
            cJSON_AddBoolToObject(response, "success", true);
            
            publish_json_message(MQTT_TOPIC_DEVICE_RESPONSE, MQTT_MSG_TYPE_RESPONSE, response);
        }
    }
    
    cJSON_Delete(root);
}

esp_err_t mqtt_app_publish_status(const char* status) {
    return publish_json_status(status);
}

esp_err_t mqtt_app_publish_telemetry(cJSON *payload) {
    return publish_json_message(MQTT_TOPIC_DEVICE_TELEMETRY, 
                               MQTT_MSG_TYPE_TELEMETRY, 
                               payload);
}
