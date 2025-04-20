#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_system.h"
#include "medication_storage.h"
#include "../ntp_func.h"  // Para acceder a format_time()

// Define the maximum length for medication ID


static const char *TAG = "MEDICATION_STORAGE";
static const char *NVS_NAMESPACE = "medications";
static const char *NVS_MED_COUNT_KEY = "med_count";
static const char *NVS_MED_INDEX_PREFIX = "med_idx_";

static nvs_handle_t med_nvs_handle = 0;
static medication_t *medications = NULL;
static int medications_count = 0;

// Añadir después de las declaraciones de variables

#define LRU_CACHE_SIZE 3  // Pequeña caché para los medicamentos más usados
static struct {
    char id[MEDICATION_ID_MAX_LEN];
    medication_t *med_ptr;
    uint32_t last_access;
} lru_cache[LRU_CACHE_SIZE] = {0};
static uint32_t access_counter = 0;

// Añadir estas variables estáticas al inicio del archivo, junto con las otras variables globales
static int id_counter = 0;  // Contador para generar claves cortas

// Declaraciones de funciones auxiliares
static esp_err_t save_medication_to_nvs(const medication_t *medication);
static esp_err_t load_medications_from_nvs(void);
static esp_err_t save_medications_index(void);
static int64_t calculate_next_dispense_time(medication_schedule_t *schedule);
static int64_t get_current_time_ms(void);

// Añadir estas funciones a tu archivo

// Corregir la función create_short_key
static char* create_short_key(const char* medication_id) {
    static char short_key[16];  // Buffer para almacenar la clave corta
    
    // Opciones para generar claves cortas:
    
    // Opción 1: Usar un contador simple (med_0, med_1, etc.)
    snprintf(short_key, sizeof(short_key), "med_%d", id_counter++);
    
    // Opción 2: Usar los primeros caracteres del ID
    // strncpy(short_key, medication_id, 10);
    // short_key[10] = '\0';
    // strncat(short_key, "_m", 2);
    
    return short_key;  // Asegurar que se retorne el valor
}

// Estructura para mapear IDs largos a claves cortas
typedef struct {
    char long_id[37];  // Tamaño suficiente para UUID
    char short_key[16]; // 15 caracteres + NULL
} id_mapping_t;

#define MAX_MEDICATIONS 32
static id_mapping_t id_map[MAX_MEDICATIONS];
static int id_map_count = 0;

static bool mapping_changed = false;  // Para reducir escrituras NVS

// Función para obtener la clave corta a partir de un ID largo
static const char* get_short_key(const char* long_id) {
    // Primero buscar si ya existe un mapeo
    for (int i = 0; i < id_map_count; i++) {
        if (strcmp(id_map[i].long_id, long_id) == 0) {
            return id_map[i].short_key;
        }
    }
    
    // Si no existe, crear uno nuevo (si hay espacio)
    if (id_map_count < MAX_MEDICATIONS) {
        strncpy(id_map[id_map_count].long_id, long_id, sizeof(id_map[id_map_count].long_id) - 1);
        id_map[id_map_count].long_id[sizeof(id_map[id_map_count].long_id) - 1] = '\0';
        
        char* short_key = create_short_key(long_id);
        strncpy(id_map[id_map_count].short_key, short_key, sizeof(id_map[id_map_count].short_key) - 1);
        id_map[id_map_count].short_key[sizeof(id_map[id_map_count].short_key) - 1] = '\0';
        
        id_map_count++;
        mapping_changed = true;  // Marcar que hay cambios pendientes
        
        return id_map[id_map_count - 1].short_key;
    }
    
    // Si no hay espacio, devolver un valor predeterminado seguro
    return "med_err";
}

// Función para cargar los mapeos desde NVS durante la inicialización
static void load_id_mappings(void) {
    id_map_count = 0;
    
    uint32_t count = 0;
    esp_err_t err = nvs_get_u32(med_nvs_handle, "map_count", &count);
    if (err == ESP_OK && count > 0) {
        // Limitar al máximo soportado
        if (count > MAX_MEDICATIONS) {
            count = MAX_MEDICATIONS;
        }
        
        for (uint32_t i = 0; i < count; i++) {
            char map_key[16];
            snprintf(map_key, sizeof(map_key), "map_%lu", (unsigned long)i);
            
            size_t required_size = 0;
            err = nvs_get_str(med_nvs_handle, map_key, NULL, &required_size);
            if (err != ESP_OK) {
                continue;
            }
            
            char *map_json = malloc(required_size);
            if (!map_json) {
                continue;
            }
            
            err = nvs_get_str(med_nvs_handle, map_key, map_json, &required_size);
            if (err != ESP_OK) {
                free(map_json);
                continue;
            }
            
            cJSON *map_obj = cJSON_Parse(map_json);
            free(map_json);
            
            if (!map_obj) {
                continue;
            }
            
            cJSON *long_id = cJSON_GetObjectItem(map_obj, "long");
            cJSON *short_key = cJSON_GetObjectItem(map_obj, "short");
            
            if (long_id && cJSON_IsString(long_id) && short_key && cJSON_IsString(short_key)) {
                strncpy(id_map[id_map_count].long_id, long_id->valuestring, sizeof(id_map[id_map_count].long_id) - 1);
                id_map[id_map_count].long_id[sizeof(id_map[id_map_count].long_id) - 1] = '\0';
                
                strncpy(id_map[id_map_count].short_key, short_key->valuestring, sizeof(id_map[id_map_count].short_key) - 1);
                id_map[id_map_count].short_key[sizeof(id_map[id_map_count].short_key) - 1] = '\0';
                
                id_map_count++;
            }
            
            cJSON_Delete(map_obj);
        }
    }
}

