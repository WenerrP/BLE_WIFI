#ifndef MQTT_APP_H
#define MQTT_APP_H

#include <esp_err.h>
#include <stdbool.h>  // Para el tipo bool
#include "cJSON.h"

// Constantes exportadas para tipos de mensajes MQTT
#define MQTT_MSG_TYPE_COMMAND        "command"
#define MQTT_MSG_TYPE_STATUS         "status"
#define MQTT_MSG_TYPE_TELEMETRY      "telemetry"
#define MQTT_MSG_TYPE_RESPONSE       "response"
#define MQTT_MSG_TYPE_MED_CONFIRM    "med_confirmation"  // Nuevo tipo para confirmaciones de medicamentos

// Tópicos MQTT estándar
#define MQTT_TOPIC_DEVICE_COMMANDS   "/device/commands"
#define MQTT_TOPIC_DEVICE_STATUS     "/device/status" 
#define MQTT_TOPIC_DEVICE_TELEMETRY  "/device/telemetry"
#define MQTT_TOPIC_DEVICE_RESPONSE   "/device/response"
#define MQTT_TOPIC_MED_CONFIRMATION  "/device/med_confirmation"
#define MQTT_TOPIC_MEDICATION_TAKEN  "/device/medication_taken"

/**
 * @brief Inicia el módulo MQTT completo (conexión, suscripciones, etc.)
 */
void mqtt_app_init(void);

/**
 * @brief Detiene el módulo MQTT completo
 */
void mqtt_app_deinit(void);

/**
 * @brief Establece la dirección IP del dispositivo para informes de estado
 * 
 * @param ip Cadena con la dirección IP
 */
void mqtt_app_set_ip(const char* ip);

/**
 * @brief Verifica si el cliente MQTT está conectado al broker
 * 
 * @return true si está conectado, false en caso contrario
 */
bool mqtt_app_is_connected(void);

/**
 * @brief Publica un mensaje JSON de estado (wrapper para mqtt_pub_status)
 * 
 * @param status Cadena con el estado ("online", "offline", etc.)
 * @return esp_err_t ESP_OK si se publicó correctamente
 */
esp_err_t mqtt_app_publish_status(const char* status);

/**
 * @brief Publica un mensaje JSON de telemetría (wrapper para mqtt_pub_telemetry)
 * 
 * @param payload Objeto cJSON con los datos de telemetría
 * @return esp_err_t ESP_OK si se publicó correctamente
 */
esp_err_t mqtt_app_publish_telemetry(cJSON *payload);

/**
 * @brief Publica un mensaje en un tópico MQTT (wrapper para mqtt_pub_message)
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
 * @brief Suscribe al cliente a un tópico MQTT (wrapper para mqtt_sub_subscribe)
 * 
 * @param topic Tópico a suscribirse
 * @param qos Calidad de servicio (0, 1 o 2)
 * @return esp_err_t ESP_OK si la suscripción fue exitosa
 */
esp_err_t mqtt_app_subscribe(const char *topic, int qos);

/**
 * @brief Cancela la suscripción a un tópico MQTT (wrapper para mqtt_sub_unsubscribe)
 * 
 * @param topic Tópico a cancelar suscripción
 * @return esp_err_t ESP_OK si la operación fue exitosa
 */
esp_err_t mqtt_app_unsubscribe(const char *topic);

/**
 * @brief Obtiene el valor actual del LED activo
 * 
 * @return int Número del LED activo (0=ninguno, 1=A, 2=B, 3=C)
 */
int mqtt_app_get_active_led(void);

/**
 * @brief Establece el valor del LED activo
 * 
 * @param led_num Número del LED activo (0=ninguno, 1=A, 2=B, 3=C)
 */
void mqtt_app_set_active_led(int led_num);

/**
 * @brief Inicia MQTT desde app_main.c
 */
void mqtt_app_start(void);

// Callback para procesar comandos LED
void mqtt_app_process_led_command(char command);

/**
 * @brief Publica una confirmación sobre la recepción y procesamiento de medicamentos
 * 
 * @param success Indicador de éxito (true) o error (false)
 * @param message Mensaje descriptivo del resultado
 * @param timestamp Marca de tiempo de la solicitud original (0 para usar timestamp actual)
 * @return esp_err_t ESP_OK si la publicación fue exitosa
 */
esp_err_t mqtt_app_publish_med_confirmation(bool success, const char* message, int64_t timestamp);

#endif /* MQTT_APP_H */
