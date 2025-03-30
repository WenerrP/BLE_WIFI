#ifndef MQTT_SUBSCRIPTION_H
#define MQTT_SUBSCRIPTION_H

#include <esp_err.h>
#include <stdbool.h>  // Para el tipo bool

/**
 * @brief Procesa un comando JSON recibido
 * 
 * @param json_str Cadena JSON recibida
 */
void process_json_command(const char* json_str);

/**
 * @brief Suscribe al cliente a un tópico MQTT
 * 
 * @param topic Tópico a suscribirse
 * @param qos Calidad de servicio (0, 1 o 2)
 * @return esp_err_t ESP_OK si la suscripción fue exitosa
 */
esp_err_t mqtt_sub_subscribe(const char *topic, int qos);

/**
 * @brief Cancela la suscripción a un tópico MQTT
 * 
 * @param topic Tópico a cancelar suscripción
 * @return esp_err_t ESP_OK si la operación fue exitosa
 */
esp_err_t mqtt_sub_unsubscribe(const char *topic);

/**
 * @brief Inicializa las suscripciones a los tópicos necesarios
 * 
 * @return esp_err_t ESP_OK si la operación fue exitosa
 */
esp_err_t mqtt_sub_init(void);

/**
 * @brief Obtiene la dirección IP actual del dispositivo
 * 
 * @return Cadena con la dirección IP
 */
char* mqtt_sub_get_device_ip(void);

#endif // MQTT_SUBSCRIPTION_H