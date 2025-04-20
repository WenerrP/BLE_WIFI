#ifndef MEDICATION_STORAGE_H
#define MEDICATION_STORAGE_H

#include <esp_err.h>
#include "cJSON.h"

#define MEDICATION_ID_MAX_LEN 64
/**
 * @brief Estructura para representar un horario de medicamento
 */
typedef struct {
    char id[32];
    uint16_t time_in_minutes;     // Tiempo en minutos desde el inicio del día (0-1439)
    bool interval_mode;           // Si es true, usar intervalos de horas en vez de días fijos
    uint8_t interval_hours;       // Horas entre dosis (modo intervalo)
    uint8_t treatment_days;       // Días totales de tratamiento (modo intervalo)
    uint8_t days_count;           // Número de días seleccionados (0-7)
    uint8_t days[7];              // Días de la semana: 1=lunes, 7=domingo
    uint8_t _padding;             // Para alineación
    int64_t treatment_end_date;   // Fecha fin del tratamiento (timestamp en ms)
    int64_t next_dispense_time;   // Próxima dispensación programada (timestamp en ms)
    int64_t last_dispensed_time;  // Última dispensación (timestamp en ms)
    int64_t last_taken_time;       // Última vez que se tomó el medicamento (timestamp en ms)
} medication_schedule_t;

/**
 * @brief Estructura para representar un medicamento
 */
typedef struct {
    char id[32];
    char name[64];
    int compartment;             // 1-4
    char type[16];               // "pill" o "liquid"
    int pills_per_dose;          // Pastillas por dosis
    int total_pills;             // Total de pastillas restantes
    medication_schedule_t *schedules; // Arreglo de horarios
    int schedules_count;         // Número de horarios
} medication_t;

/**
 * @brief Inicializa el sistema de almacenamiento de medicamentos
 * 
 * @return esp_err_t ESP_OK si se inicializó correctamente
 */
esp_err_t medication_storage_init(void);

/**
 * @brief Procesa un mensaje JSON con datos de medicamentos
 * 
 * @param json_str Cadena JSON con la información de medicamentos
 * @return esp_err_t ESP_OK si se procesó correctamente
 */
esp_err_t medication_storage_process_json(const char* json_str);

/**
 * @brief Obtiene un medicamento por su ID
 * 
 * @param med_id ID del medicamento a buscar
 * @return medication_t* Puntero al medicamento o NULL si no existe
 */
medication_t* medication_storage_get_medication(const char* med_id);

/**
 * @brief Obtiene la lista de todos los medicamentos
 * 
 * @param count Puntero para almacenar el número de medicamentos
 * @return medication_t* Arreglo de medicamentos
 */
medication_t* medication_storage_get_all_medications(int* count);

/**
 * @brief Calcula la próxima dispensación para todos los medicamentos
 */
void medication_storage_update_next_dispense_times(void);

/**
 * @brief Verifica si hay medicamentos que deben dispensarse
 * 
 * @param current_time Tiempo actual en milisegundos
 * @return medication_t* Medicamento que debe dispensarse, NULL si no hay ninguno
 */
medication_t* medication_storage_check_dispense(int64_t current_time);

/**
 * @brief Marca un medicamento como dispensado
 * 
 * @param med_id ID del medicamento
 * @param schedule_id ID del horario
 * @return esp_err_t ESP_OK si se actualizó correctamente
 */
esp_err_t medication_storage_mark_dispensed(const char* med_id, const char* schedule_id);

/**
 * @brief Guarda el estado actual del almacenamiento de medicamentos
 * 
 * @return esp_err_t ESP_OK si se guardó correctamente
 */
esp_err_t medication_storage_save(void);

#endif // MEDICATION_STORAGE_H