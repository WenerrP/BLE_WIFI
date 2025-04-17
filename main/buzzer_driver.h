#ifndef BUZZER_DRIVER_H
#define BUZZER_DRIVER_H

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

// Definir el pin GPIO para el buzzer (ajustar según tu hardware)
#define BUZZER_GPIO_PIN       26    // Ajustar al pin utilizado

// Patrones de sonido predefinidos
typedef enum {
    BUZZER_PATTERN_STARTUP,           // Secuencia de inicio del sistema
    BUZZER_PATTERN_WIFI_CONNECTED,    // Conexión WiFi exitosa
    BUZZER_PATTERN_WIFI_FAILED,       // Fallo de conexión WiFi
    BUZZER_PATTERN_NTP_SUCCESS,       // Sincronización NTP exitosa
    BUZZER_PATTERN_MEDICATION_READY,  // Medicamento listo para tomar
    BUZZER_PATTERN_MEDICATION_TAKEN,  // Medicamento tomado correctamente
    BUZZER_PATTERN_MEDICATION_MISSED, // Medicamento no tomado (alarma)
    BUZZER_PATTERN_ERROR,             // Error general
    BUZZER_PATTERN_PROVISIONING,      // Modo provisioning WiFi activo
    BUZZER_PATTERN_CONFIRM            // Confirmación general
} buzzer_pattern_t;

// Inicializar el buzzer
void buzzer_init(void);

// Reproducir un patrón de sonido específico
void buzzer_play_pattern(buzzer_pattern_t pattern);

// Reproducir un tono simple
void buzzer_beep(uint32_t duration_ms);

// Reproducir un tono con frecuencia específica (emulada con PWM)
void buzzer_beep_with_frequency(uint32_t duration_ms, uint32_t freq_hz);

// Detener cualquier sonido actual
void buzzer_stop(void);

// Para uso avanzado: reproducir una secuencia personalizada
// El formato es una serie de números: duración1, pausa1, duración2, pausa2, ...
void buzzer_play_sequence(const uint32_t *sequence, size_t length);

#endif // BUZZER_DRIVER_H