#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "medication_storage.h"
#include "medication_dispenser.h"
#include "../mqtt/mqtt_app.h"
#include "../ntp_func.h" // Para acceder a las funciones de tiempo NTP
#include "medication_hardware.h"  // Añadir esta línea al inicio
#include "buzzer_driver.h" // Añadir el include al principio

static const char *TAG = "MED_DISPENSER";
static TaskHandle_t dispenser_task_handle = NULL;
static esp_timer_handle_t check_timer = NULL;
static bool dispenser_initialized = false;
static bool auto_dispense_enabled = true;

// Prototipo para la tarea de dispensación
static void medication_dispenser_task(void *pvParameters);
static void check_timer_callback(void* arg);
static void publish_med_notification(medication_t *medication, medication_schedule_t *schedule);
void medication_reminder_callback(void *arg); // modificado de static a público
void schedule_medication_reminders(void); // nueva función para programar los recordatorios

// Añadir este prototipo al inicio junto con los otros
esp_err_t medication_dispenser_confirm_taken(const char* medication_id, const char* schedule_id);

// Añadir esta estructura para gestionar temporizadores de recordatorios
typedef struct {
    esp_timer_handle_t timer_handle;
    char medication_id[MEDICATION_ID_MAX_LEN];
    char schedule_id[MEDICATION_ID_MAX_LEN];
} reminder_timer_t;

// Arreglo para almacenar los temporizadores de recordatorios activos
#define MAX_REMINDER_TIMERS 10
static reminder_timer_t reminder_timers[MAX_REMINDER_TIMERS] = {0};
static int active_reminder_count = 0;

// Tiempo de anticipación para el recordatorio (en milisegundos)
#define REMINDER_ADVANCE_TIME (5 * 60 * 1000)  // 5 minutos antes

// Esta función programa recordatorios para todos los medicamentos
void schedule_medication_reminders(void) {
    ESP_LOGI(TAG, "Programando recordatorios para medicamentos");
    
    // Limpiar recordatorios anteriores
    for (int i = 0; i < active_reminder_count; i++) {
        if (reminder_timers[i].timer_handle != NULL) {
            esp_timer_stop(reminder_timers[i].timer_handle);
            esp_timer_delete(reminder_timers[i].timer_handle);
            reminder_timers[i].timer_handle = NULL;
        }
    }
    active_reminder_count = 0;
    
    // Obtener el tiempo actual
    int64_t current_time = get_time_ms();
    
    // Obtener todos los medicamentos
    int count;
    medication_t *meds = medication_storage_get_all_medications(&count);
    
    if (!meds || count == 0) {
        ESP_LOGI(TAG, "No hay medicamentos para programar recordatorios");
        return;
    }
    
    // Iterar sobre todos los medicamentos y sus horarios
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < meds[i].schedules_count; j++) {
            medication_schedule_t *schedule = &meds[i].schedules[j];
            
            // Verificar si el próximo tiempo de dispensación es en el futuro
            if (schedule->next_dispense_time > current_time) {
                // Calcular cuándo debe activarse el recordatorio (5 minutos antes)
                int64_t reminder_time = schedule->next_dispense_time - REMINDER_ADVANCE_TIME;
                
                // Si el tiempo ya pasó, programar para la próxima vez
                if (reminder_time <= current_time) {
                    ESP_LOGI(TAG, "El tiempo de recordatorio ya pasó, se programará para el siguiente ciclo");
                    continue;
                }
                
                // Comprobar si hay espacio para un nuevo recordatorio
                if (active_reminder_count >= MAX_REMINDER_TIMERS) {
                    ESP_LOGW(TAG, "Alcanzado el límite máximo de recordatorios");
                    break;
                }
                
                // Crear contexto para el recordatorio
                medication_schedule_t *reminder_ctx = malloc(sizeof(medication_schedule_t));
                if (!reminder_ctx) {
                    ESP_LOGE(TAG, "Error de memoria al crear contexto de recordatorio");
                    continue;
                }
                memcpy(reminder_ctx, schedule, sizeof(medication_schedule_t));
                
                // Crear temporizador para el recordatorio
                esp_timer_create_args_t timer_args = {
                    .callback = medication_reminder_callback,
                    .arg = reminder_ctx,
                    .name = "med_reminder"
                };
                
                esp_timer_handle_t timer_handle;
                esp_err_t ret = esp_timer_create(&timer_args, &timer_handle);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Error al crear temporizador para recordatorio: %s", esp_err_to_name(ret));
                    free(reminder_ctx);
                    continue;
                }
                
                // Calcular cuánto tiempo falta (en microsegundos)
                int64_t time_to_reminder_us = (reminder_time - current_time) * 1000;
                
                // Iniciar el temporizador
                ret = esp_timer_start_once(timer_handle, time_to_reminder_us);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Error al iniciar temporizador para recordatorio: %s", esp_err_to_name(ret));
                    esp_timer_delete(timer_handle);
                    free(reminder_ctx);
                    continue;
                }
                
                // Guardar información del temporizador
                reminder_timers[active_reminder_count].timer_handle = timer_handle;
                strncpy(reminder_timers[active_reminder_count].medication_id, meds[i].id, MEDICATION_ID_MAX_LEN-1);
                strncpy(reminder_timers[active_reminder_count].schedule_id, schedule->id, MEDICATION_ID_MAX_LEN-1);
                active_reminder_count++;
                
                char time_str[32];
                format_time(reminder_time, time_str, sizeof(time_str));
                ESP_LOGI(TAG, "Recordatorio programado para %s: %s (medicamento: %s)", 
                         time_str, schedule->id, meds[i].name);
            }
        }
    }
    
    ESP_LOGI(TAG, "Total de recordatorios programados: %d", active_reminder_count);
}

