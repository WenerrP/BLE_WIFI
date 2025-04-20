#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/mcpwm.h"
#include "soc/mcpwm_periph.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "medication_hardware.h"
#include "buzzer_driver.h"

static const char *TAG = "MED_HARDWARE";

// Configuración de pines
#define SERVO_PIN_1      33  // Compartimento 1 - píldoras
#define SERVO_PIN_2      32  // Compartimento 2 - píldoras
#define SERVO_PIN_3      22  // Compartimento 3 - píldoras
#define PUMP_PIN         25  // Pin para controlar la bomba PWM
#define ULTRASONIC_PILL_TRIGGER  27   // Trigger para sensor de pastillas
#define ULTRASONIC_PILL_ECHO     14   // Echo para sensor de pastillas
#define ULTRASONIC_LIQ_TRIGGER   12  // Trigger para sensor de líquido
#define ULTRASONIC_LIQ_ECHO      13  // Echo para sensor de líquido

// Definiciones para el control de servos
#define SERVO_MIN_PULSEWIDTH     500     // Pulso mínimo en microsegundos (0 grados)
#define SERVO_MAX_PULSEWIDTH     2500    // Pulso máximo en microsegundos (180 grados)
#define SERVO_OPEN_POSITION      1500    // Posición abierta (90 grados)
#define SERVO_CLOSE_POSITION     500     // Posición cerrada (0 grados)

// Definiciones para sensores ultrasónicos
#define ULTRASONIC_TIMEOUT_US    30000   // Timeout en microsegundos
#define PILL_DETECTION_THRESHOLD  5.0    // Distancia en cm para detectar píldoras
#define LIQUID_DETECTION_THRESHOLD 5.0  // Distancia en cm para detectar líquido
#define MIN_TIME_BETWEEN_READINGS_US 60000 // Tiempo mínimo entre lecturas (60ms)

// Definiciones para bomba
#define PUMP_FREQUENCY           500     // Frecuencia PWM para bomba (Hz)
#define PUMP_DUTY_CYCLE_MIN      0       // Mínimo duty cycle (apagado)
#define PUMP_DUTY_CYCLE_MAX       50     // Máximo duty cycle (para dispensación)
#define LIQUID_DISPENSE_BASE_TIME 1000   // Tiempo base en ms para dispensar líquido

// Definiciones para dispensación de píldoras
#define PILL_DISPENSE_BASE_TIME   1000    // Tiempo base en ms por píldora
#define PILL_DISPENSE_TIME_PER_PILL 1000  // Tiempo adicional en ms por píldora extra

// Tiempos de espera
#define CONTAINER_WAIT_TIMEOUT_MS 60000  // Tiempo máximo de espera para recipiente (60s)
#define CONTAINER_CHECK_INTERVAL_MS 1000  // Intervalo de verificación para recipiente (1s)

// Variables para el control de hardware
static bool hardware_initialized = false;
static mcpwm_unit_t servo_mcpwm_unit = MCPWM_UNIT_0;
static mcpwm_unit_t pump_mcpwm_unit = MCPWM_UNIT_1;

// Definir un temporizador y una estructura para los parámetros
static esp_timer_handle_t pump_timer = NULL;
static struct {
    bool in_use;
} pump_context = {0};

// Añadir variables para controlar el tiempo entre lecturas
static int64_t last_pill_reading_time = 0;
static int64_t last_liquid_reading_time = 0;

// Callback del temporizador para apagar la bomba
static void pump_timer_callback(void* arg) {
    medication_hardware_pump_stop();
    pump_context.in_use = false;
    ESP_LOGI(TAG, "Bomba detenida automáticamente por temporizador");
}

// Reemplazar en la función measure_distance
float measure_distance(uint8_t trigger_pin, uint8_t echo_pin) {
    // Enviar un pulso de 10us al sensor
    gpio_set_level(trigger_pin, 1);
    esp_rom_delay_us(10);
    gpio_set_level(trigger_pin, 0);

    // Medir el tiempo hasta recibir el eco
    int64_t timeout_start = esp_timer_get_time();
    int64_t start_time = 0;
    
    // Esperar a que el pin ECHO se ponga en alto
    while (gpio_get_level(echo_pin) == 0) {
        // Verificar timeout
        if ((esp_timer_get_time() - timeout_start) > ULTRASONIC_TIMEOUT_US) {
            ESP_LOGW(TAG, "Timeout esperando señal ECHO alta");
            return -1;
        }
    }
    
    // Capturar el tiempo de inicio cuando ECHO se pone en alto
    start_time = esp_timer_get_time();
    
    // Esperar a que el pin ECHO se ponga en bajo
    while (gpio_get_level(echo_pin) == 1) {
        // Verificar timeout
        if ((esp_timer_get_time() - start_time) > ULTRASONIC_TIMEOUT_US) {
            ESP_LOGW(TAG, "Timeout esperando señal ECHO baja");
            return -1;
        }
    }
    
    // Capturar el tiempo final cuando ECHO se pone en bajo
    int64_t end_time = esp_timer_get_time();

    // Calcular la distancia en cm
    float distance = (end_time - start_time) * 0.034 / 2;
    return distance;
}