// Añadir esta función después de load_id_mappings()

// Añadir una nueva función para guardar los mapeos de forma diferida
static void save_id_mappings_if_changed(void) {
    if (!mapping_changed) {
        return;  // No hay cambios para guardar
    }
    
    ESP_LOGI(TAG, "Guardando mapeos ID-clave actualizados");
    
    // Actualizar contador primero
    nvs_set_u32(med_nvs_handle, "map_count", id_map_count);
    
    // Guardar cada mapeo
    for (int i = 0; i < id_map_count; i++) {
        char map_key[16];
        snprintf(map_key, sizeof(map_key), "map_%d", i);
        
        cJSON *map_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(map_obj, "long", id_map[i].long_id);
        cJSON_AddStringToObject(map_obj, "short", id_map[i].short_key);
        char *map_json = cJSON_Print(map_obj);
        
        if (map_json) {
            nvs_set_str(med_nvs_handle, map_key, map_json);
            free(map_json);
        }
        
        cJSON_Delete(map_obj);
    }
    
    nvs_commit(med_nvs_handle);
    mapping_changed = false;
}

esp_err_t medication_storage_init(void) {
    esp_err_t err;
    
    // Inicializar NVS
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition is full or version has changed, erase and retry
        ESP_LOGW(TAG, "Erasing NVS partition due to initialization error: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    
    // Abrir handle para NVS
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &med_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS namespace: %s", esp_err_to_name(err));
        return err;
    }
    
    // Cargar mapeos de IDs
    load_id_mappings();
    
    // Cargar medicamentos almacenados
    err = load_medications_from_nvs();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not load medications from NVS: %s", esp_err_to_name(err));
        // No retornamos error, ya que podría ser la primera ejecución
    }
    
    ESP_LOGI(TAG, "Medication storage initialized with %d medications", medications_count);
    return ESP_OK;
}

