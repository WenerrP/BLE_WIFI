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

// Función para sincronizar el tiempo con NTP
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
    
    // Inicializar SNTP con más opciones y diagnóstico
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    ESP_LOGI(TAG, "Configurando servidores NTP...");
    sntp_setservername(0, "pool.ntp.org");
    sntp_setservername(1, "time.google.com");
    sntp_setservername(2, "time.cloudflare.com");
    // Opcional: Habilitar logging de SNTP (si está disponible en la configuración)
    // sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    
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

    // Esperar sincronización con mejor diagnóstico
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 15;

    while (timeinfo.tm_year < (2022 - 1900) && ++retry < retry_count)
    {
        ESP_LOGI(TAG, "Esperando sincronización NTP... (%d/%d)", retry, retry_count);
        
        // Si es el 5to o 10mo intento, mostrar más diagnóstico
        if (retry == 5 || retry == 10) {
            // Reemplazar el código de tcpip_adapter por esp_netif
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    ESP_LOGI(TAG, "Diagnóstico - IP: " IPSTR ", GW: " IPSTR,
                            IP2STR(&ip_info.ip), IP2STR(&ip_info.gw));
                }
            }
            
            for (int i = 0; i < 3; i++) {
                if (sntp_getservername(i)) {
                    ESP_LOGI(TAG, "Servidor NTP %d: %s", i, sntp_getservername(i));
                }
            }
        }
        
        vTaskDelay(1000 / portTICK_PERIOD_MS);
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

// Ejemplo de función inicializadora para ser llamada desde app_main
void ntp_init(const char *ssid, const char *password, const char *timezone)
{
    wifi_init(ssid, password);
    sync_ntp_time(timezone);
}