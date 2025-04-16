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
#define NEXTION_UART_NUM           UART_NUM_1    // Puerto UART a usar
#define NEXTION_UART_BAUD_RATE     9600          // Velocidad de comunicación
#define NEXTION_UART_TX_PIN        17            // GPIO para TX (ajustar según tu hardware)
#define NEXTION_UART_RX_PIN        16            // GPIO para RX (ajustar según tu hardware)
#define NEXTION_UART_BUFFER_SIZE   512           // Tamaño del buffer

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

#endif // NEXTION_DRIVER_H