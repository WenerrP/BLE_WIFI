#ifndef MEDICATION_DISPENSER_H
#define MEDICATION_DISPENSER_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Inicializa el sistema de dispensación de medicamentos
 * @return ESP_OK si se inicializó correctamente, error en caso contrario
 */
esp_err_t medication_dispenser_init(void);

/**
 * @brief Detiene el sistema de dispensación de medicamentos
 */
void medication_dispenser_deinit(void);

/**
 * @brief Habilita o deshabilita la dispensación automática
 * @param enable true para habilitar, false para deshabilitar
 */
void medication_dispenser_set_auto_dispense(bool enable);

/**
 * @brief Dispensa manualmente un medicamento específico
 * @param medication_id ID del medicamento a dispensar
 * @param schedule_id ID del horario específico
 * @return ESP_OK si se dispensó correctamente, error en caso contrario
 */
esp_err_t medication_dispenser_manual_dispense(const char* medication_id, const char* schedule_id);

// Añadir esta declaración junto con las demás
void check_missed_medications(void);

#endif /* MEDICATION_DISPENSER_H */