esp_err_t medication_storage_process_json(const char* json_str) {
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Received NULL JSON string");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Processing medication JSON: %s", json_str);
    
    // Parsear JSON
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Error parsing JSON: %s", cJSON_GetErrorPtr());
        return ESP_FAIL;
    }
    
    // Obtener el objeto "payload"
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    if (!payload) {
        ESP_LOGE(TAG, "No 'payload' field in JSON");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    // Obtener el array "medications"
    cJSON *medications_array = cJSON_GetObjectItem(payload, "medications");
    if (!medications_array || !cJSON_IsArray(medications_array)) {
        ESP_LOGE(TAG, "No 'medications' array in payload");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    // Liberar medicamentos anteriores si existen
    if (medications != NULL) {
        for (int i = 0; i < medications_count; i++) {
            if (medications[i].schedules != NULL) {
                free(medications[i].schedules);
            }
        }
        free(medications);
        medications = NULL;
        medications_count = 0;
    }
    
    // Contar número de medicamentos
    medications_count = cJSON_GetArraySize(medications_array);
    if (medications_count <= 0) {
        ESP_LOGW(TAG, "Empty medications array received");
        cJSON_Delete(root);
        return ESP_OK; // No es un error, simplemente no hay medicamentos
    }
    
    // Asignar memoria para los medicamentos
    medications = calloc(medications_count, sizeof(medication_t));
    if (!medications) {
        ESP_LOGE(TAG, "Memory allocation failed for medications");
        medications_count = 0;
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    
    // Procesar cada medicamento
    int med_index = 0;
    cJSON *medication_item;
    cJSON_ArrayForEach(medication_item, medications_array) {
        if (!cJSON_IsObject(medication_item)) {
            ESP_LOGW(TAG, "Item in medications array is not an object, skipping");
            continue;
        }
        
        // Obtener datos básicos del medicamento
        cJSON *id = cJSON_GetObjectItem(medication_item, "id");
        cJSON *name = cJSON_GetObjectItem(medication_item, "name");
        cJSON *compartment = cJSON_GetObjectItem(medication_item, "compartment");
        cJSON *type = cJSON_GetObjectItem(medication_item, "type");
        cJSON *pills_per_dose = cJSON_GetObjectItem(medication_item, "pillsPerDose");
        cJSON *total_pills = cJSON_GetObjectItem(medication_item, "totalPills");
        
        // Verificar campos obligatorios
        if (!id || !cJSON_IsString(id) || !name || !cJSON_IsString(name) || 
            !compartment || !cJSON_IsNumber(compartment) || !type || !cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Medication missing required fields, skipping");
            continue;
        }
        
        // Copiar datos básicos
        medication_t *med = &medications[med_index];
        strncpy(med->id, id->valuestring, sizeof(med->id) - 1);
        strncpy(med->name, name->valuestring, sizeof(med->name) - 1);
        med->compartment = compartment->valueint;
        strncpy(med->type, type->valuestring, sizeof(med->type) - 1);
        
        // Campos opcionales según tipo
        if (strcmp(med->type, "pill") == 0) {
            if (pills_per_dose && cJSON_IsNumber(pills_per_dose)) {
                med->pills_per_dose = pills_per_dose->valueint;
            } else {
                med->pills_per_dose = 1; // Valor por defecto
            }
            
            if (total_pills && cJSON_IsNumber(total_pills)) {
                med->total_pills = total_pills->valueint;
            } else {
                med->total_pills = 0;
            }
        }
        
        // Procesar horarios
        cJSON *schedules_array = cJSON_GetObjectItem(medication_item, "schedules");
        if (schedules_array && cJSON_IsArray(schedules_array)) {
            int schedules_count = cJSON_GetArraySize(schedules_array);
            if (schedules_count > 0) {
                med->schedules = calloc(schedules_count, sizeof(medication_schedule_t));
                if (!med->schedules) {
                    ESP_LOGE(TAG, "Memory allocation failed for schedules");
                    // Continuar con el siguiente medicamento
                    continue;
                }
                
                med->schedules_count = 0; // Inicializar contador de horarios válidos
                
                // Procesar cada horario
                cJSON *schedule_item;
                int sched_index = 0;
                cJSON_ArrayForEach(schedule_item, schedules_array) {
                    if (!cJSON_IsObject(schedule_item)) {
                        continue;
                    }
                    
                    medication_schedule_t *schedule = &med->schedules[sched_index];
                    
                    // Obtener ID del horario
                    cJSON *sched_id = cJSON_GetObjectItem(schedule_item, "id");
                    if (sched_id && cJSON_IsString(sched_id)) {
                        strncpy(schedule->id, sched_id->valuestring, sizeof(schedule->id) - 1);
                    } else {
                        // Generar ID si no existe
                        snprintf(schedule->id, sizeof(schedule->id), "sched_%d", sched_index);
                    }
                    
                    // Hora en minutos
                    cJSON *time = cJSON_GetObjectItem(schedule_item, "time");
                    if (time && cJSON_IsNumber(time)) {
                        schedule->time_in_minutes = time->valueint;
                    } else {
                        // Si no tiene hora, usar 8:00 AM como predeterminado
                        schedule->time_in_minutes = 8 * 60;
                    }
                    
                    // Modo de intervalo
                    cJSON *interval_mode = cJSON_GetObjectItem(schedule_item, "intervalMode");
                    schedule->interval_mode = interval_mode && cJSON_IsTrue(interval_mode);
                    
                    if (schedule->interval_mode) {
                        // Modo intervalo
                        cJSON *interval_hours = cJSON_GetObjectItem(schedule_item, "intervalHours");
                        if (interval_hours && cJSON_IsNumber(interval_hours)) {
                            schedule->interval_hours = interval_hours->valueint;
                        } else {
                            schedule->interval_hours = 24; // Por defecto, cada 24 horas
                        }
                        
                        cJSON *treatment_days = cJSON_GetObjectItem(schedule_item, "treatmentDays");
                        if (treatment_days && cJSON_IsNumber(treatment_days)) {
                            schedule->treatment_days = treatment_days->valueint;
                            
                            // Calcular fecha de fin de tratamiento
                            int64_t now = get_current_time_ms();
                            schedule->treatment_end_date = now + (int64_t)schedule->treatment_days * 24 * 60 * 60 * 1000;
                        } else {
                            schedule->treatment_days = 0; // Sin fin definido
                            schedule->treatment_end_date = 0;
                        }
                    } else {
                        // Modo días de semana
                        cJSON *days_array = cJSON_GetObjectItem(schedule_item, "days");
                        
                        // Versión optimizada para recuperar días de la semana
                        if (days_array && cJSON_IsArray(days_array)) {
                            int days_count = cJSON_GetArraySize(days_array);
                            schedule->days_count = 0;
                            
                            // Reservar espacio para todos los días posibles
                            uint8_t valid_days[7] = {0};
                            int valid_count = 0;
                            
                            // Obtener días en un solo paso
                            for (int i = 0; i < days_count && i < 7; i++) {
                                cJSON *day_item = cJSON_GetArrayItem(days_array, i);
                                if (day_item && cJSON_IsNumber(day_item)) {
                                    int day = day_item->valueint;
                                    if (day >= 1 && day <= 7 && !valid_days[day-1]) {
                                        valid_days[day-1] = 1;
                                        valid_count++;
                                    }
                                }
                            }
                            
                            // Ahora copiar todos los días válidos de una vez
                            if (valid_count > 0) {
                                for (int i = 0, j = 0; i < 7; i++) {
                                    if (valid_days[i]) {
                                        schedule->days[j++] = i + 1;
                                    }
                                }
                                schedule->days_count = valid_count;
                            }
                        }
                        
                        if (schedule->days_count == 0) {
                            // Si no hay días seleccionados, usar todos los días
                            for (int i = 1; i <= 7; i++) {
                                schedule->days[i-1] = i;
                            }
                            schedule->days_count = 7;
                        }
                    }
                    
                    // Inicializar tiempos de dispensación
                    schedule->next_dispense_time = 0;
                    schedule->last_dispensed_time = 0;
                    
                    sched_index++;
                    med->schedules_count = sched_index;
                }
            }
        }
        
        // Guardar medicamento en NVS
        save_medication_to_nvs(med);
        
        med_index++;
    }
    
    // Actualizar el conteo real de medicamentos
    medications_count = med_index;
    
    // Guardar índice de medicamentos
    save_medications_index();
    
    // Calcular próximas dispensaciones
    medication_storage_update_next_dispense_times();
    
    // Guardar mapeos si hubo cambios
    save_id_mappings_if_changed();
    
    ESP_LOGI(TAG, "Successfully processed %d medications", medications_count);
    return ESP_OK;
}

static esp_err_t save_medication_to_nvs(const medication_t *medication) {
    if (!medication || !med_nvs_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Crear objeto JSON para el medicamento
    cJSON *med_obj = cJSON_CreateObject();
    if (!med_obj) {
        return ESP_ERR_NO_MEM;
    }
    
    // Añadir propiedades básicas
    cJSON_AddStringToObject(med_obj, "id", medication->id);
    cJSON_AddStringToObject(med_obj, "name", medication->name);
    cJSON_AddNumberToObject(med_obj, "compartment", medication->compartment);
    cJSON_AddStringToObject(med_obj, "type", medication->type);
    cJSON_AddNumberToObject(med_obj, "pillsPerDose", medication->pills_per_dose);
    cJSON_AddNumberToObject(med_obj, "totalPills", medication->total_pills);
    
    // Añadir horarios
    cJSON *schedules_array = cJSON_AddArrayToObject(med_obj, "schedules");
    if (medication->schedules) {
        for (int i = 0; i < medication->schedules_count; i++) {
            medication_schedule_t *schedule = &medication->schedules[i];
            cJSON *sched_obj = cJSON_CreateObject();
            
            cJSON_AddStringToObject(sched_obj, "id", schedule->id);
            cJSON_AddNumberToObject(sched_obj, "timeInMinutes", schedule->time_in_minutes);
            cJSON_AddBoolToObject(sched_obj, "intervalMode", schedule->interval_mode);
            cJSON_AddNumberToObject(sched_obj, "intervalHours", schedule->interval_hours);
            cJSON_AddNumberToObject(sched_obj, "treatmentDays", schedule->treatment_days);
            cJSON_AddNumberToObject(sched_obj, "treatmentEndDate", (double)schedule->treatment_end_date);
            cJSON_AddNumberToObject(sched_obj, "nextDispenseTime", (double)schedule->next_dispense_time);
            cJSON_AddNumberToObject(sched_obj, "lastDispensedTime", (double)schedule->last_dispensed_time);
            
            // Añadir días seleccionados
            cJSON *days_array = cJSON_AddArrayToObject(sched_obj, "days");
            for (int j = 0; j < schedule->days_count; j++) {
                cJSON_AddItemToArray(days_array, cJSON_CreateNumber(schedule->days[j]));
            }
            
            cJSON_AddItemToArray(schedules_array, sched_obj);
        }
    }
    
    // Convertir a string JSON - Versión optimizada
    #define JSON_BUFFER_SIZE 2048  // Tamaño adecuado para almacenar un medicamento
    static char json_buffer[JSON_BUFFER_SIZE];
    if (cJSON_PrintPreallocated(med_obj, json_buffer, JSON_BUFFER_SIZE, false)) {
        // Guardar en NVS usando la clave corta
        const char* short_key = get_short_key(medication->id);
        esp_err_t err = nvs_set_str(med_nvs_handle, short_key, json_buffer);
        
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error saving medication to NVS: %s", esp_err_to_name(err));
            cJSON_Delete(med_obj);
            return err;
        }
        
        // Confirmar escritura solo cuando sea necesario
        static uint8_t write_count = 0;
        if (++write_count >= 5) {  // Hacer commit cada 5 escrituras
            err = nvs_commit(med_nvs_handle);
            write_count = 0;
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error committing to NVS: %s", esp_err_to_name(err));
                cJSON_Delete(med_obj);
                return err;
            }
        }
        
        cJSON_Delete(med_obj);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Error printing JSON, fallback to dynamic allocation");
        // Si falla, usar el método original como fallback
        char *json_str = cJSON_Print(med_obj);
        cJSON_Delete(med_obj);
        
        if (!json_str) {
            return ESP_ERR_NO_MEM;
        }
        
        // Obtener la clave corta para el medicamento
        const char* short_key = get_short_key(medication->id);
        esp_err_t err = nvs_set_str(med_nvs_handle, short_key, json_str);
        free(json_str);
        
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error saving medication to NVS: %s", esp_err_to_name(err));
            return err;
        }
        
        err = nvs_commit(med_nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error committing to NVS: %s", esp_err_to_name(err));
            return err;
        }
        
        return ESP_OK;
    }
}

