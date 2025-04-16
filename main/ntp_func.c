#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/apps/sntp.h"
// Añadir estos nuevos includes:
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include <errno.h>

static const char *TAG = "NTP";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

// Manejador de eventos WiFi
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGE(TAG, "Conexión perdida. Reintentando...");
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init(const char *ssid, const char *password)
{
    esp_netif_init();
    esp_event_loop_create_default();
    nvs_flash_init();

    s_wifi_event_group = xEventGroupCreate(); // Crear grupo de eventos

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    // Copiar SSID y contraseña a la configuración WiFi
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Conectando a WiFi...");

    // Espera hasta que se conecte a WiFi
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "Conectado a WiFi.");
    }
    else
    {
        ESP_LOGE(TAG, "Error al conectar.");
    }
}

// Modificación a la función sync_ntp_time existente
bool sync_ntp_time(const char *timezone)
{
    static bool sntp_initialized = false;

    ESP_LOGI(TAG, "Inicializando NTP...");
    
    // Primero verificar conexión WiFi
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        ESP_LOGE(TAG, "No hay conexión WiFi activa");
        return false;
    }
    ESP_LOGI(TAG, "WiFi conectado a SSID: %s, RSSI: %d", ap_info.ssid, ap_info.rssi);
    
    // Si SNTP ya fue inicializado, detenerlo primero
    if (sntp_initialized) {
        ESP_LOGI(TAG, "SNTP ya inicializado, deteniendo primero...");
        sntp_stop();
        // Pequeña pausa para asegurar que se detenga completamente
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    
    // Inicializar SNTP
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    ESP_LOGI(TAG, "Configurando servidores NTP...");
    sntp_setservername(0, "pool.ntp.org");
    sntp_setservername(1, "time.google.com");
    sntp_setservername(2, "time.cloudflare.com");
    
    sntp_init();
    ESP_LOGI(TAG, "SNTP inicializado, esperando respuesta de servidores");
    
    // Marcar como inicializado
    sntp_initialized = true;

    // Configurar zona horaria
    if (timezone != NULL) {
        setenv("TZ", timezone, 1);
    } else {
        // Zona horaria por defecto (GMT-4)
        setenv("TZ", "EST4", 1);
    }
    tzset();

    // Esperar sincronización
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 10;  // Cambiado a 10 como en notas.txt

    while (timeinfo.tm_year < (2024 - 1900) && ++retry < retry_count)  // Año actualizado a 2024
    {
        ESP_LOGI(TAG, "Esperando sincronización NTP... (%d/%d)", retry, retry_count);
        
        vTaskDelay(2000 / portTICK_PERIOD_MS);  // Mayor tiempo de espera, 2 segundos como en notas.txt
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if (retry == retry_count)
    {
        ESP_LOGE(TAG, "Fallo al sincronizar NTP. Verifique conexión a Internet y/o firewalls.");
        return false;
    }

    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "Hora sincronizada: %s", strftime_buf);
    return true;
}

// Obtener timestamp actual en milisegundos
int64_t get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (tv.tv_usec / 1000);
}

// Obtener timestamp actual en segundos
time_t get_time_sec(void)
{
    time_t now;
    time(&now);
    return now;
}

// Formatear fecha/hora actual en una cadena personalizada
void format_current_time(char *buffer, size_t buffer_size, const char *format)
{
    time_t now;
    struct tm timeinfo;
    
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(buffer, buffer_size, format, &timeinfo);
}

// Añadir esta implementación (al final del archivo, antes de ntp_init):
void format_time(int64_t timestamp_ms, char *buffer, size_t size) {
    time_t t = timestamp_ms / 1000;
    struct tm timeinfo;
    localtime_r(&t, &timeinfo);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &timeinfo);
}

// Añadir esta función después de format_time y antes de ntp_init
bool test_internet_connectivity(void) {
    // Crear un socket
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Error al crear socket: errno %d", errno);
        return false;
    }
    
    // Configurar dirección de Google DNS (8.8.8.8:53)
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr("8.8.8.8");
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(53);
    
    // Configurar timeout del socket
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    // Intentar conectar
    int err = connect(sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Error de conexión a Google DNS: errno %d", errno);
        close(sock);
        return false;
    }
    
    // Conexión exitosa
    ESP_LOGI(TAG, "Conexión a Internet verificada (alcance a 8.8.8.8:53)");
    close(sock);
    return true;
}

// Añadir después de test_internet_connectivity()

/**
 * @brief Intenta sincronizar el tiempo con múltiples intentos
 * 
 * @param timezone Zona horaria
 * @param max_attempts Número máximo de intentos
 * @return true si la sincronización fue exitosa
 */
