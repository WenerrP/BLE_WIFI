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

static const char *TAG = "MED_HARDWARE";

// Configuración de pines
#define SERVO_PIN_1      33  // Compartimento 1 - píldoras
#define SERVO_PIN_2      32  // Compartimento 2 - píldoras
#define SERVO_PIN_3      35  // Compartimento 3 - píldoras
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

// Definiciones para bomba
#define PUMP_FREQUENCY           500    // Frecuencia PWM para bomba (Hz)
#define PUMP_DUTY_CYCLE_MIN      0       // Mínimo duty cycle (apagado)
#define PUMP_DUTY_CYCLE_MAX      50     // Máximo duty cycle (100%)

// Variables para el control de hardware
static bool hardware_initialized = false;
static mcpwm_unit_t servo_mcpwm_unit = MCPWM_UNIT_0;
static mcpwm_unit_t pump_mcpwm_unit = MCPWM_UNIT_1;

// Definir un temporizador y una estructura para los parámetros
static esp_timer_handle_t pump_timer = NULL;
static struct {
    bool in_use;
} pump_context = {0};

// Callback del temporizador para apagar la bomba
static void pump_timer_callback(void* arg) {
    medication_hardware_pump_stop();
    pump_context.in_use = false;
    ESP_LOGI(TAG, "Bomba detenida automáticamente por temporizador");
}

// Función corregida para medir distancia con sensor ultrasónico
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
        if ((esp_timer_get_time() - timeout_start) > 30000) {
            ESP_LOGW(TAG, "Timeout esperando señal ECHO alta");
            return -1;
        }
    }
    
    // Capturar el tiempo de inicio cuando ECHO se pone en alto
    start_time = esp_timer_get_time();
    
    // Esperar a que el pin ECHO se ponga en bajo
    while (gpio_get_level(echo_pin) == 1) {
        // Verificar timeout
        if ((esp_timer_get_time() - start_time) > 30000) {  // 30ms timeout
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

// Inicializar hardware de dispensación
esp_err_t medication_hardware_init(void) {
    if (hardware_initialized) {
        ESP_LOGW(TAG, "Hardware ya inicializado");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Inicializando hardware de dispensación");
    
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
    
    // Asegurarse que todos los dispositivos empiecen en posición segura
    medication_hardware_close_compartment(1);
    medication_hardware_close_compartment(2);
    medication_hardware_close_compartment(3);
    medication_hardware_pump_stop();
    
    hardware_initialized = true;
    ESP_LOGI(TAG, "Hardware de dispensación inicializado correctamente");
    
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

typedef enum {
    SENSOR_ERROR = -1,
    OBJECT_NOT_PRESENT = 0,
    OBJECT_PRESENT = 1
} sensor_state_t;

bool medication_hardware_check_pill_presence(void) {
    float distance = measure_distance(ULTRASONIC_PILL_TRIGGER, ULTRASONIC_PILL_ECHO);
    
    if (distance < 0) {
        ESP_LOGW(TAG, "Error midiendo distancia en sensor de píldoras");
        return SENSOR_ERROR;
    }
    
    sensor_state_t state = (distance < 5.0) ? OBJECT_PRESENT : OBJECT_NOT_PRESENT;
    ESP_LOGI(TAG, "Distancia sensor píldoras: %.2f cm - Píldora %s", 
             distance, (state == OBJECT_PRESENT) ? "detectada" : "no detectada");
    
    return state;
}

// Verificar si hay líquido debajo del dispensador
bool medication_hardware_check_liquid_presence(void) {
    float distance = measure_distance(ULTRASONIC_LIQ_TRIGGER, ULTRASONIC_LIQ_ECHO);
    
    // Verificar si la medición es válida
    if (distance < 0) {
        ESP_LOGW(TAG, "Error midiendo distancia en sensor de líquido");
        return false;
    }
    
    // Si la distancia es menor a 10cm, consideramos que hay un recipiente presente
    bool liquid_container_present = (distance < 10.0);
    ESP_LOGI(TAG, "Distancia sensor líquido: %.2f cm - Recipiente %s", 
             distance, liquid_container_present ? "detectado" : "no detectado");
    
    return liquid_container_present;
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