static esp_err_t save_medications_index(void) {
    if (!med_nvs_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Guardar conteo de medicamentos
    esp_err_t err = nvs_set_u32(med_nvs_handle, NVS_MED_COUNT_KEY, medications_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving medications count: %s", esp_err_to_name(err));
        return err;
    }
    
    // Guardar índice de medicamentos
    for (int i = 0; i < medications_count; i++) {
        char key[32];
        snprintf(key, sizeof(key), "%s%d", NVS_MED_INDEX_PREFIX, i);
        
        err = nvs_set_str(med_nvs_handle, key, medications[i].id);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error saving medication index %d: %s", i, esp_err_to_name(err));
            return err;
        }
    }
    
    // Confirmar escritura
    err = nvs_commit(med_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing index to NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    return ESP_OK;
}

static esp_err_t load_medications_from_nvs(void) {
    if (!med_nvs_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Obtener conteo de medicamentos
    esp_err_t err = nvs_get_u32(med_nvs_handle, NVS_MED_COUNT_KEY, (uint32_t*)&medications_count);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            // No hay medicamentos guardados
            medications_count = 0;
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Error getting medications count: %s", esp_err_to_name(err));
        return err;
    }
    
    if (medications_count <= 0) {
        // No hay medicamentos
        return ESP_OK;
    }
    
    // Asignar memoria para medicamentos
    medications = calloc(medications_count, sizeof(medication_t));
    if (!medications) {
        ESP_LOGE(TAG, "Memory allocation failed for medications");
        medications_count = 0;
        return ESP_ERR_NO_MEM;
    }
    
    int valid_meds = 0;  // Añadir esta declaración
    
    // Para cada medicamento en el índice
    for (int i = 0; i < medications_count; i++) {
        char key[32];
        snprintf(key, sizeof(key), "%s%d", NVS_MED_INDEX_PREFIX, i);
        
        // Obtener ID del medicamento (será el ID largo)
        size_t required_size = 0;
        err = nvs_get_str(med_nvs_handle, key, NULL, &required_size);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Error getting medication ID size at index %d: %s", i, esp_err_to_name(err));
            continue;
        }
        
        char *med_id = malloc(required_size);
        if (!med_id) {
            ESP_LOGE(TAG, "Memory allocation failed for medication ID");
            continue;
        }
        
        err = nvs_get_str(med_nvs_handle, key, med_id, &required_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error getting medication ID at index %d: %s", i, esp_err_to_name(err));
            free(med_id);
            continue;
        }
        
        // Obtener la clave corta para este ID
        const char* short_key = get_short_key(med_id);
        
        // Obtener datos del medicamento usando la clave corta
        required_size = 0;
        err = nvs_get_str(med_nvs_handle, short_key, NULL, &required_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error getting medication data size for %s: %s", med_id, esp_err_to_name(err));
            free(med_id);
            continue;
        }
        
        char *json_str = malloc(required_size);
        if (!json_str) {
            ESP_LOGE(TAG, "Memory allocation failed for medication data");
            free(med_id);
            continue;
        }
        
        err = nvs_get_str(med_nvs_handle, short_key, json_str, &required_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error getting medication data for %s: %s", med_id, esp_err_to_name(err));
            free(med_id);
            free(json_str);
            continue;
        }
        
        // Parsear JSON
        cJSON *med_obj = cJSON_Parse(json_str);
        free(json_str);
        
        if (!med_obj) {
            ESP_LOGE(TAG, "Error parsing JSON for medication %s", med_id);
            free(med_id);
            continue;
        }
        
        // Copiar datos al medicamento
        medication_t *med = &medications[valid_meds];
        
        // ID y campos básicos
        strncpy(med->id, med_id, sizeof(med->id) - 1);
        free(med_id);
        
        cJSON *name = cJSON_GetObjectItem(med_obj, "name");
        if (name && cJSON_IsString(name)) {
            strncpy(med->name, name->valuestring, sizeof(med->name) - 1);
        }
        
        cJSON *compartment = cJSON_GetObjectItem(med_obj, "compartment");
        if (compartment && cJSON_IsNumber(compartment)) {
            med->compartment = compartment->valueint;
        }
        
        cJSON *type = cJSON_GetObjectItem(med_obj, "type");
        if (type && cJSON_IsString(type)) {
            strncpy(med->type, type->valuestring, sizeof(med->type) - 1);
        }
        
        cJSON *pills_per_dose = cJSON_GetObjectItem(med_obj, "pillsPerDose");
        if (pills_per_dose && cJSON_IsNumber(pills_per_dose)) {
            med->pills_per_dose = pills_per_dose->valueint;
        }
        
        cJSON *total_pills = cJSON_GetObjectItem(med_obj, "totalPills");
        if (total_pills && cJSON_IsNumber(total_pills)) {
            med->total_pills = total_pills->valueint;
        }
        
        // Horarios
        cJSON *schedules_array = cJSON_GetObjectItem(med_obj, "schedules");
        if (schedules_array && cJSON_IsArray(schedules_array)) {
            int schedules_count = cJSON_GetArraySize(schedules_array);
            if (schedules_count > 0) {
                med->schedules = calloc(schedules_count, sizeof(medication_schedule_t));
                if (!med->schedules) {
                    ESP_LOGE(TAG, "Memory allocation failed for schedules");
                    cJSON_Delete(med_obj);
                    continue;
                }
                
                // Cargar cada horario
                int valid_scheds = 0;
                cJSON *sched_item;
                int index = 0;
                cJSON_ArrayForEach(sched_item, schedules_array) {
                    if (index >= schedules_count) break;
                    
                    medication_schedule_t *schedule = &med->schedules[valid_scheds];
                    
                    // ID del horario
                    cJSON *sched_id = cJSON_GetObjectItem(sched_item, "id");
                    if (sched_id && cJSON_IsString(sched_id)) {
                        strncpy(schedule->id, sched_id->valuestring, sizeof(schedule->id) - 1);
                    } else {
                        snprintf(schedule->id, sizeof(schedule->id), "sched_%d", index);
                    }
                    
                    // Hora en minutos
                    cJSON *time = cJSON_GetObjectItem(sched_item, "timeInMinutes");
                    if (time && cJSON_IsNumber(time)) {
                        schedule->time_in_minutes = time->valueint;
                    }
                    
                    // Modo intervalo
                    cJSON *interval_mode = cJSON_GetObjectItem(sched_item, "intervalMode");
                    schedule->interval_mode = interval_mode && cJSON_IsTrue(interval_mode);
                    
                    // Horas de intervalo
                    cJSON *interval_hours = cJSON_GetObjectItem(sched_item, "intervalHours");
                    if (interval_hours && cJSON_IsNumber(interval_hours)) {
                        schedule->interval_hours = interval_hours->valueint;
                    }
                    
                    // Días de tratamiento
                    cJSON *treatment_days = cJSON_GetObjectItem(sched_item, "treatmentDays");
                    if (treatment_days && cJSON_IsNumber(treatment_days)) {
                        schedule->treatment_days = treatment_days->valueint;
                    }
                    
                    // Fecha fin tratamiento
                    cJSON *treatment_end_date = cJSON_GetObjectItem(sched_item, "treatmentEndDate");
                    if (treatment_end_date && cJSON_IsNumber(treatment_end_date)) {
                        schedule->treatment_end_date = (int64_t)treatment_end_date->valuedouble;
                    }
                    
                    // Próxima dispensación
                    cJSON *next_dispense_time = cJSON_GetObjectItem(sched_item, "nextDispenseTime");
                    if (next_dispense_time && cJSON_IsNumber(next_dispense_time)) {
                        schedule->next_dispense_time = (int64_t)next_dispense_time->valuedouble;
                    }
                    
                    // Última dispensación
                    cJSON *last_dispensed_time = cJSON_GetObjectItem(sched_item, "lastDispensedTime");
                    if (last_dispensed_time && cJSON_IsNumber(last_dispensed_time)) {
                        schedule->last_dispensed_time = (int64_t)last_dispensed_time->valuedouble;
                    }
                    
                    // Días seleccionados
                    schedule->days_count = 0;
                    cJSON *days_array = cJSON_GetObjectItem(sched_item, "days");
                    if (days_array && cJSON_IsArray(days_array)) {
                        int days_count = cJSON_GetArraySize(days_array);
                        for (int j = 0; j < days_count && j < 7; j++) {
                            cJSON *day_item = cJSON_GetArrayItem(days_array, j);
                            if (day_item && cJSON_IsNumber(day_item)) {
                                int day = day_item->valueint;
                                if (day >= 1 && day <= 7) {
                                    schedule->days[schedule->days_count++] = day;
                                }
                            }
                        }
                    }
                    
                    valid_scheds++;
                    index++;
                }
                
                med->schedules_count = valid_scheds;
            }
        }
        
        cJSON_Delete(med_obj);
        valid_meds++;
    }
    
    // Actualizar el contador real
    medications_count = valid_meds;
    
    // Calcular próximas dispensaciones
    medication_storage_update_next_dispense_times();
    
    return ESP_OK;
}

