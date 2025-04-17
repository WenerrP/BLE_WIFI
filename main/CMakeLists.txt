idf_component_register(
    SRCS 
        "app_main.c"
        "wifi_provisioning.c"
        "mqtt/mqtt_app.c"
        "mqtt/mqtt_connection.c"
        "mqtt/mqtt_publication.c"
        "mqtt/mqtt_subscription.c"
        "medication/medication_storage.c"
        "medication/medication_dispenser.c" 
        "medication/medication_hardware.c"
        "ntp_func.c"
        "nextion_driver.c"
        "buzzer_driver.c"
    INCLUDE_DIRS 
        "."
        "mqtt"
        "medication" 
    REQUIRES 
        nvs_flash 
        esp_wifi 
        esp_netif 
        wifi_provisioning 
        protocomm 
        json 
        driver
        mqtt
        lwip 
)

target_compile_options(${COMPONENT_LIB} PRIVATE 
    -Os                          # Optimize for size
    -fno-jump-tables             # No jump tables
    -fno-tree-switch-conversion  # No switch conversion
    -ffunction-sections          # Put each function in separate section
    -fdata-sections              # Put each data item in separate section
    -mno-text-section-literals   # Place literals in flash instead of IRAM
)

target_link_options(${COMPONENT_LIB} PRIVATE 
    -Wl,--gc-sections            # Remove unused sections
)
