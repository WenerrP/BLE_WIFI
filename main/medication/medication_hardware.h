#ifndef MEDICATION_HARDWARE_H
#define MEDICATION_HARDWARE_H

#include <stdbool.h>
#include "esp_err.h"

// Definiciones de tipos de compartimentos
#define COMPARTMENT_TYPE_PILL    "pill"
#define COMPARTMENT_TYPE_LIQUID  "liquid"
#define MAX_PILL_COMPARTMENTS     3
#define LIQUID_COMPARTMENT_NUM    4

// Estado del sensor
typedef enum {
    SENSOR_ERROR = -1,
    OBJECT_NOT_PRESENT = 0,
    OBJECT_PRESENT = 1
} sensor_state_t;

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
 * @brief Verifica si hay un recipiente para píldoras usando el sensor ultrasónico
 * @return OBJECT_PRESENT si se detecta un objeto, OBJECT_NOT_PRESENT si no hay objeto,
 *         SENSOR_ERROR en caso de error de lectura
 */
sensor_state_t medication_hardware_check_pill_presence(void);

/**
 * @brief Verifica si hay un recipiente para líquido debajo del dispensador
 * @return OBJECT_PRESENT si se detecta un objeto, OBJECT_NOT_PRESENT si no hay objeto,
 *         SENSOR_ERROR en caso de error de lectura
 */
sensor_state_t medication_hardware_check_liquid_presence(void);

/**
 * @brief Espera la presencia de un recipiente con alertas periódicas
 * @param is_liquid Indica si se espera un recipiente para líquido (true) o píldoras (false)
 * @param max_wait_time_ms Tiempo máximo de espera en milisegundos
 * @return OBJECT_PRESENT si se detecta el recipiente, OBJECT_NOT_PRESENT si se agota el tiempo de espera
 */
sensor_state_t wait_for_container_with_alerts(bool is_liquid, uint32_t max_wait_time_ms);

/**
 * @brief Libera los recursos del hardware
 */
void medication_hardware_deinit(void);

/**
 * @brief Dispensa un medicamento específico según el tipo y número de compartimento
 * @param compartment_number Número de compartimento (1-4, donde 4 es el compartimento de líquido)
 * @param is_liquid Indica si es un medicamento líquido (true) o una píldora (false)
 * @param amount Para píldoras: número de píldoras; Para líquidos: duración en ms
 * @return ESP_OK si se dispensó correctamente
 */
esp_err_t medication_hardware_dispense(uint8_t compartment_number, bool is_liquid, uint32_t amount);

// Añadir esta declaración junto con las demás
esp_err_t medication_hardware_alert_missed(void);

/**
 * @brief Ejecuta un diagnóstico completo de los servomotores
 * @return ESP_OK si el diagnóstico se ejecutó correctamente
 */
esp_err_t medication_hardware_servo_diagnostic(void);

#endif /* MEDICATION_HARDWARE_H */