// Obtener la hora actual en milisegundos desde EPOCH
static int64_t get_current_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (tv.tv_usec / 1000);
}

// Calcular la próxima dispensación para un horario
static int64_t calculate_next_dispense_time(medication_schedule_t *schedule) {
    if (!schedule) return 0;
    
    // Cachear el tiempo actual para evitar múltiples llamadas
    static time_t last_time_check = 0;
    static struct tm cached_timeinfo;
    static int64_t cached_now_ms = 0;
    static int cached_time_mins = 0;
    static int cached_weekday = 0;
    
    time_t now_secs;
    time(&now_secs);
    
    // Solo recalcular si pasó al menos 1 segundo desde la última llamada
    if (now_secs != last_time_check) {
        last_time_check = now_secs;
        localtime_r(&now_secs, &cached_timeinfo);
        cached_now_ms = get_current_time_ms();
        cached_time_mins = cached_timeinfo.tm_hour * 60 + cached_timeinfo.tm_min;
        cached_weekday = cached_timeinfo.tm_wday == 0 ? 7 : cached_timeinfo.tm_wday;
    }
    
    // Usar valores cacheados
    int64_t now_ms = cached_now_ms;
    int current_time_mins = cached_time_mins;
    int current_weekday = cached_weekday;
    struct tm timeinfo = cached_timeinfo;
    
    // Resto de la función igual...

    // Si el tratamiento ha finalizado
    if (schedule->treatment_end_date > 0 && now_ms >= schedule->treatment_end_date) {
        return INT64_MAX; // No más dispensaciones
    }
    
    // MODO INTERVALO
    if (schedule->interval_mode) {
        // Si la hora del día no ha pasado
        if (schedule->time_in_minutes > current_time_mins) {
            // Programar para hoy
            struct tm dispense_time = timeinfo;
            dispense_time.tm_hour = schedule->time_in_minutes / 60;
            dispense_time.tm_min = schedule->time_in_minutes % 60;
            dispense_time.tm_sec = 0;
            
            time_t dispense_time_t = mktime(&dispense_time);
            return (int64_t)dispense_time_t * 1000;
        }
        
        // Si es la primera dispensación o la última ya pasó
        if (schedule->last_dispensed_time == 0 || 
            now_ms - schedule->last_dispensed_time >= (int64_t)schedule->interval_hours * 60 * 60 * 1000) {
            
            // Calcular para mañana a la hora programada
            struct tm next_time = timeinfo;
            next_time.tm_mday += 1; // Mañana
            next_time.tm_hour = schedule->time_in_minutes / 60;
            next_time.tm_min = schedule->time_in_minutes % 60;
            next_time.tm_sec = 0;
            
            time_t next_time_t = mktime(&next_time);
            int64_t next_ms = (int64_t)next_time_t * 1000;
            
            // Verificar si excede el fin de tratamiento
            if (schedule->treatment_end_date > 0 && next_ms > schedule->treatment_end_date) {
                return INT64_MAX;
            }
            
            return next_ms;
        }
        
        // Calcular próxima dispensación en base a la última más el intervalo
        int64_t next_interval_ms = schedule->last_dispensed_time + 
                                  (int64_t)schedule->interval_hours * 60 * 60 * 1000;
        
        // Verificar fin de tratamiento
        if (schedule->treatment_end_date > 0 && next_interval_ms > schedule->treatment_end_date) {
            return INT64_MAX;
        }
        
        return next_interval_ms;
    }
    
    // MODO DÍAS DE SEMANA
    else {
        // Comprobar si hoy es un día seleccionado y la hora aún no ha pasado
        bool is_today_selected = false;
        for (int i = 0; i < schedule->days_count; i++) {
            if (schedule->days[i] == current_weekday) {
                is_today_selected = true;
                break;
            }
        }
        
        if (is_today_selected && schedule->time_in_minutes > current_time_mins) {
            // Programar para hoy
            struct tm dispense_time = timeinfo;
            dispense_time.tm_hour = schedule->time_in_minutes / 60;
            dispense_time.tm_min = schedule->time_in_minutes % 60;
            dispense_time.tm_sec = 0;
            
            time_t dispense_time_t = mktime(&dispense_time);
            return (int64_t)dispense_time_t * 1000;
        }
        
        // Buscar el próximo día seleccionado
        int days_ahead = 1;
        while (days_ahead <= 7) {
            int next_day = current_weekday + days_ahead;
            if (next_day > 7) next_day -= 7;
            
            for (int i = 0; i < schedule->days_count; i++) {
                if (schedule->days[i] == next_day) {
                    // Encontrado próximo día
                    struct tm next_time = timeinfo;
                    next_time.tm_mday += days_ahead;
                    next_time.tm_hour = schedule->time_in_minutes / 60;
                    next_time.tm_min = schedule->time_in_minutes % 60;
                    next_time.tm_sec = 0;
                    
                    time_t next_time_t = mktime(&next_time);
                    return (int64_t)next_time_t * 1000;
                }
            }
            
            days_ahead++;
        }
        
        // No se encontró ningún día válido (no debería ocurrir)
        return INT64_MAX;
    }
}

