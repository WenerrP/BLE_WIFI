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

static const char *TAG = "MED_DISPENSER";
static TaskHandle_t dispenser_task_handle = NULL;
static esp_timer_handle_t check_timer = NULL;
static bool dispenser_initialized = false;
static bool auto_dispense_enabled = true;

// Prototipo para la tarea de dispensación
static void medication_dispenser_task(void *pvParameters);
static void check_timer_callback(void* arg);
static void publish_med_notification(medication_t *medication, medication_schedule_t *schedule);

// Función para verificar si el tiempo está sincronizado correctamente
static bool is_time_reliable(void) {
    struct tm timeinfo;
    time_t now = 0;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // Si el año es menor a 2022, probable que NTP no esté sincronizado
    return (timeinfo.tm_year >= (2022 - 1900));
}

// Inicializa el sistema de dispensación de medicamentos
esp_err_t medication_dispenser_init(void) {
    if (dispenser_initialized) {
        ESP_LOGW(TAG, "El dispensador ya está inicializado");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Inicializando dispensador de medicamentos");

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
        return ret;
    }

    dispenser_initialized = true;
    auto_dispense_enabled = true;
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
    
    dispenser_initialized = false;
    ESP_LOGI(TAG, "Dispensador detenido");
}

// Habilita o deshabilita la dispensación automática
void medication_dispenser_set_auto_dispense(bool enable) {
    auto_dispense_enabled = enable;
    ESP_LOGI(TAG, "Dispensación automática %s", enable ? "habilitada" : "deshabilitada");
}

// Callback del timer para verificar medicamentos
static void check_timer_callback(void* arg) {
    ESP_LOGI(TAG, "Timer de verificación activado, notificando a la tarea del dispensador");
    // Enviar una notificación a la tarea para que verifique los medicamentos
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
            int64_t nearest_time = INT64_MAX;
            
            for (int i = 0; i < medication->schedules_count; i++) {
                medication_schedule_t *schedule = &medication->schedules[i];
                
                // Convertir a formato legible
                char next_time_str[32];
                char last_time_str[32];
                format_time(schedule->next_dispense_time, next_time_str, sizeof(next_time_str));
                format_time(schedule->last_dispensed_time, last_time_str, sizeof(last_time_str));
                
                ESP_LOGI(TAG, "  - Horario %s: próxima=%s, última=%s", 
                        schedule->id, next_time_str, last_time_str);
                
                // Verificar si este es el horario que acaba de ser marcado para dispensación
                if (schedule->next_dispense_time > schedule->last_dispensed_time &&
                    schedule->next_dispense_time <= current_time &&
                    schedule->next_dispense_time < nearest_time) {
                    active_schedule = schedule;
                    nearest_time = schedule->next_dispense_time;
                    ESP_LOGI(TAG, "    * Horario seleccionado para dispensación");
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