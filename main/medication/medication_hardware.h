#ifndef MEDICATION_HARDWARE_H
#define MEDICATION_HARDWARE_H

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Inicializa el hardware del dispensador de medicamentos
 * @return ESP_OK si se inicializó correctamente
 */
esp_err_t medication_hardware_init(void);

/**
 * @brief Abre un compartimento específico para dispensar píldoras
 * @param compartment_number Número de compartimento (1-3)
 * @return ESP_OK si se abrió correctamente
 */
esp_err_t medication_hardware_open_compartment(uint8_t compartment_number);

/**
 * @brief Cierra un compartimento específico
 * @param compartment_number Número de compartimento (1-3)
 * @return ESP_OK si se cerró correctamente
 */
esp_err_t medication_hardware_close_compartment(uint8_t compartment_number);

/**
 * @brief Activa la bomba para dispensar medicamento líquido
 * @param duty_percent Porcentaje de duty cycle (0-100)
 * @param duration_ms Duración de la activación en milisegundos (0 para activar sin límite)
 * @return ESP_OK si se activó correctamente
 */
esp_err_t medication_hardware_pump_start(uint8_t duty_percent, uint32_t duration_ms);

/**
 * @brief Detiene la bomba de medicamento líquido
 * @return ESP_OK si se detuvo correctamente
 */
esp_err_t medication_hardware_pump_stop(void);

/**
 * @brief Verifica si hay una píldora en el dispensador usando el sensor ultrasónico
 * @return true si se detecta un objeto, false en caso contrario
 */
bool medication_hardware_check_pill_presence(void);

/**
 * @brief Verifica si hay un recipiente para líquido debajo del dispensador
 * @return true si se detecta un objeto, false en caso contrario
 */
bool medication_hardware_check_liquid_presence(void);

/**
 * @brief Libera los recursos del hardware
 */
void medication_hardware_deinit(void);

#endif /* MEDICATION_HARDWARE_H */