// Añadir esta función para verificar la alimentación de los servos
static bool check_servo_power_supply(void) {
    // Esta es una implementación básica de ejemplo
    // En un sistema real, deberías comprobar el voltaje de alimentación de los servos
    
    ESP_LOGI(TAG, "Verificando alimentación de servos");
    
    // Intentar mover cada servo un poco y verificar si hay alguna señal
    for (int i = 0; i < 3; i++) {
        mcpwm_timer_t timer = i;
        
        // Intentar un pequeño movimiento
        mcpwm_set_duty_in_us(servo_mcpwm_unit, timer, MCPWM_OPR_A, SERVO_MIN_PULSEWIDTH + 100);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        mcpwm_set_duty_in_us(servo_mcpwm_unit, timer, MCPWM_OPR_A, SERVO_MIN_PULSEWIDTH);
    }
    
    return true; // Asumimos que está bien por ahora
}

// Inicializar hardware de dispensación
esp_err_t medication_hardware_init(void) {
    if (hardware_initialized) {
        ESP_LOGW(TAG, "Hardware ya inicializado");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Inicializando hardware de dispensación");
    
    // Inicializar buzzer primero para poder dar feedback de error si algo falla
    buzzer_init();
    
    // 1. Configurar los pines de los sensores ultrasónicos
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << ULTRASONIC_PILL_TRIGGER) | (1ULL << ULTRASONIC_LIQ_TRIGGER);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << ULTRASONIC_PILL_ECHO) | (1ULL << ULTRASONIC_LIQ_ECHO);
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    
    // 2. Inicializar MCPWM para servos
    ESP_LOGI(TAG, "Configurando servomotores");
    mcpwm_gpio_init(servo_mcpwm_unit, MCPWM0A, SERVO_PIN_1);
    mcpwm_gpio_init(servo_mcpwm_unit, MCPWM1A, SERVO_PIN_2);
    mcpwm_gpio_init(servo_mcpwm_unit, MCPWM2A, SERVO_PIN_3);
    
    mcpwm_config_t servo_config;
    servo_config.frequency = 50;  // 50Hz para servos estándar
    servo_config.cmpr_a = 0;
    servo_config.cmpr_b = 0;
    servo_config.duty_mode = MCPWM_DUTY_MODE_0;
    servo_config.counter_mode = MCPWM_UP_COUNTER;
    
    mcpwm_init(servo_mcpwm_unit, MCPWM_TIMER_0, &servo_config);
    mcpwm_init(servo_mcpwm_unit, MCPWM_TIMER_1, &servo_config);
    mcpwm_init(servo_mcpwm_unit, MCPWM_TIMER_2, &servo_config);
    
    // Añadir espera para estabilización
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    // 3. Inicializar MCPWM para bomba
    ESP_LOGI(TAG, "Configurando bomba");
    mcpwm_gpio_init(pump_mcpwm_unit, MCPWM0A, PUMP_PIN);
    
    mcpwm_config_t pump_config;
    pump_config.frequency = PUMP_FREQUENCY;
    pump_config.cmpr_a = PUMP_DUTY_CYCLE_MIN;
    pump_config.cmpr_b = 0;
    pump_config.duty_mode = MCPWM_DUTY_MODE_0;
    pump_config.counter_mode = MCPWM_UP_COUNTER;
    
    mcpwm_init(pump_mcpwm_unit, MCPWM_TIMER_0, &pump_config);
    
    // Verificar alimentación antes de continuar
    if (!check_servo_power_supply()) {
        ESP_LOGE(TAG, "Problema detectado en alimentación de servos");
        buzzer_play_pattern(BUZZER_PATTERN_ERROR);
        // Podrías retornar error, pero permitimos continuar con advertencia
    }
    
    // Asegurarse que todos los dispositivos empiecen en posición segura
    medication_hardware_close_compartment(1);
    medication_hardware_close_compartment(2);
    medication_hardware_close_compartment(3);
    medication_hardware_pump_stop();
    
    hardware_initialized = true;
    ESP_LOGI(TAG, "Hardware de dispensación inicializado correctamente");
    
    // Sonido de confirmación
    buzzer_play_pattern(BUZZER_PATTERN_CONFIRM);
    
    // Test explícito de servomotores
    ESP_LOGI(TAG, "Probando servomotores...");
    for (int i = 1; i <= 3; i++) {
        ESP_LOGI(TAG, "Probando servo %d", i);
        
        // Abrir el compartimento
        ESP_LOGI(TAG, "  Abriendo compartimento %d", i);
        mcpwm_set_duty_in_us(servo_mcpwm_unit, i-1, MCPWM_OPR_A, SERVO_OPEN_POSITION);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        
        // Cerrar el compartimento
        ESP_LOGI(TAG, "  Cerrando compartimento %d", i);
        mcpwm_set_duty_in_us(servo_mcpwm_unit, i-1, MCPWM_OPR_A, SERVO_CLOSE_POSITION);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    ESP_LOGI(TAG, "Prueba de servomotores completada");
    ESP_LOGI(TAG, "Probando sensores ultrasónicos...");
    
    float pill_distance = measure_distance(ULTRASONIC_PILL_TRIGGER, ULTRASONIC_PILL_ECHO);
    ESP_LOGI(TAG, "Sensor de píldoras - distancia: %.2f cm", pill_distance);

    // Pequeña espera entre lecturas
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // Test sensor de líquido
    float liquid_distance = measure_distance(ULTRASONIC_LIQ_TRIGGER, ULTRASONIC_LIQ_ECHO);
    ESP_LOGI(TAG, "Sensor de líquido - distancia: %.2f cm", liquid_distance);

    ESP_LOGI(TAG, "Prueba de sensores completada");

    return ESP_OK;
}

