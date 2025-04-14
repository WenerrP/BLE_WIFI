#ifndef NTP_FUNC_H
#define NTP_FUNC_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/**
 * @brief Inicializa la conexión WiFi
 * 
 * @param ssid Nombre de la red WiFi
 * @param password Contraseña de la red WiFi
 */
void wifi_init(const char *ssid, const char *password);

/**
 * @brief Sincroniza la hora con servidores NTP
 * 
 * @param timezone Zona horaria (formato TZ, ej. "EST5EDT")
 * @return true si la sincronización fue exitosa
 * @return false si falló la sincronización
 */
bool sync_ntp_time(const char *timezone);

/**
 * @brief Obtiene el tiempo actual en milisegundos desde epoch
 * 
 * @return int64_t Timestamp en milisegundos
 */
int64_t get_time_ms(void);

/**
 * @brief Obtiene el tiempo actual en segundos desde epoch
 * 
 * @return time_t Timestamp en segundos
 */
time_t get_time_sec(void);

/**
 * @brief Formatea la hora actual según el formato especificado
 * 
 * @param buffer Buffer donde se almacenará el resultado
 * @param buffer_size Tamaño del buffer
 * @param format Formato de tiempo (como strftime)
 */
void format_current_time(char *buffer, size_t buffer_size, const char *format);

/**
 * @brief Formatea un timestamp en milisegundos a una cadena de texto legible
 * 
 * @param timestamp_ms Tiempo en milisegundos desde epoch
 * @param buffer Buffer donde se escribirá la cadena formateada
 * @param size Tamaño del buffer
 */
void format_time(int64_t timestamp_ms, char *buffer, size_t size);

/**
 * @brief Función de inicialización completa (WiFi + NTP)
 * 
 * @param ssid Nombre de la red WiFi
 * @param password Contraseña de la red WiFi
 * @param timezone Zona horaria (formato TZ)
 */
void ntp_init(const char *ssid, const char *password, const char *timezone);

#endif /* NTP_FUNC_H */