// Modificar el callback para que libere la memoria del contexto
void medication_reminder_callback(void *arg) {
    medication_schedule_t *schedule = (medication_schedule_t *)arg;
    
    // Necesitamos encontrar el medicamento al que pertenece este horario
    int count;
    medication_t *meds = medication_storage_get_all_medications(&count);
    const char *med_name = "desconocido"; // Valor por defecto
    
    // Buscar el medicamento que contiene este horario
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < meds[i].schedules_count; j++) {
            if (strcmp(meds[i].schedules[j].id, schedule->id) == 0) {
                med_name = meds[i].name;
                break;
            }
        }
    }
    
    ESP_LOGI(TAG, "⏰ RECORDATORIO DE MEDICAMENTO: %s (horario %s)", med_name, schedule->id);
    
    // Reproducir alerta de recordatorio
    buzzer_play_pattern(BUZZER_PATTERN_MEDICATION_READY);
    
    // Reproducir una segunda vez para asegurar que se escuche
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    buzzer_play_pattern(BUZZER_PATTERN_MEDICATION_READY);
    
    // Publicar notificación MQTT para recordatorio
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddStringToObject(root, "type", "medication_reminder");
        cJSON_AddStringToObject(root, "scheduleId", schedule->id);
        cJSON_AddStringToObject(root, "medicationName", med_name);
        cJSON_AddNumberToObject(root, "reminderTime", get_time_ms());
        cJSON_AddNumberToObject(root, "dispenseTime", schedule->next_dispense_time);
        
        char *json_str = cJSON_Print(root);
        if (json_str) {
            mqtt_app_publish(MQTT_TOPIC_DEVICE_TELEMETRY, json_str, 0, 1, false);
            free(json_str);
        }
        
        cJSON_Delete(root);
    }
    
    // Liberar la memoria del contexto
    free(arg);
}

// Función para verificar si el tiempo está sincronizado correctamente
static bool is_time_reliable(void) {
    struct tm timeinfo;
    time_t now = 0;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // Si el año es menor a 2022, probable que NTP no esté sincronizado
    return (timeinfo.tm_year >= (2022 - 1900));
}