// Abrir compartimento específico
esp_err_t medication_hardware_open_compartment(uint8_t compartment_number) {
    if (!hardware_initialized) {
        ESP_LOGE(TAG, "Hardware no inicializado");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (compartment_number < 1 || compartment_number > 3) {
        ESP_LOGE(TAG, "Número de compartimento inválido: %d", compartment_number);
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Abriendo compartimento %d", compartment_number);
    
    mcpwm_timer_t timer;
    mcpwm_operator_t operator = MCPWM_OPR_A;
    
    switch (compartment_number) {
        case 1:
            timer = MCPWM_TIMER_0;
            break;
        case 2:
            timer = MCPWM_TIMER_1;
            break;
        case 3:
            timer = MCPWM_TIMER_2;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }
    
    // Mover servo a posición abierta
    mcpwm_set_duty_in_us(servo_mcpwm_unit, timer, operator, SERVO_OPEN_POSITION);
    
    // Añadir retraso para que el servo complete el movimiento
    vTaskDelay(300 / portTICK_PERIOD_MS);
    
    return ESP_OK;
}

// Cerrar compartimento específico
esp_err_t medication_hardware_close_compartment(uint8_t compartment_number) {
    if (!hardware_initialized) {
        ESP_LOGE(TAG, "Hardware no inicializado");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (compartment_number < 1 || compartment_number > 3) {
        ESP_LOGE(TAG, "Número de compartimento inválido: %d", compartment_number);
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Cerrando compartimento %d", compartment_number);
    
    mcpwm_timer_t timer;
    mcpwm_operator_t operator = MCPWM_OPR_A;
    
    switch (compartment_number) {
        case 1:
            timer = MCPWM_TIMER_0;
            break;
        case 2:
            timer = MCPWM_TIMER_1;
            break;
        case 3:
            timer = MCPWM_TIMER_2;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }
    
    // Mover servo a posición cerrada
    mcpwm_set_duty_in_us(servo_mcpwm_unit, timer, operator, SERVO_CLOSE_POSITION);
    
    // Añadir retraso para que el servo complete el movimiento
    vTaskDelay(300 / portTICK_PERIOD_MS);
    
    return ESP_OK;
}

// Activar bomba usando temporizador
esp_err_t medication_hardware_pump_start(uint8_t duty_percent, uint32_t duration_ms) {
    if (!hardware_initialized) {
        ESP_LOGE(TAG, "Hardware no inicializado");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Validar parámetros
    if (duty_percent > 100) {
        duty_percent = 100;
    }
    
    // Detener temporizador anterior si existe
    if (pump_context.in_use && pump_timer) {
        esp_timer_stop(pump_timer);
    }
    
    ESP_LOGI(TAG, "Activando bomba con duty cycle %d%%", duty_percent);
    
    // Establecer duty cycle para la bomba
    mcpwm_set_duty(pump_mcpwm_unit, MCPWM_TIMER_0, MCPWM_OPR_A, duty_percent);
    mcpwm_set_duty_type(pump_mcpwm_unit, MCPWM_TIMER_0, MCPWM_OPR_A, MCPWM_DUTY_MODE_0);
    
    // Si se especificó un tiempo, configurar temporizador
    if (duration_ms > 0) {
        // Crear temporizador si no existe
        if (pump_timer == NULL) {
            esp_timer_create_args_t timer_args = {
                .callback = pump_timer_callback,
                .name = "pump_timer"
            };
            esp_err_t err = esp_timer_create(&timer_args, &pump_timer);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error creando temporizador para bomba");
                return err;
            }
        }
        
        // Iniciar temporizador
        pump_context.in_use = true;
        esp_timer_start_once(pump_timer, duration_ms * 1000);  // Convertir a microsegundos
        ESP_LOGI(TAG, "Bomba programada para detenerse en %lu ms", duration_ms);
    }
    
    return ESP_OK;
}

// Detener bomba
esp_err_t medication_hardware_pump_stop(void) {
    if (!hardware_initialized) {
        ESP_LOGE(TAG, "Hardware no inicializado");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Deteniendo bomba");
    
    // Establecer duty cycle a 0 para detener la bomba
    mcpwm_set_duty(pump_mcpwm_unit, MCPWM_TIMER_0, MCPWM_OPR_A, PUMP_DUTY_CYCLE_MIN);
    mcpwm_set_duty_type(pump_mcpwm_unit, MCPWM_TIMER_0, MCPWM_OPR_A, MCPWM_DUTY_MODE_0);
    
    return ESP_OK;
}

// Reemplazar en medication_hardware_check_pill_presence
sensor_state_t medication_hardware_check_pill_presence(void) {
    // Asegurar intervalo mínimo entre lecturas (60ms recomendado)
    int64_t current_time = esp_timer_get_time();
    if ((current_time - last_pill_reading_time) < MIN_TIME_BETWEEN_READINGS_US) {
        // Esperar hasta completar el intervalo mínimo
        int64_t wait_time = MIN_TIME_BETWEEN_READINGS_US - (current_time - last_pill_reading_time);
        esp_rom_delay_us(wait_time > 0 ? wait_time : 0);
    }
    
    // Realizar la medición
    float distance = measure_distance(ULTRASONIC_PILL_TRIGGER, ULTRASONIC_PILL_ECHO);
    last_pill_reading_time = esp_timer_get_time();
    
    if (distance < 0) {
        ESP_LOGW(TAG, "Error midiendo distancia en sensor de píldoras");
        return SENSOR_ERROR;
    }
    
    sensor_state_t state = (distance < PILL_DETECTION_THRESHOLD) ? OBJECT_PRESENT : OBJECT_NOT_PRESENT;
    ESP_LOGI(TAG, "Distancia sensor píldoras: %.2f cm - Píldora %s", 
             distance, (state == OBJECT_PRESENT) ? "detectada" : "no detectada");
    
    return state;
}

// Reemplazar en medication_hardware_check_liquid_presence
sensor_state_t medication_hardware_check_liquid_presence(void) {
    // Asegurar intervalo mínimo entre lecturas
    int64_t current_time = esp_timer_get_time();
    if ((current_time - last_liquid_reading_time) < MIN_TIME_BETWEEN_READINGS_US) {
        int64_t wait_time = MIN_TIME_BETWEEN_READINGS_US - (current_time - last_liquid_reading_time);
        esp_rom_delay_us(wait_time > 0 ? wait_time : 0);
    }
    
    float distance = measure_distance(ULTRASONIC_LIQ_TRIGGER, ULTRASONIC_LIQ_ECHO);
    last_liquid_reading_time = esp_timer_get_time();
    
    if (distance < 0) {
        ESP_LOGW(TAG, "Error midiendo distancia en sensor de líquido");
        return SENSOR_ERROR;
    }
    
    sensor_state_t state = (distance < LIQUID_DETECTION_THRESHOLD) ? OBJECT_PRESENT : OBJECT_NOT_PRESENT;
    ESP_LOGI(TAG, "Distancia sensor líquido: %.2f cm - Recipiente %s", 
             distance, (state == OBJECT_PRESENT) ? "detectado" : "no detectado");
    
    return state;
}

// Reemplazar en wait_for_container_with_alerts
sensor_state_t wait_for_container_with_alerts(bool is_liquid, uint32_t max_wait_time_ms) {
    const uint32_t check_interval_ms = CONTAINER_CHECK_INTERVAL_MS;
    uint32_t elapsed_time = 0;
    sensor_state_t container_state;
    
    ESP_LOGI(TAG, "Esperando recipiente para %s...", is_liquid ? "líquido" : "píldoras");
    
    // Bucle de espera activa
    while (elapsed_time < max_wait_time_ms) {
        // Verificar presencia del recipiente según el tipo
        if (is_liquid) {
            container_state = medication_hardware_check_liquid_presence();
        } else {
            container_state = medication_hardware_check_pill_presence();
        }
        
        // Si se detecta el recipiente, retornar éxito
        if (container_state == OBJECT_PRESENT) {
            ESP_LOGI(TAG, "Recipiente detectado, procediendo con dispensación");
            return OBJECT_PRESENT;
        }
        
        // Alertar al usuario que falta el recipiente
        ESP_LOGW(TAG, "No se detecta recipiente. Por favor, coloque un %s", 
                 is_liquid ? "vaso para líquido" : "recipiente para píldoras");
        buzzer_play_pattern(BUZZER_PATTERN_MEDICATION_READY);
        
        // Esperar el intervalo de verificación
        vTaskDelay(pdMS_TO_TICKS(check_interval_ms));
        elapsed_time += check_interval_ms;
    }
    
    // Si llegamos aquí, se agotó el tiempo de espera
    ESP_LOGW(TAG, "Tiempo de espera agotado. No se detectó recipiente");
    return OBJECT_NOT_PRESENT;
}

// Reemplazar en medication_hardware_dispense
esp_err_t medication_hardware_dispense(uint8_t compartment_number, bool is_liquid, uint32_t amount) {
    esp_err_t result = ESP_OK;
    
    if (!hardware_initialized) {
        ESP_LOGE(TAG, "Hardware no inicializado");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (compartment_number < 1 || compartment_number > 4) {
        ESP_LOGE(TAG, "Número de compartimento inválido: %d", compartment_number);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Si es líquido, debe ser el compartimento 4
    if (is_liquid) {
        // Código existente para dispensación de líquidos, no cambia
        if (compartment_number != LIQUID_COMPARTMENT_NUM) {
            ESP_LOGE(TAG, "El medicamento líquido solo puede dispensarse del compartimento 4");
            buzzer_play_pattern(BUZZER_PATTERN_ERROR);
            return ESP_ERR_INVALID_ARG;
        }
        
        // Esperar hasta 60 segundos (1 minuto) a que se coloque un recipiente para líquido
        if (wait_for_container_with_alerts(true, CONTAINER_WAIT_TIMEOUT_MS) != OBJECT_PRESENT) {
            buzzer_play_pattern(BUZZER_PATTERN_MEDICATION_MISSED);
            return ESP_ERR_TIMEOUT;
        }
        
        buzzer_play_pattern(BUZZER_PATTERN_CONFIRM);
        
        ESP_LOGI(TAG, "Dispensando medicamento líquido por %lu ms", (unsigned long)amount);
        result = medication_hardware_pump_start(PUMP_DUTY_CYCLE_MAX, amount);
        
        if (result == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(amount + 100));
            buzzer_play_pattern(BUZZER_PATTERN_MEDICATION_TAKEN);
        }
    } else {
        // Dispensación de píldoras - lógica modificada para dispensación incremental
        if (compartment_number > MAX_PILL_COMPARTMENTS) {
            ESP_LOGE(TAG, "Las píldoras solo pueden dispensarse de los compartimentos 1-3");
            buzzer_play_pattern(BUZZER_PATTERN_ERROR);
            return ESP_ERR_INVALID_ARG;
        }
        
        // Esperar hasta 60 segundos a que se coloque un recipiente para píldoras
        if (wait_for_container_with_alerts(false, CONTAINER_WAIT_TIMEOUT_MS) != OBJECT_PRESENT) {
            buzzer_play_pattern(BUZZER_PATTERN_MEDICATION_MISSED);
            return ESP_ERR_TIMEOUT;
        }
        
        buzzer_play_pattern(BUZZER_PATTERN_CONFIRM);
        
        ESP_LOGI(TAG, "Dispensando %lu píldoras del compartimento %d", (unsigned long)amount, compartment_number);
        
        // Dispensar cada píldora individualmente
        for (uint32_t i = 0; i < amount; i++) {
            ESP_LOGI(TAG, "Dispensando píldora %lu de %lu", (unsigned long)(i+1), (unsigned long)amount);
            
            // 1. Mover a posición abierta (180°) para liberar la píldora actual
            mcpwm_timer_t timer;
            mcpwm_operator_t operator = MCPWM_OPR_A;
            
            switch (compartment_number) {
                case 1: timer = MCPWM_TIMER_0; break;
                case 2: timer = MCPWM_TIMER_1; break;
                case 3: timer = MCPWM_TIMER_2; break;
                default: return ESP_ERR_INVALID_ARG;
            }
            
            // Abrir - girar a 180°
            ESP_LOGI(TAG, "  Abriendo compartimento para liberar píldora");
            mcpwm_set_duty_in_us(servo_mcpwm_unit, timer, operator, SERVO_MAX_PULSEWIDTH); // 180°
            vTaskDelay(PILL_DISPENSE_BASE_TIME / portTICK_PERIOD_MS);
            
            // 2. Volver a posición cerrada (0°) para recibir la siguiente píldora
            ESP_LOGI(TAG, "  Cerrando compartimento para recibir siguiente píldora");
            mcpwm_set_duty_in_us(servo_mcpwm_unit, timer, operator, SERVO_MIN_PULSEWIDTH); // 0°
            
            // Esperar a que la siguiente píldora caiga al hueco
            vTaskDelay(PILL_DISPENSE_TIME_PER_PILL / portTICK_PERIOD_MS);
            
            // Si no es la última píldora, añadir una pequeña pausa entre ciclos
            if (i < amount - 1) {
                vTaskDelay(200 / portTICK_PERIOD_MS);
            }
        }
        
        // Asegurarse de que el servo quede en posición cerrada (0°)
        result = medication_hardware_close_compartment(compartment_number);
        
        // Sonido de confirmación cuando termina
        if (result == ESP_OK) {
            buzzer_play_pattern(BUZZER_PATTERN_MEDICATION_TAKEN);
        }
    }
    
    return result;
}

// Liberar recursos del hardware
void medication_hardware_deinit(void) {
    if (!hardware_initialized) {
        return;
    }
    
    // Cerrar todos los compartimentos
    medication_hardware_close_compartment(1);
    medication_hardware_close_compartment(2);
    medication_hardware_close_compartment(3);
    medication_hardware_pump_stop();
    
    // Opcional: De-inicializar MCPWM si hay una API para ello
    // Actualmente ESP-IDF no proporciona una función mcpwm_deinit
    
    hardware_initialized = false;
    ESP_LOGI(TAG, "Hardware de dispensación deinicializado");
}

esp_err_t medication_hardware_alert_missed(void) {
    ESP_LOGW(TAG, "¡Alerta! Medicamento no tomado");
    
    // Reproducir sonido de alerta de medicamento no tomado
    buzzer_play_pattern(BUZZER_PATTERN_MEDICATION_MISSED);
    
    return ESP_OK;
}

// Añadir esta función para poder diagnosticar problemas desde el menú MQTT
esp_err_t medication_hardware_servo_diagnostic(void) {
    if (!hardware_initialized) {
        ESP_LOGE(TAG, "Hardware no inicializado");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Ejecutando diagnóstico de servomotores");
    
    // Test simple de cada servo: movimiento completo y luego regreso
    for (int i = 1; i <= 3; i++) {
        // Posición actual (cerrada)
        ESP_LOGI(TAG, "Servo %d - posición inicial", i);
        
        // Abrir completamente
        ESP_LOGI(TAG, "Servo %d - abriendo completamente", i);
        medication_hardware_open_compartment(i);
        vTaskDelay(1500 / portTICK_PERIOD_MS);
        
        // Cerrar completamente
        ESP_LOGI(TAG, "Servo %d - cerrando completamente", i);
        medication_hardware_close_compartment(i);
        vTaskDelay(1500 / portTICK_PERIOD_MS);
    }
    
    ESP_LOGI(TAG, "Diagnóstico de servomotores completado");
    return ESP_OK;
}