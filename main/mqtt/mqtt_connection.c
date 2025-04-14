// Código para la conexión MQTT y manejo de eventos

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mqtt_manager_config.h"
#include "mqtt_app.h"          // Para usar las constantes de tópicos MQTT
#include "esp_log.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "cJSON.h"
#include "esp_wifi.h"
#include "esp_mac.h"           // Para ESP_MAC_WIFI_STA
#include "mqtt_connection.h"   // Incluir su propio encabezado
#include "mqtt_subscription.h" // Para process_json_command

static const char *TAG = "MQTT_CONNECTION";
static esp_mqtt_client_handle_t client = NULL;
static esp_timer_handle_t reconnect_timer = NULL;
static int mqtt_retry_count = 0;
static bool mqtt_connected = false;
static char device_ip[16] = "0.0.0.0"; // Default IP

// Declaración de la función publish_json_status que no estaba definida
static void publish_json_status(const char* status);

// Declaración de la función process_json_command que debería estar en mqtt_subscription.c
extern void process_json_command(const char* json_str);

// Declaraciones adelantadas para las funciones privadas
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

// Declaración de la función mqtt_send_medication_confirmation
void mqtt_send_medication_confirmation(const char* medication_id);
static void mqtt_reconnect_timer_callback(void* arg);
static uint32_t exponential_backoff(uint8_t retry_count);
static void handle_mqtt_error(esp_mqtt_event_handle_t event);
static void log_error_if_nonzero(const char *message, int error_code);
static char* generate_client_id(void);

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

// Implementación de la función publish_json_status
static void publish_json_status(const char* status) {
    if (!client || !mqtt_connected) {
        ESP_LOGW(TAG, "No se puede publicar estado, cliente no conectado");
        return;
    }
    
    char json_message[100];
    snprintf(json_message, sizeof(json_message), 
             "{\"type\":\"status\",\"status\":\"%s\",\"ip\":\"%s\"}", 
             status, device_ip);
    
    int msg_id = esp_mqtt_client_publish(client, MQTT_TOPIC_DEVICE_STATUS, 
                                        json_message, strlen(json_message), 1, true);
    
    if (msg_id >= 0) {
        ESP_LOGI(TAG, "Publicado estado '%s' con éxito", status);
    } else {
        ESP_LOGE(TAG, "Error al publicar estado '%s'", status);
    }
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
            
            // Publicar estado online con JSON inmediatamente al conectar
            cJSON *online_json = cJSON_CreateObject();
            cJSON_AddStringToObject(online_json, "type", MQTT_MSG_TYPE_STATUS);
            cJSON_AddStringToObject(online_json, "status", "online");
            cJSON_AddStringToObject(online_json, "ip", device_ip);
            cJSON_AddNumberToObject(online_json, "uptime", esp_timer_get_time() / 1000000);
            cJSON_AddNumberToObject(online_json, "free_heap", esp_get_free_heap_size());
            cJSON_AddNumberToObject(online_json, "active_led", mqtt_app_get_active_led());
            
            char *online_message = cJSON_Print(online_json);
            if (online_message) {
                esp_mqtt_client_publish(client, MQTT_TOPIC_DEVICE_STATUS, 
                                    online_message, 0, 1, true);
                free(online_message);
            }
            cJSON_Delete(online_json);
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

// Implementando las funciones públicas del API

// Función para verificar la conexión
bool mqtt_connect_is_connected(void) {
    return (client != NULL && mqtt_connected);
}

// Función para obtener el cliente MQTT
esp_mqtt_client_handle_t mqtt_connect_get_client(void) {
    return client;
}

// Inicialización del cliente MQTT
void mqtt_connect_init(void) {
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
    
    // Crear el mensaje LWT en formato JSON
    cJSON *lwt_json = cJSON_CreateObject();
    cJSON_AddStringToObject(lwt_json, "type", MQTT_MSG_TYPE_STATUS);
    cJSON_AddStringToObject(lwt_json, "status", "offline");
    cJSON_AddStringToObject(lwt_json, "ip", device_ip);
    cJSON_AddNumberToObject(lwt_json, "uptime", esp_timer_get_time() / 1000000);
    
    char *lwt_message = cJSON_Print(lwt_json);
    cJSON_Delete(lwt_json);
    
    if (!lwt_message) {
        ESP_LOGE(TAG, "Error creando mensaje LWT");
        free(client_id);
        return;
    }

    // Configurar el cliente MQTT con LWT
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://broker.emqx.io",
        .broker.address.port = 1883,
        .session.keepalive = 120,  // Reducir keepalive para detección más rápida
        .network = {
            .reconnect_timeout_ms = 10000,
            .timeout_ms = 10000,
        },
        .credentials.client_id = client_id,
        .credentials.username = NULL,
        .session.last_will.topic = MQTT_TOPIC_DEVICE_STATUS,
        .session.last_will.msg = lwt_message,
        .session.last_will.msg_len = strlen(lwt_message),
        .session.last_will.qos = 1,
        .session.last_will.retain = 1  // Importante: usar retain para que quede disponible
    };
    
    // Inicializar el cliente MQTT
    client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Error inicializando el cliente MQTT");
        free(client_id);
        free(lwt_message);
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
        free(lwt_message);
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
        free(lwt_message);
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
    free(lwt_message); // Liberamos la memoria del mensaje LWT
}

// Detener el cliente MQTT
void mqtt_connect_deinit(void) {
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
    
    // Publicar mensaje de desconexión explícito si estamos conectados
    if (mqtt_connected) {
        cJSON *offline_json = cJSON_CreateObject();
        cJSON_AddStringToObject(offline_json, "type", MQTT_MSG_TYPE_STATUS);
        cJSON_AddStringToObject(offline_json, "status", "offline");
        cJSON_AddStringToObject(offline_json, "ip", device_ip);
        cJSON_AddStringToObject(offline_json, "reason", "controlled_shutdown");
        
        char *offline_message = cJSON_Print(offline_json);
        if (offline_message) {
            esp_mqtt_client_publish(client, MQTT_TOPIC_DEVICE_STATUS, 
                                offline_message, 0, 1, true);
            free(offline_message);
        }
        cJSON_Delete(offline_json);
        
        // Pequeña pausa para asegurar que el mensaje se envíe
        vTaskDelay(100 / portTICK_PERIOD_MS);
        
        esp_mqtt_client_disconnect(client);
    }
    
    // Detener y destruir cliente MQTT
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = NULL;
    mqtt_connected = false;

    ESP_LOGI(TAG, "Cliente MQTT detenido y recursos liberados");
}

// Establecer la dirección IP
void mqtt_connect_set_ip(const char* ip) {
    if (ip) {
        strncpy(device_ip, ip, sizeof(device_ip) - 1);
        device_ip[sizeof(device_ip) - 1] = '\0'; // Garantizar terminación NULL
        ESP_LOGI(TAG, "IP actualizada: %s", device_ip);
    }
}

// Implementación de la función para registrar handlers de eventos adicionales
esp_err_t mqtt_connect_register_event_handler(esp_event_handler_t event_handler) {
    if (!client) {
        ESP_LOGE(TAG, "No se puede registrar el handler, cliente no inicializado");
        return ESP_ERR_INVALID_STATE;
    }
    
    return esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, event_handler, NULL);
}