// Actualizar todos los tiempos de dispensación
void medication_storage_update_next_dispense_times(void) {
    if (!medications || medications_count == 0) {
        return;
    }
    
    for (int i = 0; i < medications_count; i++) {
        medication_t *med = &medications[i];
        
        for (int j = 0; j < med->schedules_count; j++) {
            medication_schedule_t *schedule = &med->schedules[j];
            
            // Calcular próxima dispensación
            schedule->next_dispense_time = calculate_next_dispense_time(schedule);
            
#if CONFIG_LOG_DEFAULT_LEVEL >= ESP_LOG_INFO
            static char time_buffer[32];
            // NO declarar la función aquí, solo usarla
            if (esp_log_level_get(TAG) >= ESP_LOG_INFO) {
                format_time(schedule->next_dispense_time, time_buffer, sizeof(time_buffer));
                ESP_LOGI(TAG, "Next dispense for %s (schedule %s): %s", 
                        med->name, schedule->id, time_buffer);
            }
#endif
        }
        
        // Guardar cambios en NVS
        save_medication_to_nvs(med);
    }
}

// Modificar medication_storage_get_medication para usar la caché
medication_t* medication_storage_get_medication(const char* med_id) {
    if (!med_id || !medications) {
        return NULL;
    }
    
    access_counter++;
    
    // Primero buscar en la caché
    int lru_idx = -1;
    uint32_t oldest_access = UINT32_MAX;
    
    for (int i = 0; i < LRU_CACHE_SIZE; i++) {
        if (lru_cache[i].med_ptr && strcmp(lru_cache[i].id, med_id) == 0) {
            // Encontrado en caché, actualizar tiempo de acceso
            lru_cache[i].last_access = access_counter;
            return lru_cache[i].med_ptr;
        }
        
        // Encontrar el elemento menos usado recientemente
        if (lru_cache[i].last_access < oldest_access) {
            oldest_access = lru_cache[i].last_access;
            lru_idx = i;
        }
    }
    
    // No encontrado en caché, buscar en la lista principal
    for (int i = 0; i < medications_count; i++) {
        if (strcmp(medications[i].id, med_id) == 0) {
            // Actualizar caché con este medicamento
            if (lru_idx >= 0) {
                strncpy(lru_cache[lru_idx].id, med_id, MEDICATION_ID_MAX_LEN - 1);
                lru_cache[lru_idx].id[MEDICATION_ID_MAX_LEN - 1] = '\0';
                lru_cache[lru_idx].med_ptr = &medications[i];
                lru_cache[lru_idx].last_access = access_counter;
            }
            return &medications[i];
        }
    }
    
    return NULL;
}

