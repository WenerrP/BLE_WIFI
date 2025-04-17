#include "buzzer_driver.h"
#include "esp_rom_sys.h"

static const char *TAG = "BUZZER";
static TaskHandle_t buzzer_task_handle = NULL;

// Estructura para pasar datos a la tarea del buzzer
typedef struct {
    const uint32_t *sequence;
    size_t length;
} buzzer_task_params_t;

/**
 * @brief Tarea para reproducir secuencias de sonido
 * 
 * La secuencia es un array de duraciones en ms: 
 * [sonido1, pausa1, sonido2, pausa2, ...]
 */
static void buzzer_task(void *pvParameters) {
    buzzer_task_params_t *params = (buzzer_task_params_t *)pvParameters;
    
    for (size_t i = 0; i < params->length; i++) {
        if (i % 2 == 0) {
            // Encender buzzer
            gpio_set_level(BUZZER_GPIO_PIN, 1);
        } else {
            // Apagar buzzer
            gpio_set_level(BUZZER_GPIO_PIN, 0);
        }
        
        // Esperar la duración especificada
        vTaskDelay(params->sequence[i] / portTICK_PERIOD_MS);
    }
    
    // Asegurarse de que el buzzer quede apagado al finalizar
    gpio_set_level(BUZZER_GPIO_PIN, 0);
    
    // Liberar la memoria y eliminar la referencia de la tarea
    free(params->sequence);
    free(params);
    buzzer_task_handle = NULL;
    
    // Eliminar la tarea
    vTaskDelete(NULL);
}

/**
 * @brief Inicializar el buzzer
 */
void buzzer_init(void) {
    // Configurar el pin del buzzer como salida
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUZZER_GPIO_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    // Asegurarse de que el buzzer inicie apagado
    gpio_set_level(BUZZER_GPIO_PIN, 0);
    
    ESP_LOGI(TAG, "Buzzer inicializado en GPIO %d", BUZZER_GPIO_PIN);
}

/**
 * @brief Detener cualquier sonido actual
 */
void buzzer_stop(void) {
    if (buzzer_task_handle != NULL) {
        vTaskDelete(buzzer_task_handle);
        buzzer_task_handle = NULL;
    }
    
    gpio_set_level(BUZZER_GPIO_PIN, 0);
}

/**
 * @brief Reproducir un tono simple
 */
void buzzer_beep(uint32_t duration_ms) {
    // Detener cualquier sonido actual primero
    buzzer_stop();
    
    // Crear una secuencia simple: encender, apagar
    uint32_t *sequence = malloc(2 * sizeof(uint32_t));
    if (sequence == NULL) {
        ESP_LOGE(TAG, "Error de memoria al crear secuencia");
        return;
    }
    
    sequence[0] = duration_ms;  // Duración del sonido
    sequence[1] = 0;            // Sin pausa al final
    
    buzzer_task_params_t *params = malloc(sizeof(buzzer_task_params_t));
    if (params == NULL) {
        ESP_LOGE(TAG, "Error de memoria al crear parámetros");
        free(sequence);
        return;
    }
    
    params->sequence = sequence;
    params->length = 2;
    
    xTaskCreate(buzzer_task, "buzzer_task", 2048, params, 5, &buzzer_task_handle);
}

/**
 * @brief Reproducir una secuencia personalizada
 */
void buzzer_play_sequence(const uint32_t *input_sequence, size_t length) {
    // Detener cualquier sonido actual primero
    buzzer_stop();
    
    // Verificar parámetros
    if (input_sequence == NULL || length == 0) {
        ESP_LOGE(TAG, "Secuencia inválida");
        return;
    }
    
    // Copiar la secuencia para que no cambie mientras se reproduce
    uint32_t *sequence = malloc(length * sizeof(uint32_t));
    if (sequence == NULL) {
        ESP_LOGE(TAG, "Error de memoria al copiar secuencia");
        return;
    }
    
    memcpy(sequence, input_sequence, length * sizeof(uint32_t));
    
    buzzer_task_params_t *params = malloc(sizeof(buzzer_task_params_t));
    if (params == NULL) {
        ESP_LOGE(TAG, "Error de memoria al crear parámetros");
        free(sequence);
        return;
    }
    
    params->sequence = sequence;
    params->length = length;
    
    xTaskCreate(buzzer_task, "buzzer_task", 2048, params, 5, &buzzer_task_handle);
}

/**
 * @brief Emular diferentes frecuencias con PWM básico
 * Nota: Esta es una implementación simplificada. Para mejor calidad
 * de sonido, usar el módulo LEDC del ESP32.
 */