// Añadir esta función para dispensar físicamente un medicamento
bool dispensar_medicamento_fisicamente(medication_t *medication) {
    if (!medication) {
        ESP_LOGE(TAG, "Medicamento inválido");
        return false;
    }
    
    ESP_LOGI(TAG, "Dispensando medicamento físicamente: %s (compartimento %d)",
             medication->name, medication->compartment);
    
    bool is_liquid = (strcmp(medication->type, COMPARTMENT_TYPE_LIQUID) == 0);
    bool success = false;
    
    // Comprobar que el compartimento es válido
    if (medication->compartment < 1 || 
        (is_liquid && medication->compartment != LIQUID_COMPARTMENT_NUM) || 
        (!is_liquid && medication->compartment > MAX_PILL_COMPARTMENTS)) {
        
        ESP_LOGE(TAG, "Compartimento inválido para tipo de medicamento: %d", medication->compartment);
        return false;
    }
    
    // Preparar parámetros para la dispensación
    uint32_t amount;
    if (is_liquid) {
        // Para líquidos, calculamos un tiempo en ms basado en dosis
        // Ej: 500ms por cada unidad de dosis
        amount = medication->pills_per_dose * 500;
        if (amount < 500) amount = 500;  // mínimo 500ms
        if (amount > 5000) amount = 5000;  // máximo 5 segundos
    } else {
        // Para píldoras, usamos la cantidad de píldoras por dosis
        amount = medication->pills_per_dose;
        if (amount < 1) amount = 1;  // mínimo 1 píldora
    }
    
    // Intentar dispensar el medicamento
    esp_err_t result = medication_hardware_dispense(medication->compartment, is_liquid, amount);
    
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "✅ Medicamento dispensado físicamente con éxito");
        success = true;
    } else {
        switch (result) {
            case ESP_ERR_INVALID_STATE:
                ESP_LOGW(TAG, "❌ No se detecta recipiente para recibir el medicamento");
                break;
            case ESP_ERR_INVALID_ARG:
                ESP_LOGE(TAG, "❌ Parámetros inválidos para dispensar");
                break;
            default:
                ESP_LOGE(TAG, "❌ Error al dispensar medicamento: %s", esp_err_to_name(result));
                break;
        }
    }
    
    return success;
}

// Inicializa el sistema de dispensación de medicamentos
esp_err_t medication_dispenser_init(void) {
    if (dispenser_initialized) {
        ESP_LOGW(TAG, "El dispensador ya está inicializado");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Inicializando dispensador de medicamentos");

    // Inicializar el hardware de dispensación
    esp_err_t hw_init = medication_hardware_init();
    if (hw_init != ESP_OK) {
        ESP_LOGE(TAG, "Error al inicializar hardware de dispensación: %s", esp_err_to_name(hw_init));
        return hw_init;
    }

    // Crear la tarea de dispensación
    BaseType_t task_created = xTaskCreate(
        medication_dispenser_task,
        "med_dispenser",
        4096,       // Stack size
        NULL,       // Parameters
        5,          // Priority
        &dispenser_task_handle);
        
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Error al crear la tarea del dispensador");
        medication_hardware_deinit();
        return ESP_FAIL;
    }

    // Configurar un timer para comprobar medicamentos periódicamente
    esp_timer_create_args_t timer_args = {
        .callback = &check_timer_callback,
        .name = "med_check_timer"
    };
    
    esp_err_t ret = esp_timer_create(&timer_args, &check_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error al crear el timer de comprobación: %s", esp_err_to_name(ret));
        vTaskDelete(dispenser_task_handle);
        dispenser_task_handle = NULL;
        medication_hardware_deinit();
        return ret;
    }
    
    // Iniciar el timer para verificar cada 30 segundos
    ret = esp_timer_start_periodic(check_timer, 30 * 1000000); // 30 segundos en microsegundos
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error al iniciar el timer: %s", esp_err_to_name(ret));
        esp_timer_delete(check_timer);
        check_timer = NULL;
        vTaskDelete(dispenser_task_handle);
        dispenser_task_handle = NULL;
        medication_hardware_deinit();
        return ret;
    }

    dispenser_initialized = true;
    auto_dispense_enabled = true;
    
    // Programar recordatorios iniciales
    schedule_medication_reminders();
    
    ESP_LOGI(TAG, "Dispensador inicializado correctamente");
    return ESP_OK;
}

