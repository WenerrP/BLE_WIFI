#include "nextion_driver.h"
#include "esp_system.h"
#include <time.h>
#include <sys/time.h>
#include "ntp_func.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "NEXTION";
static const char *TAG_TIME = "NEXTION_TIME";
static TaskHandle_t time_update_task_handle = NULL;
static char *current_user_name = NULL;
static bool nextion_initialized = false;

// Variables globales
static QueueHandle_t nextion_uart_queue;
static TaskHandle_t nextion_rx_task_handle = NULL;
void nextion_time_updater_stop(void);
bool nextion_time_updater_start(const char *user_name);

// Añadir estas variables globales
static uint32_t update_interval_ms = 1000; // 1 segundo por defecto
static bool low_power_mode = false;
static uint8_t update_priority = 2;  // 0=solo minuto, 1=segundos, 2=todo

// Definir constantes para prioridades
#define PRIORITY_MINIMAL 0   // Solo actualiza minutos
#define PRIORITY_MEDIUM  1   // Actualiza segundos
#define PRIORITY_FULL    2   // Actualiza todo constantemente

/**
 * @brief Configura la prioridad de actualización de la pantalla
 * 
 * @param priority 0=mínima (solo cambios de minuto), 1=media (segundos), 2=máxima (todo)
 */
void nextion_set_update_priority(uint8_t priority) {
    if (priority > PRIORITY_FULL) priority = PRIORITY_FULL;
    update_priority = priority;
    
    const char* level_names[] = {"MÍNIMA", "MEDIA", "MÁXIMA"};
    ESP_LOGI(TAG, "Prioridad de actualización: %s", level_names[priority]);
}

/**
 * @brief Establece el modo de bajo consumo
 */
void nextion_set_low_power_mode(bool enable) {
    low_power_mode = enable;
    
    if (enable) {
        update_interval_ms = 5000;           // 5 segundos
        update_priority = PRIORITY_MINIMAL;  // Solo minutos
    } else {
        update_interval_ms = 1000;           // 1 segundo
        update_priority = PRIORITY_FULL;     // Completa
    }
    
    ESP_LOGI(TAG, "Modo bajo consumo: %s", enable ? "ACTIVADO" : "DESACTIVADO");
}

/**
 * @brief Inicializa la comunicación UART con la pantalla Nextion
 * 
 * @return true si la inicialización fue exitosa
 */
bool nextion_init(void) {
    // Si ya está inicializado, devolver éxito directamente
    if (nextion_initialized) {
        ESP_LOGI(TAG, "Nextion ya inicializado, omitiendo inicialización");
        return true;
    }
    
    // Intentar desinstalar el driver primero por si acaso está en uso
    uart_driver_delete(NEXTION_UART_NUM);
    
    // Configuración del UART
    uart_config_t uart_config = {
        .baud_rate = NEXTION_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    // Configurar UART
    esp_err_t ret = uart_param_config(NEXTION_UART_NUM, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando parámetros UART: %d (%s)", ret, esp_err_to_name(ret));
        return false;
    }
    
    // Configurar los pines
    ret = uart_set_pin(NEXTION_UART_NUM, NEXTION_UART_TX_PIN, NEXTION_UART_RX_PIN, 
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando pines UART: %d (%s)", ret, esp_err_to_name(ret));
        return false;
    }
    
    // Instalar el driver
    ret = uart_driver_install(NEXTION_UART_NUM, NEXTION_UART_BUFFER_SIZE, 
                           NEXTION_UART_BUFFER_SIZE, 10, &nextion_uart_queue, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error instalando driver UART: %d (%s)", ret, esp_err_to_name(ret));
        return false;
    }
    
    // Marcar como inicializado
    nextion_initialized = true;
    
    ESP_LOGI(TAG, "Nextion UART inicializado correctamente");
    return true;
}

/**
 * @brief Envía un comando a la pantalla Nextion
 * 
 * @param cmd Comando a enviar (sin terminadores)
 * @return true si el envío fue exitoso
 */
bool nextion_send_cmd(const char *cmd) {
    if (cmd == NULL) {
        ESP_LOGE(TAG, "Comando nulo");
        return false;
    }
    
    // Crear buffer para comando + terminadores
    char *full_cmd = malloc(strlen(cmd) + 4); // +4 para los 3 terminadores y el nulo
    if (!full_cmd) {
        ESP_LOGE(TAG, "Error de memoria al enviar comando");
        return false;
    }
    
    // Formatear comando completo
    sprintf(full_cmd, "%s%s", cmd, NEXTION_CMD_END);
    
    // Enviar el comando
    int sent = uart_write_bytes(NEXTION_UART_NUM, full_cmd, strlen(full_cmd));
    free(full_cmd);
    
    if (sent < 0) {
        ESP_LOGE(TAG, "Error enviando comando a Nextion");
        return false;
    } 
    return true;
}

/**
 * @brief Establece el valor de un componente en la pantalla Nextion
 * 
 * @param component Nombre del componente
 * @param value Valor a establecer
 * @return true si el envío fue exitoso
 */
bool nextion_set_component_value(const char *component, const char *value) {
    if (!component || !value) {
        ESP_LOGE(TAG, "Parámetros inválidos para establecer valor de componente");
        return false;
    }
    
    // Crear comando: component.txt="value"
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s.txt=\"%s\"", component, value);
    
    return nextion_send_cmd(cmd);
}

