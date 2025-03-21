idf_component_register(SRCS "app_main.c"
                            "mqtt_app.c" 
                      INCLUDE_DIRS "."
                      REQUIRES 
                      esp_wifi 
                      wifi_provisioning 
                      nvs_flash 
                      mqtt 
                      driver
                      json)
