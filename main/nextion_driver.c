#include "nextion_driver.h"
#include "esp_system.h"
#include <time.h>
#include <sys/time.h>

static const char *TAG = "NEXTION";

// Variables globales
static QueueHandle_t nextion_uart_queue;
static nextion_time_data_t last_time_data = {0};
static TaskHandle_t nextion_rx_task_handle = NULL;

/**
 * @brief Inicializa la comunicación UART con la pantalla Nextion
 * 
 * @return true si la inicialización fue exitosa
 */
bool nextion_init(void) {
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
        ESP_LOGE(TAG, "Error configurando parámetros UART: %d", ret);
        return false;
    }
    
    // Configurar los pines
    ret = uart_set_pin(NEXTION_UART_NUM, NEXTION_UART_TX_PIN, NEXTION_UART_RX_PIN, 
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando pines UART: %d", ret);
        return false;
    }
    
    // Instalar el driver
    ret = uart_driver_install(NEXTION_UART_NUM, NEXTION_UART_BUFFER_SIZE, 
                           NEXTION_UART_BUFFER_SIZE, 10, &nextion_uart_queue, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error instalando driver UART: %d", ret);
        return false;
    }
    
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
    
    ESP_LOGI(TAG, "Comando enviado: %s", cmd);
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
 * @brief Solicita a la pantalla mostrar la interfaz de configuración de fecha/hora
 * 
 * @return true si el envío fue exitoso
 */
bool nextion_request_time_setup(void) {
    ESP_LOGI(TAG, "Solicitando configuración manual de fecha y hora");
    
    // Cambiar a la página de configuración de fecha y hora
    return nextion_goto_page("datetime");  // Ajustar al nombre real de tu página
}

/**
 * @brief Actualiza los componentes de visualización de fecha/hora en la pantalla
 * 
 * @return true si la actualización fue exitosa
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
    
    ESP_LOGI(TAG, "Datos recibidos: %s", cmd_buffer);
    
    // Verificar si es un comando de configuración de hora (time_set|year|month|day|hour|minute|second)
    if (strncmp(cmd_buffer, "time_set|", 9) == 0) {
        int year, month, day, hour, minute, second;
        
        if (sscanf(cmd_buffer, "time_set|%d|%d|%d|%d|%d|%d", 
                &year, &month, &day, &hour, &minute, &second) == 6) {
            
            // Almacenar los datos recibidos
            last_time_data.year = year;
            last_time_data.month = month;
            last_time_data.day = day;
            last_time_data.hour = hour;
            last_time_data.minute = minute;
            last_time_data.second = second;
            last_time_data.valid = true;
            
            ESP_LOGI(TAG, "Hora configurada manualmente: %04d-%02d-%02d %02d:%02d:%02d",
                    year, month, day, hour, minute, second);
            
            // Configurar la hora del sistema
            struct tm timeinfo = {
                .tm_year = year - 1900,
                .tm_mon = month - 1,
                .tm_mday = day,
                .tm_hour = hour,
                .tm_min = minute,
                .tm_sec = second
            };
            
            struct timeval tv = {
                .tv_sec = mktime(&timeinfo),
                .tv_usec = 0
            };
            
            if (settimeofday(&tv, NULL) != 0) {
                ESP_LOGE(TAG, "Error al configurar la hora del sistema");
            } else {
                ESP_LOGI(TAG, "Hora del sistema actualizada correctamente");
                nextion_set_ntp_status(true);  // Notificar al módulo NTP que tenemos hora válida
            }
            
            free(cmd_buffer);
            return true;
        }
    }
    
    free(cmd_buffer);
    return false;
}

/**
 * @brief Obtiene los últimos datos de tiempo recibidos desde Nextion
 * 
 * @return Estructura con los datos de tiempo
 */
nextion_time_data_t nextion_get_last_time_data(void) {
    return last_time_data;
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