medication_t* medication_storage_get_all_medications(int* count) {
    if (!count) {
        return NULL;
    }
    
    *count = medications_count;
    return medications;
}

medication_t* medication_storage_check_dispense(int64_t current_time) {
    if (!medications || medications_count == 0) {
        ESP_LOGW(TAG, "No hay medicamentos registrados para verificar dispensación");
        return NULL;
    }
    
    char current_time_str[32];
    format_time(current_time, current_time_str, sizeof(current_time_str));
    ESP_LOGI(TAG, "Verificando dispensación a las %s", current_time_str);
    
    medication_t *next_med = NULL;
    int64_t soonest_time = INT64_MAX;
    int soonest_sched_idx = -1;
    
    // Buscar el medicamento más próximo a dispensar
    for (int i = 0; i < medications_count; i++) {
        medication_t *med = &medications[i];
        
        ESP_LOGI(TAG, "Revisando medicamento: %s (%d horarios)", med->name, med->schedules_count);
        
        for (int j = 0; j < med->schedules_count; j++) {
            medication_schedule_t *schedule = &med->schedules[j];
            
            char next_time_str[32];
            format_time(schedule->next_dispense_time, next_time_str, sizeof(next_time_str));
            
            ESP_LOGI(TAG, "  - Horario %s: próxima dispensación %s", 
                    schedule->id, next_time_str);
            
            // Si está programado para dispensar y es el más cercano
            if (schedule->next_dispense_time > 0 && 
                schedule->next_dispense_time <= current_time &&
                schedule->next_dispense_time < soonest_time) {
                
                ESP_LOGI(TAG, "    ✓ Horario elegible para dispensación");
                next_med = med;
                soonest_time = schedule->next_dispense_time;
                soonest_sched_idx = j;
            } else {
                if (schedule->next_dispense_time <= 0) {
                    ESP_LOGI(TAG, "    ✗ Horario no programado (next_dispense_time <= 0)");
                } else if (schedule->next_dispense_time > current_time) {
                    ESP_LOGI(TAG, "    ✗ Horario programado para el futuro (%lld ms después)", 
                            schedule->next_dispense_time - current_time);
                } else {
                    ESP_LOGI(TAG, "    ✗ No es el horario más cercano");
                }
            }
        }
    }
    
    // Si encontramos un medicamento a dispensar
    if (next_med && soonest_sched_idx >= 0) {
        // Actualizar último tiempo de dispensación
        medication_schedule_t *schedule = &next_med->schedules[soonest_sched_idx];
        schedule->last_dispensed_time = current_time;
        
        // Actualizar recuento de pastillas
        if (strcmp(next_med->type, "pill") == 0) {
            next_med->total_pills -= next_med->pills_per_dose;
            if (next_med->total_pills < 0) {
                next_med->total_pills = 0;
            }
            ESP_LOGI(TAG, "Actualizado recuento de pastillas: %d restantes", next_med->total_pills);
        }
        
        // Recalcular próximo tiempo de dispensación
        schedule->next_dispense_time = calculate_next_dispense_time(schedule);
        
        char next_time_str[32];
        format_time(schedule->next_dispense_time, next_time_str, sizeof(next_time_str));
        ESP_LOGI(TAG, "Próxima dispensación programada para: %s", next_time_str);
        
        // Guardar cambios
        save_medication_to_nvs(next_med);
        
        ESP_LOGI(TAG, "✅ Medicamento %s listo para dispensar desde compartimento %d", 
                next_med->name, next_med->compartment);
        
        return next_med;
    }
    
    ESP_LOGI(TAG, "No se encontró ningún medicamento para dispensar ahora");
    return NULL;
}

