idf_component_register(
    SRCS 
        "app_main.c"
        "wifi_provisioning.c"
        "mqtt/mqtt_app.c"
        "mqtt/mqtt_connection.c"
        "mqtt/mqtt_publication.c"
        "mqtt/mqtt_subscription.c"
        "medication/medication_storage.c"
        "medication/medication_dispenser.c"  # Corregida la ubicación
        "ntp_func.c"
    INCLUDE_DIRS 
        "."
        "mqtt"
        "medication"  # Añadido directorio de medicamentos para los includes
    REQUIRES 
        nvs_flash 
        esp_wifi 
        esp_netif 
        wifi_provisioning 
        protocomm 
        json 
        driver
        mqtt
        lwip  # Añadido para sntp (parte de lwip)
)