void buzzer_beep_with_frequency(uint32_t duration_ms, uint32_t freq_hz) {
    // Implementación básica: para frecuencias bajas, usamos ciclos de encendido/apagado
    if (freq_hz < 50) {
        buzzer_beep(duration_ms);
        return;
    }
    
    // Calcular el período en microsegundos
    uint32_t period_us = 1000000 / freq_hz;
    uint32_t half_period_us = period_us / 2;
    
    // Calcular cuántos ciclos necesitamos
    uint32_t cycles = (duration_ms * 1000) / period_us;
    
    esp_err_t ret = gpio_set_direction(BUZZER_GPIO_PIN, GPIO_MODE_OUTPUT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error al configurar pin: %d", ret);
        return;
    }
    
    // Generar la onda cuadrada manualmente
    for (uint32_t i = 0; i < cycles; i++) {
        gpio_set_level(BUZZER_GPIO_PIN, 1);
        esp_rom_delay_us(half_period_us);  // Reemplazar ets_delay_us por esp_rom_delay_us
        gpio_set_level(BUZZER_GPIO_PIN, 0);
        esp_rom_delay_us(half_period_us);  // Reemplazar ets_delay_us por esp_rom_delay_us
    }
}

/**
 * @brief Reproducir un patrón de sonido predefinido
 */
void buzzer_play_pattern(buzzer_pattern_t pattern) {
    // Patrones predefinidos (duración en ms)
    // Formato: sonido1, pausa1, sonido2, pausa2, ...
    switch (pattern) {
        case BUZZER_PATTERN_STARTUP: {
            // Secuencia ascendente para inicio del sistema
            static const uint32_t sequence[] = {100, 50, 100, 50, 200, 50, 400, 0};
            buzzer_play_sequence(sequence, sizeof(sequence)/sizeof(sequence[0]));
            break;
        }
        
        case BUZZER_PATTERN_WIFI_CONNECTED: {
            // Dos beeps cortos para conexión WiFi
            static const uint32_t sequence[] = {100, 100, 100, 0};
            buzzer_play_sequence(sequence, sizeof(sequence)/sizeof(sequence[0]));
            break;
        }
        
        case BUZZER_PATTERN_WIFI_FAILED: {
            // Un beep largo para fallo WiFi
            static const uint32_t sequence[] = {500, 0};
            buzzer_play_sequence(sequence, sizeof(sequence)/sizeof(sequence[0]));
            break;
        }
        
        case BUZZER_PATTERN_NTP_SUCCESS: {
            // Beep corto para sincronización NTP exitosa
            static const uint32_t sequence[] = {100, 100, 100, 100, 300, 0};
            buzzer_play_sequence(sequence, sizeof(sequence)/sizeof(sequence[0]));
            break;
        }
        
        case BUZZER_PATTERN_MEDICATION_READY: {
            // Secuencia repetitiva para alerta de medicamento
            static const uint32_t sequence[] = {300, 300, 300, 300, 300, 1000};
            buzzer_play_sequence(sequence, sizeof(sequence)/sizeof(sequence[0]));
            break;
        }
        
        case BUZZER_PATTERN_MEDICATION_TAKEN: {
            // Secuencia alegre para medicamento tomado
            static const uint32_t sequence[] = {150, 50, 150, 50, 300, 0};
            buzzer_play_sequence(sequence, sizeof(sequence)/sizeof(sequence[0]));
            break;
        }
        
        case BUZZER_PATTERN_MEDICATION_MISSED: {
            // Secuencia insistente para medicamento no tomado
            static const uint32_t sequence[] = {500, 200, 500, 200, 500, 200, 1000, 500};
            buzzer_play_sequence(sequence, sizeof(sequence)/sizeof(sequence[0]));
            break;
        }
        
        case BUZZER_PATTERN_ERROR: {
            // Tres beeps cortos para error
            static const uint32_t sequence[] = {100, 100, 100, 100, 100, 100};
            buzzer_play_sequence(sequence, sizeof(sequence)/sizeof(sequence[0]));
            break;
        }
        
        case BUZZER_PATTERN_PROVISIONING: {
            // Secuencia distintiva para modo provisioning
            static const uint32_t sequence[] = {100, 100, 100, 100, 300, 300};
            buzzer_play_sequence(sequence, sizeof(sequence)/sizeof(sequence[0]));
            break;
        }
        
        case BUZZER_PATTERN_CONFIRM: {
            // Un beep simple para confirmación
            static const uint32_t sequence[] = {200, 0};
            buzzer_play_sequence(sequence, sizeof(sequence)/sizeof(sequence[0]));
            break;
        }
        
        default:
            ESP_LOGW(TAG, "Patrón desconocido: %d", pattern);
            break;
    }
}