/**
 * @brief Establece el valor numérico de un componente en la pantalla Nextion
 * 
 * @param component Nombre del componente
 * @param value Valor numérico a establecer
 * @return true si el envío fue exitoso
 */
bool nextion_set_component_value_int(const char *component, int value) {
    if (!component) {
        ESP_LOGE(TAG, "Componente inválido para establecer valor numérico");
        return false;
    }
    
    // Crear comando: component.val=value
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s.val=%d", component, value);
    
    return nextion_send_cmd(cmd);
}

/**
 * @brief Cambia a una página específica en la pantalla Nextion
 * 
 * @param page Nombre de la página
 * @return true si el envío fue exitoso
 */
bool nextion_goto_page(const char *page) {
    if (!page) {
        ESP_LOGE(TAG, "Nombre de página inválido");
        return false;
    }
    
    // Crear comando: page pagename
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "page %s", page);
    
    return nextion_send_cmd(cmd);
}

/**
 * @brief Actualiza los componentes de visualización de fecha/hora en la pantalla
 * _
 */
bool nextion_update_time_display(void) {
    // Obtener hora actual del sistema
    time_t now;
    struct tm timeinfo;
    char buffer[32];
    
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // Actualizar fecha
    strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
    if (!nextion_set_component_value("tDate", buffer)) {  // Ajustar al nombre real del componente
        return false;
    }
    
    // Actualizar hora
    strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo);
    if (!nextion_set_component_value("tTime", buffer)) {  // Ajustar al nombre real del componente
        return false;
    }
    
    return true;
}

/**
 * @brief Procesa los datos recibidos desde la pantalla Nextion
 * 
 * @param data Buffer con los datos recibidos
 * @param len Longitud de los datos
 * @return true si se procesó un comando válido
 */
bool nextion_process_received_data(uint8_t *data, size_t len) {
    if (len < 5) { // Verificar longitud mínima
        return false;
    }
    
    // Asegurar que los datos tengan terminación nula
    char *cmd_buffer = malloc(len + 1);
    if (!cmd_buffer) {
        ESP_LOGE(TAG, "Error de memoria al procesar datos");
        return false;
    }
    
    memcpy(cmd_buffer, data, len);
    cmd_buffer[len] = '\0';
    
    // Procesar otros tipos de comandos aquí
    // Por ejemplo, comandos de navegación, configuración, etc.
    
    // Liberar buffer y retornar
    free(cmd_buffer);
    return false; // No se procesó ningún comando conocido
}

/**
 * @brief Tarea para recibir datos desde la pantalla Nextion
 * 
 * @param pvParameters No utilizado
 */
static void nextion_uart_rx_task(void *pvParameters) {
    uint8_t data[128];
    
    while (1) {
        // Leer datos del UART
        int len = uart_read_bytes(NEXTION_UART_NUM, data, sizeof(data), pdMS_TO_TICKS(100));
        
        if (len > 0) {
            // Procesar datos recibidos
            nextion_process_received_data(data, len);
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));  // Pequeña pausa para no saturar la CPU
    }
}

/**
 * @brief Inicia la tarea de recepción de datos desde Nextion
 */
