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

static const char *TAG = "MQTT_APP";
static esp_mqtt_client_handle_t client = NULL;
static esp_timer_handle_t reconnect_timer = NULL;
static int mqtt_retry_count = 0;
static bool mqtt_connected = false;

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
            mqtt_connected = true;  // Set flag to true
            
            // Suscribirnos a los tópicos relevantes
            esp_mqtt_client_subscribe(client, "/led/command", 0);
            
            // Publicar mensaje de conexión
            esp_mqtt_client_publish(client, "/test/topic", "ESP32 conectado!", 0, 1, 0);
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
            
            // Procesar comandos para los LEDs si el mensaje es un solo carácter
            if (event->data_len == 1) {
                process_led_command(event->data[0]);
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
    
    // Configurar el cliente MQTT
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://broker.emqx.io",
        .broker.address.port = 1883,
        .session.keepalive = 120,
        .session.disable_clean_session = false,
        .network.timeout_ms = MQTT_NETWORK_TIMEOUT_MS,
        .network.disable_auto_reconnect = true, // Manejamos la reconexión manualmente
        .credentials.client_id = client_id,
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
        esp_mqtt_client_publish(client, "/test/topic", "ESP32 desconectado", 0, 1, 0);
        esp_mqtt_client_disconnect(client);
    }
    
    // Detener y destruir cliente MQTT
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = NULL;
    
    ESP_LOGI(TAG, "Cliente MQTT detenido y recursos liberados");
}