// Detiene el sistema de dispensación
void medication_dispenser_deinit(void) {
    if (!dispenser_initialized) {
        return;
    }
    
    // Detener y eliminar el timer
    if (check_timer != NULL) {
        esp_timer_stop(check_timer);
        esp_timer_delete(check_timer);
        check_timer = NULL;
    }
    
    // Detener la tarea
    if (dispenser_task_handle != NULL) {
        vTaskDelete(dispenser_task_handle);
        dispenser_task_handle = NULL;
    }
    
    // Deinicializar el hardware
    medication_hardware_deinit();
    
    dispenser_initialized = false;
    ESP_LOGI(TAG, "Dispensador detenido");
}

// Habilita o deshabilita la dispensación automática
void medication_dispenser_set_auto_dispense(bool enable) {
    auto_dispense_enabled = enable;
    ESP_LOGI(TAG, "Dispensación automática %s", enable ? "habilitada" : "deshabilitada");
}

// Modificar el callback del timer para que también verifique medicamentos perdidos
static void check_timer_callback(void* arg) {
    ESP_LOGI(TAG, "Timer de verificación activado");
    
    // Verificar medicamentos no tomados
    static int missed_counter = 0;
    missed_counter++;
    
    // Verificar medicamentos perdidos cada 5 minutos (10 ciclos de 30 segundos)
    if (missed_counter >= 10) {
        ESP_LOGI(TAG, "Verificando medicamentos perdidos...");
        check_missed_medications();
        missed_counter = 0;
    }
    
    // Reprogramar recordatorios (cada 10 minutos - 20 ciclos de 30 segundos)
    static int reminder_counter = 0;
    reminder_counter++;
    if (reminder_counter >= 20) {
        ESP_LOGI(TAG, "Reprogramando recordatorios...");
        schedule_medication_reminders();
        reminder_counter = 0;
    }
    
    // Notificar a la tarea para que verifique los medicamentos a dispensar
    if (dispenser_task_handle != NULL) {
        xTaskNotifyGive(dispenser_task_handle);
    } else {
        ESP_LOGW(TAG, "La tarea del dispensador no está disponible");
    }
}

// Publica una notificación de medicamento a dispensar vía MQTT
static void publish_med_notification(medication_t *medication, medication_schedule_t *schedule) {
    if (!medication || !schedule) {
        return;
    }
    
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "Error creando JSON para notificación de medicamento");
        return;
    }
    
    // Datos básicos del mensaje
    cJSON_AddStringToObject(root, "type", "medication_alert");
    cJSON_AddNumberToObject(root, "timestamp", get_time_ms()); // Usar la función del módulo NTP
    
    // Datos del medicamento
    cJSON *med_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(med_obj, "id", medication->id);
    cJSON_AddStringToObject(med_obj, "name", medication->name);
    cJSON_AddNumberToObject(med_obj, "compartment", medication->compartment);
    cJSON_AddStringToObject(med_obj, "type", medication->type);
    
    if (strcmp(medication->type, "pill") == 0) {
        cJSON_AddNumberToObject(med_obj, "pillsPerDose", medication->pills_per_dose);
        cJSON_AddNumberToObject(med_obj, "remainingPills", medication->total_pills);
    }
    
    // Datos del horario
    cJSON *sched_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(sched_obj, "id", schedule->id);
    cJSON_AddNumberToObject(sched_obj, "timeInMinutes", schedule->time_in_minutes);
    
    // Añadir objetos al mensaje principal
    cJSON_AddItemToObject(root, "medication", med_obj);
    cJSON_AddItemToObject(root, "schedule", sched_obj);
    
    // Convertir a string y publicar
    char *json_str = cJSON_Print(root);
    if (json_str) {
        // Usamos el tópico de telemetría para enviar la notificación
        mqtt_app_publish(MQTT_TOPIC_DEVICE_TELEMETRY, json_str, 0, 1, false);
        free(json_str);
    }
    
    cJSON_Delete(root);
}

