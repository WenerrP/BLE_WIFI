#ifndef MQTT_MANAGER_CONFIG_H
#define MQTT_MANAGER_CONFIG_H

#include <stdint.h>

#define MQTT_BROKER_URI "mqtts://broker.emqx.io:8883"
#define MQTT_KEEPALIVE 120
#define MQTT_MAX_RETRY_COUNT 5
#define MQTT_LAST_WILL_TOPIC "/device/status"
#define MQTT_LAST_WILL_MESSAGE "offline"
#define MQTT_LAST_WILL_QOS 1
#define MQTT_LAST_WILL_RETAIN true
#define MQTT_RESPONSE_TOPIC "/response"
#define MQTT_USER "user"
#define MQTT_PASSWORD "password"

extern const uint8_t client_cert_pem_start[] asm("_binary_client_cert_pem_start");
extern const uint8_t client_cert_pem_end[]   asm("_binary_client_cert_pem_end");
extern const uint8_t client_key_pem_start[]  asm("_binary_client_key_pem_start");
extern const uint8_t client_key_pem_end[]    asm("_binary_client_key_pem_end");
extern const uint8_t mqtt_eclipseprojects_io_pem_start[] asm("_binary_mqtt_eclipseprojects_io_pem_start");

#endif
