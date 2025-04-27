#ifndef NEXTION_DRIVER_H
#define NEXTION_DRIVER_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_log.h"

// Definiciones para la comunicación con Nextion
#define NEXTION_UART_NUM           UART_NUM_2    // Puerto UART a usar
#define NEXTION_UART_BAUD_RATE     9600          // Velocidad de comunicación
#define NEXTION_UART_TX_PIN        17            // GPIO para TX (ajustar según tu hardware)
#define NEXTION_UART_RX_PIN        16            // GPIO para RX (ajustar según tu hardware)
#define NEXTION_UART_BUFFER_SIZE   1024           // Tamaño del buffer

// Comandos terminadores para Nextion
#define NEXTION_CMD_END            "\xFF\xFF\xFF"

// Estructura para almacenar datos de tiempo seleccionados en Nextion
typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    bool valid;
} nextion_time_data_t;

// Función para inicializar la comunicación con Nextion
bool nextion_init(void);

// Enviar comando simple a Nextion
bool nextion_send_cmd(const char *cmd);

// Actualizar valor de un componente
bool nextion_set_component_value(const char *component, const char *value);
bool nextion_set_component_value_int(const char *component, int value);
bool nextion_time_updater_start(const char *username);

// Cambiar a una página específica
bool nextion_goto_page(const char *page);

// Funciones relacionadas con fecha/hora
bool nextion_request_time_setup(void);
bool nextion_process_received_data(uint8_t *data, size_t len);
nextion_time_data_t nextion_get_last_time_data(void);

// Actualizar la visualización de fecha/hora en Nextion
bool nextion_update_time_display(void);

// Función para iniciar tarea de recepción de datos de Nextion
void nextion_start_rx_task(void);

// Para integrarse con el módulo NTP existente
void nextion_set_ntp_status(bool success);

// Añadir estas declaraciones

/**
 * @brief Configura la prioridad de actualización de la pantalla
 * 
 * @param priority 0=mínima (solo cambios de minuto), 1=media (segundos), 2=máxima (todo)
 */
void nextion_set_update_priority(uint8_t priority);

/**
 * @brief Activa/desactiva el modo de bajo consumo
 * 
 * @param enable true para modo bajo consumo, false para normal
 */
void nextion_set_low_power_mode(bool enable);

/**
 * @brief Establece el intervalo de actualización en milisegundos
 * 
 * @param interval_ms Tiempo entre actualizaciones (100-60000 ms)
 */
void nextion_set_update_interval(uint32_t interval_ms);

#endif // NEXTION_DRIVER_H