// Marcar un medicamento como dispensado
esp_err_t medication_storage_mark_dispensed(const char* med_id, const char* schedule_id) {
    if (!med_id || !schedule_id) {
        return ESP_ERR_INVALID_ARG;
    }
    
    medication_t *med = medication_storage_get_medication(med_id);
    if (!med) {
        ESP_LOGW(TAG, "Medication %s not found", med_id);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Buscar el horario
    int sched_idx = -1;
    for (int i = 0; i < med->schedules_count; i++) {
        if (strcmp(med->schedules[i].id, schedule_id) == 0) {
            sched_idx = i;
            break;
        }
    }
    
    if (sched_idx < 0) {
        ESP_LOGW(TAG, "Schedule %s not found for medication %s", schedule_id, med_id);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Actualizar tiempo de dispensación
    medication_schedule_t *schedule = &med->schedules[sched_idx];
    int64_t current_time = get_current_time_ms();
    schedule->last_dispensed_time = current_time;
    
    // Actualizar recuento de pastillas
    if (strcmp(med->type, "pill") == 0) {
        med->total_pills -= med->pills_per_dose;
        if (med->total_pills < 0) {
            med->total_pills = 0;
        }
    }
    
    // Recalcular próximo tiempo de dispensación
    schedule->next_dispense_time = calculate_next_dispense_time(schedule);
    
    // Guardar cambios
    esp_err_t err = save_medication_to_nvs(med);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving medication after dispensing: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "Medication %s (schedule %s) marked as dispensed", med->name, schedule_id);
    return ESP_OK;
}

// Guardar todos los medicamentos en almacenamiento
esp_err_t medication_storage_save(void) {
    if (!medications || medications_count <= 0 || !med_nvs_handle) {
        ESP_LOGW(TAG, "No hay medicamentos para guardar o NVS no inicializado");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Guardando todos los medicamentos en almacenamiento");
    
    esp_err_t err = ESP_OK;
    
    // Guardar cada medicamento
    for (int i = 0; i < medications_count; i++) {
        esp_err_t temp_err = save_medication_to_nvs(&medications[i]);
        if (temp_err != ESP_OK) {
            ESP_LOGW(TAG, "Error al guardar medicamento %s: %s", 
                     medications[i].name, esp_err_to_name(temp_err));
            err = temp_err;  // Guardar el último error pero continuar con los demás
        }
    }
    
    // Forzar un commit final para asegurar que todos los cambios se guarden
    esp_err_t commit_err = nvs_commit(med_nvs_handle);
    if (commit_err != ESP_OK) {
        ESP_LOGE(TAG, "Error al hacer commit de los cambios: %s", esp_err_to_name(commit_err));
        return commit_err;  // Priorizar el error de commit
    }
    
    // Guardar mapeos ID si hubo cambios
    save_id_mappings_if_changed();
    
    return err;
}