void nextion_start_rx_task(void) {
    // Si la tarea ya está corriendo, no crear otra
    if (nextion_rx_task_handle != NULL) {
        return;
    }
    
    // Crear tarea de recepción
    BaseType_t result = xTaskCreate(nextion_uart_rx_task, 
                                  "nextion_rx", 
                                  4096,      // Stack size 
                                  NULL, 
                                  5,         // Prioridad
                                  &nextion_rx_task_handle);
                                  
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Error al crear tarea de recepción Nextion");
    }
}

/**
 * @brief Informa al módulo Nextion del estado de sincronización NTP
 * 
 * @param success true si la sincronización fue exitosa
 */
void nextion_set_ntp_status(bool success) {
    // Variable estática para evitar actualizaciones redundantes
    static bool last_status = false;
    
    if (last_status == success) {
        return;  // Evitar actualizaciones redundantes
    }
    
    last_status = success;
    
    if (success) {
        // Mostrar indicador de sincronización exitosa
        nextion_set_component_value("tSyncStatus", "Sincronizado");
        nextion_set_component_value_int("bSync", 1);  // Indicador visual
    } else {
        // Mostrar indicador de fallo de sincronización
        nextion_set_component_value("tSyncStatus", "No sincronizado");
        nextion_set_component_value_int("bSync", 0);  // Indicador visual
    }
    
    // Actualizar visualización de hora
    nextion_update_time_display();
}

/**
 * @brief Verifica si la hora del sistema es válida/confiable
 * 
 * @return true si la hora es válida (después de 2023)
 */
static bool is_system_time_valid(void) {
    time_t now;
    struct tm timeinfo;
    
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // Si el año es menor a 2023, la hora probablemente no es válida
    return (timeinfo.tm_year >= (2023 - 1900));
}

/**
 * @brief Tarea para actualizar fecha y hora en la pantalla Nextion
 */
static void nextion_time_update_task(void *pvParameter) {
    // Variables para tracking
    int last_day = -1;
    int last_hour = -1;
    int last_minute = -1;
    int last_second = -1;
    bool last_is_pm = false;
    uint32_t last_update_time = 0;
    bool force_update = true;
    
    ESP_LOGI(TAG, "Iniciando tarea optimizada de actualización de hora");
    
    while (1) {
        uint32_t current_time_ms = esp_timer_get_time() / 1000;
        
        // Verificar si necesitamos actualizar ahora
        if (current_time_ms - last_update_time >= update_interval_ms || force_update) {
            // Obtener hora actual
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);
            
            // Calcular formato 12 horas
            int hour_12 = timeinfo.tm_hour;
            bool is_pm = hour_12 >= 12;
            if (hour_12 > 12) {
                hour_12 -= 12;
            } else if (hour_12 == 0) {
                hour_12 = 12;
            }
            
            // Determinar qué ha cambiado
            bool day_changed = (timeinfo.tm_mday != last_day) || 
                               (timeinfo.tm_mon != last_day / 32) || 
                               (timeinfo.tm_year != last_day / 512);
                               
            bool hour_changed = (hour_12 != last_hour);
            bool minute_changed = (timeinfo.tm_min != last_minute);
            bool second_changed = (timeinfo.tm_sec != last_second);
            bool ampm_changed = (is_pm != last_is_pm);
            
            // Actualizar según la prioridad configurada
            if (day_changed || force_update) {
                char date_str[32];
                snprintf(date_str, sizeof(date_str), "%02d-%02d-%04d", 
                        timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
                nextion_set_component_value("t0", date_str);
                last_day = timeinfo.tm_mday;
            }
            
            if (minute_changed || force_update) {
                char min_str[8]; // Tamaño aumentado para evitar truncamiento
                snprintf(min_str, sizeof(min_str), "%02d", timeinfo.tm_min);
                nextion_set_component_value("tMin", min_str);
                last_minute = timeinfo.tm_min;
            }
            
            if (hour_changed || force_update) {
                char hour_str[16]; // Tamaño aumentado para evitar truncamiento
                snprintf(hour_str, sizeof(hour_str), "%02d", hour_12);
                nextion_set_component_value("tHour", hour_str);
                last_hour = hour_12;
            }
            
            // Actualizar segundos solo en prioridad media o alta
            if (update_priority >= PRIORITY_MEDIUM && (second_changed || force_update)) {
                char sec_str[8]; // Tamaño aumentado para evitar truncamiento
                snprintf(sec_str, sizeof(sec_str), "%02d", timeinfo.tm_sec);
                nextion_set_component_value("tSec", sec_str);
                last_second = timeinfo.tm_sec;
            }
            
            if (ampm_changed || force_update) {
                nextion_set_component_value("AMPM", is_pm ? "PM" : "AM");
                last_is_pm = is_pm;
            }
            
            // Actualizar tiempo completo para displays que no tienen componentes separados
            if (minute_changed || force_update || 
                (update_priority >= PRIORITY_MEDIUM && second_changed)) {
                char time_str[32];
                if (update_priority >= PRIORITY_MEDIUM) {
                    // Con segundos
                    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d %s", 
                            hour_12, timeinfo.tm_min, timeinfo.tm_sec, is_pm ? "PM" : "AM");
                } else {
                    // Sin segundos
                    snprintf(time_str, sizeof(time_str), "%02d:%02d %s", 
                            hour_12, timeinfo.tm_min, is_pm ? "PM" : "AM");
                }
                nextion_set_component_value("t1", time_str);
            }
            
            // Log informativo (solo cuando cambia el minuto para reducir spam)
            if (minute_changed || force_update) {
                ESP_LOGI(TAG_TIME, "Actualizada hora: %02d:%02d:%02d %s [modo:%s]",
                        hour_12, timeinfo.tm_min, timeinfo.tm_sec, is_pm ? "PM" : "AM",
                        low_power_mode ? "económico" : "normal");
            }
            
            // Marcar actualización
            last_update_time = current_time_ms;
            force_update = false;
        }
        
        // Tiempo hasta próxima actualización (esta parte ya está optimizada)
        uint32_t elapsed = esp_timer_get_time() / 1000 - last_update_time;
        uint32_t wait_time = (elapsed < update_interval_ms) ? 
                           (update_interval_ms - elapsed) : 100;
                           
        // Esperar el tiempo calculado pero no más de 1 segundo
        wait_time = (wait_time > 1000) ? 1000 : wait_time;
        vTaskDelay(pdMS_TO_TICKS(wait_time));
    }
}