// Dispensar un medicamento manualmente
esp_err_t medication_dispenser_manual_dispense(const char* medication_id, const char* schedule_id) {
    if (!medication_id || !schedule_id) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Obtener el medicamento
    medication_t *med = medication_storage_get_medication(medication_id);
    if (!med) {
        ESP_LOGW(TAG, "Medicamento no encontrado: %s", medication_id);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Buscar el horario
    medication_schedule_t *schedule = NULL;
    for (int i = 0; i < med->schedules_count; i++) {
        if (strcmp(med->schedules[i].id, schedule_id) == 0) {
            schedule = &med->schedules[i];
            break;
        }
    }
    
    if (!schedule) {
        ESP_LOGW(TAG, "Horario no encontrado para medicamento %s: %s", 
                medication_id, schedule_id);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Dispensar físicamente el medicamento
    bool dispensed = dispensar_medicamento_fisicamente(med);
    if (!dispensed) {
        ESP_LOGW(TAG, "Error en dispensación física del medicamento %s", med->name);
        // Opcionalmente puedes decidir no continuar, pero aquí continuamos para actualizar el estado
        // return ESP_FAIL;
    }
    
    // Marcar como dispensado en el almacenamiento
    esp_err_t ret = medication_storage_mark_dispensed(medication_id, schedule_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error al marcar medicamento como dispensado: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Publicar confirmación
    publish_med_notification(med, schedule);
    
    ESP_LOGI(TAG, "Medicamento %s dispensado manualmente", med->name);
    return ESP_OK;
}

// Implementar la función 
esp_err_t medication_dispenser_confirm_taken(const char* medication_id, const char* schedule_id) {
    if (!medication_id || !schedule_id) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Recibida confirmación de medicamento tomado: %s, horario: %s", 
             medication_id, schedule_id);
    
    // Obtener el medicamento
    medication_t *med = medication_storage_get_medication(medication_id);
    if (!med) {
        ESP_LOGW(TAG, "Medicamento no encontrado: %s", medication_id);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Buscar el horario específico
    medication_schedule_t *schedule = NULL;
    for (int i = 0; i < med->schedules_count; i++) {
        if (strcmp(med->schedules[i].id, schedule_id) == 0) {
            schedule = &med->schedules[i];
            break;
        }
    }
    
    if (!schedule) {
        ESP_LOGW(TAG, "Horario no encontrado: %s", schedule_id);
        return ESP_ERR_NOT_FOUND;
    }
    
    int64_t current_time = get_time_ms();
    
    // Solo actualizamos si el medicamento ya fue dispensado
    if (schedule->last_dispensed_time >= schedule->next_dispense_time) {
        // Publicar confirmación MQTT
        cJSON *root = cJSON_CreateObject();
        if (root) {
            cJSON_AddStringToObject(root, "type", "medication_taken_confirmed");
            cJSON_AddStringToObject(root, "medicationId", medication_id);
            cJSON_AddStringToObject(root, "name", med->name);
            cJSON_AddStringToObject(root, "scheduleId", schedule_id);
            cJSON_AddNumberToObject(root, "timestamp", current_time);
            
            char *json_str = cJSON_Print(root);
            if (json_str) {
                mqtt_app_publish(MQTT_TOPIC_DEVICE_TELEMETRY, json_str, 0, 1, false);
                free(json_str);
            }
            cJSON_Delete(root);
        }
        
        // Actualizar el campo last_taken_time
        schedule->last_taken_time = current_time;
        
        // Guardar cambios en almacenamiento
        esp_err_t ret = medication_storage_save();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Error al guardar confirmación: %s", esp_err_to_name(ret));
            return ret;
        }
        
        ESP_LOGI(TAG, "✅ Confirmación de medicamento tomado registrada: %s", med->name);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "⚠️ El medicamento %s no ha sido dispensado todavía", med->name);
        return ESP_ERR_INVALID_STATE;
    }
}

// Tarea principal del dispensador
static void medication_dispenser_task(void *pvParameters) {
    ESP_LOGI(TAG, "Tarea del dispensador iniciada");
    
    while (1) {
        // Verificar que el tiempo esté sincronizado correctamente
        if (!is_time_reliable()) {
            ESP_LOGW(TAG, "Tiempo no sincronizado correctamente, esperando...");
            vTaskDelay(30000 / portTICK_PERIOD_MS); // Esperar 30 segundos
            continue;
        }
        
        // Log para saber que estamos esperando notificación
        ESP_LOGI(TAG, "Esperando notificación del timer o timeout...");
        
        // Esperar notificación del timer o timeout
        uint32_t notification_value = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60000)); // 1 minuto máximo
        
        if (notification_value > 0) {
            ESP_LOGI(TAG, "Notificación recibida, verificando medicamentos...");
        } else {
            ESP_LOGI(TAG, "Timeout alcanzado, verificando medicamentos de todas formas");
        }
        
        // Obtener si hay medicamentos para optimizar el tiempo de espera
        int count;
        medication_t *meds = medication_storage_get_all_medications(&count);
        
        ESP_LOGI(TAG, "Total de medicamentos encontrados: %d", count);
        
        if (count == 0) {
            // No hay medicamentos, esperar más tiempo para ahorrar energía
            ESP_LOGI(TAG, "No hay medicamentos programados, durmiendo más tiempo");
            vTaskDelay(pdMS_TO_TICKS(300000)); // 5 minutos
            continue;
        }
        
        // Mostrar información de todos los medicamentos y sus horarios
        for (int i = 0; i < count; i++) {
            ESP_LOGI(TAG, "Medicamento %d: %s (compartimento %d)", i+1, meds[i].name, meds[i].compartment);
            
            for (int j = 0; j < meds[i].schedules_count; j++) {
                char time_str[32];
                format_time(meds[i].schedules[j].next_dispense_time, time_str, sizeof(time_str));
                ESP_LOGI(TAG, "  - Horario %s: próxima dispensación en %s", 
                         meds[i].schedules[j].id, time_str);
            }
        }
        
        // Obtener el tiempo actual
        int64_t current_time = get_time_ms(); // Usar la función del módulo NTP
        
        // Convertir a formato legible y mostrar
        char current_time_str[32];
        format_time(current_time, current_time_str, sizeof(current_time_str));
        ESP_LOGI(TAG, "Tiempo actual: %s", current_time_str);
        
        // Verificar si hay medicamentos para dispensar
        ESP_LOGI(TAG, "Verificando medicamentos para dispensar...");
        medication_t *medication = medication_storage_check_dispense(current_time);
        
        if (medication != NULL) {
            // Encontramos un medicamento para dispensar
            ESP_LOGI(TAG, "¡Medicamento listo para dispensar: %s (compartimento %d)!",
                    medication->name, medication->compartment);
            
            // Buscar el horario correspondiente (el más cercano a dispensar)
            medication_schedule_t *active_schedule = NULL;

            for (int i = 0; i < medication->schedules_count; i++) {
                medication_schedule_t *schedule = &medication->schedules[i];
                
                // Convertir a formato legible
                char next_time_str[32];
                char last_time_str[32];
                format_time(schedule->next_dispense_time, next_time_str, sizeof(next_time_str));
                format_time(schedule->last_dispensed_time, last_time_str, sizeof(last_time_str));
                
                ESP_LOGI(TAG, "  - Horario %s: próxima=%s, última=%s", 
                        schedule->id, next_time_str, last_time_str);
                
                // Verificar si este horario está listo para dispensar
                // La condición principal es que la última dispensación sea anterior a la próxima programada
                if (schedule->next_dispense_time > schedule->last_dispensed_time) {
                    active_schedule = schedule;
                    ESP_LOGI(TAG, "    * Horario seleccionado para dispensación");
                    break; // Use the first schedule that matches
                }
            }
            
            if (active_schedule) {
                ESP_LOGI(TAG, "Preparando notificación para medicamento %s (horario %s)",
                        medication->name, active_schedule->id);
                
                // Enviar notificación MQTT
                publish_med_notification(medication, active_schedule);
                
                // Si está habilitada la dispensación automática, marcar como dispensado
                if (auto_dispense_enabled) {
                    ESP_LOGI(TAG, "Dispensando automáticamente medicamento: %s", medication->name);
                    
                    // Añadir esta sección para dispensar físicamente:
                    bool dispensed = dispensar_medicamento_fisicamente(medication);
                    if (!dispensed) {
                        ESP_LOGW(TAG, "❌ Error en dispensación física del medicamento");
                        // Opcionalmente, puedes decidir no marcar como dispensado si falla la dispensación física
                    } else {
                        ESP_LOGI(TAG, "✅ Medicamento dispensado físicamente con éxito");
                    }
                    
                    esp_err_t result = medication_storage_mark_dispensed(medication->id, active_schedule->id);
                    
                    if (result == ESP_OK) {
                        ESP_LOGI(TAG, "✅ Medicamento dispensado correctamente");
                    } else {
                        ESP_LOGW(TAG, "❌ Error al marcar medicamento como dispensado: %s", esp_err_to_name(result));
                    }
                } else {
                    ESP_LOGW(TAG, "⚠️ Dispensación automática desactivada, esperando confirmación manual");
                }
            } else {
                ESP_LOGW(TAG, "No se encontró ningún horario activo para dispensar");
            }
        } else {
            ESP_LOGI(TAG, "No hay medicamentos listos para dispensar en este momento");
        }
        
        ESP_LOGI(TAG, "Ciclo de verificación completado, esperando próxima notificación");
    }
}

