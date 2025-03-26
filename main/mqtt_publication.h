#ifndef MQTT_PUBLICATION_H
#define MQTT_PUBLICATION_H

#include <esp_err.h>
#include "cJSON.h"
#include <stdbool.h>

/**
 * @brief Publica un mensaje JSON de estado
 * 
 * @param status Cadena con el estado ("online", "offline", etc.)
 * @return esp_err_t ESP_OK si se publicó correctamente
 */
esp_err_t mqtt_pub_status(const char* status);

/**
 * @brief Publica un mensaje JSON de telemetría
 * 
 * @param payload Objeto cJSON con los datos de telemetría
 * @return esp_err_t ESP_OK si se publicó correctamente
 */
esp_err_t mqtt_pub_telemetry(cJSON *payload);

/**
 * @brief Publica un mensaje en un tópico MQTT
 * 
 * @param topic Tópico donde publicar
 * @param data Datos a publicar
 * @param len Longitud de los datos
 * @param qos Calidad de servicio (0, 1 o 2)
 * @param retain Bandera para retener el mensaje
 * @return esp_err_t ESP_OK si se publicó correctamente
 */
esp_err_t mqtt_pub_message(const char *topic, const char *data, int len, int qos, bool retain);

/**
 * @brief Publica un mensaje JSON genérico
 * 
 * @param topic Tópico donde publicar
 * @param type Tipo de mensaje
 * @param payload Objeto cJSON con el payload
 * @return esp_err_t ESP_OK si se publicó correctamente
 */
esp_err_t mqtt_pub_json_message(const char* topic, const char* type, cJSON *payload);

/**
 * @brief Establece la dirección IP del dispositivo para los mensajes de estado
 * 
 * @param ip Cadena con la dirección IP
 */
void mqtt_pub_set_ip(const char* ip);

#endif // MQTT_PUBLICATION_H