/**
 * @brief Inicia la tarea de actualización de fecha/hora en la pantalla Nextion
 */
bool nextion_time_updater_start(const char *user_name) {
    // Si no está inicializado, intentar inicializar
    static bool init_check_done = false;
    if (!init_check_done) {
        ESP_LOGI(TAG, "Verificando inicialización de Nextion");
        if (!nextion_init()) {
            ESP_LOGE(TAG, "Fallo al inicializar Nextion para actualizador");
            return false;
        }
        init_check_done = true;
    }
    
    // Usar la función is_system_time_valid para evitar warning de no utilizada
    if (!is_system_time_valid()) {
        ESP_LOGW(TAG, "La hora del sistema no es válida (anterior a 2023). La visualización podría ser incorrecta.");
    }
    
    // Si ya hay una tarea activa, detenerla primero
    nextion_time_updater_stop();
    
    // Guardar nombre de usuario si está definido
    if (user_name != NULL) {
        current_user_name = strdup(user_name);
    } else {
        current_user_name = NULL;
    }
    
    ESP_LOGI(TAG, "Iniciando tarea de actualización de fecha/hora para Nextion");
    
    BaseType_t ret = xTaskCreate(
        nextion_time_update_task,
        "nextion_time",
        4096,       // Stack size
        NULL,       // Parámetros
        3,          // Prioridad media
        &time_update_task_handle);
        
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Error al crear tarea de actualización de fecha/hora");
        return false;
    }
    
    return true;
}

/**
 * @brief Detiene la tarea de actualización de fecha/hora
 */
void nextion_time_updater_stop(void) {
    if (time_update_task_handle != NULL) {
        vTaskDelete(time_update_task_handle);
        time_update_task_handle = NULL;
        ESP_LOGI(TAG, "Tarea de actualización de fecha/hora detenida");
    }
    
    // Liberar memoria del nombre de usuario
    if (current_user_name != NULL) {
        free(current_user_name);
        current_user_name = NULL;
    }
}

/**
 * @brief Actualiza el nombre de usuario mostrado en la pantalla
 */
void nextion_time_updater_set_username(const char *user_name) {
    // Liberar memoria anterior
    if (current_user_name != NULL) {
        free(current_user_name);
        current_user_name = NULL;
    }
    
    // Guardar nuevo nombre de usuario
    if (user_name != NULL) {
        current_user_name = strdup(user_name);
        
        // Actualizar directamente en la pantalla
        nextion_set_component_value("t2", current_user_name);
    }
}