// Función que verifica medicamentos perdidos/no tomados
void check_missed_medications(void) {
    ESP_LOGI(TAG, "Verificando medicamentos no tomados...");
    
    // Obtener el tiempo actual
    int64_t current_time = get_time_ms();
    
    // Obtener todos los medicamentos
    int count;
    medication_t *meds = medication_storage_get_all_medications(&count);
    
    if (!meds || count == 0) {
        ESP_LOGI(TAG, "No hay medicamentos para verificar");
        return;
    }
    
    // Iterar sobre todos los medicamentos y sus horarios
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < meds[i].schedules_count; j++) {
            medication_schedule_t *schedule = &meds[i].schedules[j];
            
            // Tiempo después del cual consideramos que un medicamento está "perdido" (30 minutos)
            int64_t threshold_time = 30 * 60 * 1000; // 30 minutos en ms
            
            bool should_have_been_dispensed = (schedule->next_dispense_time < current_time - threshold_time);
            bool was_dispensed = (schedule->last_dispensed_time >= schedule->next_dispense_time);
            bool was_taken = (schedule->last_taken_time >= schedule->last_dispensed_time);
            
            // Un medicamento se considera 'never_dispensed' si debió dispensarse pero no se hizo
            bool never_dispensed = should_have_been_dispensed && !was_dispensed;
            
            // Un medicamento se considera 'dispensed_not_taken' si fue dispensado pero no tomado
            bool dispensed_not_taken = should_have_been_dispensed && was_dispensed && !was_taken;
            
            // Si alguna de las condiciones se cumple, hay un problema que reportar
            if (never_dispensed || dispensed_not_taken) {
                const char* status = never_dispensed ? "never_dispensed" : "dispensed_not_taken";
                
                ESP_LOGW(TAG, "¡Medicamento no tomado detectado! %s, horario %s (%s)", 
                         meds[i].name, schedule->id, status);
                
                // Convertir a formato legible
                char next_time_str[32];
                format_time(schedule->next_dispense_time, next_time_str, sizeof(next_time_str));
                
                ESP_LOGW(TAG, "  - Programado para: %s (hace %lld minutos)", 
                         next_time_str, (current_time - schedule->next_dispense_time) / 60000);
                
                // Generar alerta sonora
                medication_hardware_alert_missed();
                
                // Publicar notificación MQTT de medicamento perdido
                cJSON *root = cJSON_CreateObject();
                if (root) {
                    cJSON_AddStringToObject(root, "type", "medication_missed");
                    cJSON_AddStringToObject(root, "medicationId", meds[i].id);
                    cJSON_AddStringToObject(root, "name", meds[i].name);
                    cJSON_AddStringToObject(root, "scheduleId", schedule->id);
                    cJSON_AddStringToObject(root, "status", status);
                    cJSON_AddNumberToObject(root, "scheduledTime", schedule->next_dispense_time);
                    cJSON_AddNumberToObject(root, "currentTime", current_time);
                    
                    // Solo añadir estos datos si es relevante
                    if (dispensed_not_taken) {
                        cJSON_AddNumberToObject(root, "dispensedTime", schedule->last_dispensed_time);
                    }
                    
                    char *json_str = cJSON_Print(root);
                    if (json_str) {
                        mqtt_app_publish(MQTT_TOPIC_DEVICE_TELEMETRY, json_str, 0, 1, false);
                        free(json_str);
                    }
                    
                    cJSON_Delete(root);
                }
            }
        }
    }
}