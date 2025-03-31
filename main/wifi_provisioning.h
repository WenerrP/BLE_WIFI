#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include <esp_err.h>
#include <freertos/event_groups.h>

/* Define eventos del Wi-Fi */
#define WIFI_CONNECTED_EVENT BIT0

/**
 * @brief Inicializa el sistema de provisioning WiFi
 * 
 */
EventGroupHandle_t wifi_provisioning_init(void);

/**
 * @brief Inicia el Wi-Fi en modo estación después de un provisioning exitoso
 */
void wifi_init_sta(void);

/**
 * @brief Obtiene el nombre de servicio del dispositivo para provisioning
 * 
 * @param service_name Buffer para almacenar el nombre
 * @param max Tamaño máximo del buffer
 */
void get_device_service_name(char *service_name, size_t max);

/**
 * @brief Handler para el endpoint opcional de datos personalizados en provisioning
 */
esp_err_t custom_prov_data_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
                                  uint8_t **outbuf, ssize_t *outlen, void *priv_data);

/**
 * @brief Espera a que la conexión WiFi esté lista
 * 
 * Esta función bloquea hasta que el evento WIFI_CONNECTED_EVENT sea señalizado,
 * indicando que el WiFi está conectado y listo para usar.
 */
void wifi_provisioning_wait_for_connection(EventGroupHandle_t wifi_event_group);

/**
 * @brief Reinicia el estado de provisioning para permitir re-provisioning
 * 
 * Útil cuando se necesita re-configurar el WiFi o cuando hay problemas
 * con la conexión existente.
 */
void wifi_provisioning_reset_for_reprovision(void);

/**
 * @brief Registra una función de callback para notificación de conexión WiFi
 * 
 * Esta función permite al módulo principal registrar una función que será
 * llamada cuando se establezca la conexión WiFi y se obtenga la dirección IP.
 * 
 * @param callback Función a llamar cuando se conecte
 */
void wifi_provisioning_set_callback(void (*callback)(char *ip));

/**
 * @brief Callback para notificar fallos en la conexión WiFi
 * 
 * Esta función permite que el sistema principal sea notificado
 * cuando hay un fallo en la conexión WiFi.
 * 
 * @param callback Función a llamar cuando hay un fallo de conexión
 */
void wifi_provisioning_set_failure_callback(void (*callback)(void));

#endif /* WIFI_PROVISIONING_H */