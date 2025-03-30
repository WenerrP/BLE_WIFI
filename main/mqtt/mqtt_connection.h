#ifndef MQTT_CONNECTION_H
#define MQTT_CONNECTION_H

#include <esp_err.h>
#include <stdbool.h>        // Para el tipo bool
#include "mqtt_client.h"    // Para esp_mqtt_client_handle_t
#include "esp_event.h"      // Para esp_event_handler_t

// Constantes para la gestión de MQTT
#define MQTT_RECONNECT_TIMEOUT_MS 5000
#define MQTT_MAX_RETRY_COUNT 5
#define MQTT_NETWORK_TIMEOUT_MS 10000

/**
 * @brief Inicia el cliente MQTT y establece la conexión con el broker
 */
void mqtt_connect_init(void);

/**
 * @brief Detiene el cliente MQTT y libera recursos
 */
void mqtt_connect_deinit(void);

/**
 * @brief Verifica si el cliente MQTT está conectado al broker
 * 
 * @return true si está conectado, false en caso contrario
 */
bool mqtt_connect_is_connected(void);

/**
 * @brief Establece la dirección IP del dispositivo para informes de estado
 * 
 * @param ip Cadena con la dirección IP
 */
void mqtt_connect_set_ip(const char* ip);

/**
 * @brief Obtiene el cliente MQTT para que otros módulos puedan usarlo
 * 
 * @return Manejador del cliente MQTT
 */
esp_mqtt_client_handle_t mqtt_connect_get_client(void);

/**
 * @brief Registra un callback para manejar eventos MQTT
 * 
 * @param event_handler Función de callback
 * @return ESP_OK en caso de éxito
 */
esp_err_t mqtt_connect_register_event_handler(esp_event_handler_t event_handler);

#endif // MQTT_CONNECTION_H