#ifndef MQTT_APP_H
#define MQTT_APP_H

#include <esp_err.h>

/**
 * @brief Inicia el cliente MQTT y establece la conexión con el broker
 */
void mqtt_app_start(void);

/**
 * @brief Verifica si el cliente MQTT está conectado al broker
 * 
 * @return true si está conectado, false en caso contrario
 */
bool mqtt_app_is_connected(void);

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
esp_err_t mqtt_app_publish(const char *topic, const char *data, int len, int qos, bool retain);

/**
 * @brief Suscribe al cliente a un tópico MQTT
 * 
 * @param topic Tópico a suscribirse
 * @param qos Calidad de servicio (0, 1 o 2)
 * @return esp_err_t ESP_OK si la suscripción fue exitosa
 */
esp_err_t mqtt_app_subscribe(const char *topic, int qos);

/**
 * @brief Cancela la suscripción a un tópico MQTT
 * 
 * @param topic Tópico a cancelar suscripción
 * @return esp_err_t ESP_OK si la operación fue exitosa
 */
esp_err_t mqtt_app_unsubscribe(const char *topic);

/**
 * @brief Detiene el cliente MQTT y libera recursos
 */
void mqtt_app_stop(void);

// Callback para procesar comandos LED
void process_led_command(char command);

/**
 * @brief Establece la dirección IP del dispositivo para informes de estado
 * 
 * @param ip Cadena con la dirección IP
 */
void mqtt_app_set_ip(const char* ip);

#endif /* MQTT_APP_H */