bool sync_ntp_time_with_retry(const char *timezone, int max_attempts) {
    ESP_LOGI(TAG, "Iniciando sincronización NTP con %d intentos", max_attempts);
    
    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        ESP_LOGI(TAG, "Intento de sincronización NTP %d de %d", attempt, max_attempts);
        
        // Verificar conectividad a Internet antes de intentar
        if (attempt > 1) {  // Saltamos la primera vez para no retrasar el inicio
            if (!test_internet_connectivity()) {
                ESP_LOGW(TAG, "Sin conexión a Internet en intento %d. Esperando...", attempt);
                vTaskDelay(2000 / portTICK_PERIOD_MS);
                continue;
            }
        }
        
        // Intentar sincronizar
        if (sync_ntp_time(timezone)) {
            ESP_LOGI(TAG, "Sincronización NTP exitosa en intento %d", attempt);
            return true;
        }
        
        // Esperar antes del siguiente intento
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
    
    ESP_LOGW(TAG, "Todos los intentos de sincronización NTP fallaron");
    return false;
}

/**
 * @brief Establece una fecha/hora por defecto cuando NTP falla
 * 
 * @param timezone Zona horaria a configurar
 */
void set_default_time(const char *timezone) {
    ESP_LOGI(TAG, "Configurando hora por defecto");
    
    // Obtener tiempo actual para ver si ya está configurado
    time_t now;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // Si el año es menor a 2022, configurar una fecha por defecto
    if (timeinfo.tm_year < (2022 - 1900)) {
        struct tm default_time = {
            .tm_year = 2023 - 1900,  // Año 2023
            .tm_mon = 0,             // Enero
            .tm_mday = 1,            // Día 1
            .tm_hour = 12,           // 12:00
            .tm_min = 0,
            .tm_sec = 0
        };
        
        struct timeval tv = {
            .tv_sec = mktime(&default_time),
            .tv_usec = 0
        };
        
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "Hora por defecto configurada: 2023-01-01 12:00:00");
    }
    
    // Configurar zona horaria
    if (timezone != NULL) {
        setenv("TZ", timezone, 1);
    } else {
        setenv("TZ", "EST4", 1);
    }
    tzset();
    
    // Mostrar la hora configurada
    char time_buf[64];
    format_current_time(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S");
    ESP_LOGI(TAG, "Hora actual: %s", time_buf);
}

// Variable global para el estado de sincronización
static bool ntp_sync_successful = false;

/**
 * @brief Tarea para intentar sincronización NTP periódicamente
 * 
 * @param pvParameter Parámetro (no usado)
 */
void ntp_periodic_sync_task(void *pvParameter) {
    const char *timezone = (const char *)pvParameter;
    const int RETRY_INTERVAL_MS = 60000;  // 1 minuto entre reintentos
    const int DAILY_SYNC_INTERVAL_MS = 24 * 60 * 60 * 1000;  // 24 horas
    
    // Dar tiempo a que la red se estabilice
    vTaskDelay(10000 / portTICK_PERIOD_MS);
    
    // Bucle de reintento si no hay sincronización inicial
    while (!ntp_sync_successful) {
        ESP_LOGI(TAG, "Intentando sincronización NTP periódica");
        
        if (test_internet_connectivity()) {
            ntp_sync_successful = sync_ntp_time(timezone);
            
            if (ntp_sync_successful) {
                char time_buf[64];
                format_current_time(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S");
                ESP_LOGI(TAG, "Sincronización NTP exitosa. Hora actualizada: %s", time_buf);
            } else {
                ESP_LOGW(TAG, "Falló la sincronización NTP periódica");
            }
        } else {
            ESP_LOGW(TAG, "Sin conexión a Internet para sincronización NTP");
        }
        
        vTaskDelay(RETRY_INTERVAL_MS / portTICK_PERIOD_MS);
    }
    
    // Una vez sincronizado exitosamente, cambiar a sincronización diaria
    while (1) {
        vTaskDelay(DAILY_SYNC_INTERVAL_MS / portTICK_PERIOD_MS);
        
        ESP_LOGI(TAG, "Realizando sincronización NTP diaria");
        sync_ntp_time(timezone);
        
        char time_buf[64];
        format_current_time(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S");
        ESP_LOGI(TAG, "Hora sincronizada: %s", time_buf);
    }
}

// Ejemplo de función inicializadora para ser llamada desde app_main
void ntp_init(const char *ssid, const char *password, const char *timezone)
{
    wifi_init(ssid, password);
    sync_ntp_time(timezone);
}