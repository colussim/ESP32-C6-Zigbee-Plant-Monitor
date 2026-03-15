/*
 *  Configuration for ESP32-C6 Zigbee Plant Monitor
 *
 *  This file defines all the configuration parameters for the ESP32-C6 Zigbee Plant Monitor project.
 *  It includes pin assignments, I2C addresses, Zigbee settings, calibration values, and power management settings.
 *
*/


#ifndef CONFIG_H
#define CONFIG_H

/* -----------------------------------------------------------
 *  Logging
 * -----------------------------------------------------------*/
#define TAG                         "ZIGBEE_PLANT"

/* -----------------------------------------------------------
 *  I2C BUS
 * -----------------------------------------------------------*/
#define I2C_SDA_PIN                 GPIO_NUM_19
#define I2C_SCL_PIN                 GPIO_NUM_20
#define I2C_PORT                    I2C_NUM_0

/* -----------------------------------------------------------
 *  Zigbee
 * -----------------------------------------------------------*/
#define ENDPOINT_ID                 1

/* -----------------------------------------------------------
 *  I2C Sensors
 * -----------------------------------------------------------*/
#define TEMP_I2C_ADDR               0x44
#define LUX_I2C_ADDR                0x23

/* -----------------------------------------------------------
 *  ADC channels
 * -----------------------------------------------------------*/
#define SOIL_ADC_CHAN               ADC_CHANNEL_4
#define VBAT_ADC_CHAN               ADC_CHANNEL_0   /* GPIO0 / BAT on Beetle ESP32-C6 */

/* -----------------------------------------------------------
 *  Battery measurement
 * -----------------------------------------------------------*/
/* Adjust according to your multimeter if necessary */
#define VBAT_DIVIDER                2.0f 

/* -----------------------------------------------------------
 *  Soil moisture calibration
 * -----------------------------------------------------------*/
/* Calibrated values for your setup */
#define VAL_AIR                     3056
#define VAL_EAU                     298

/* -----------------------------------------------------------
 *  Power management
 * -----------------------------------------------------------*/
/* Sensor: wake up every 2 hour */
 #define SLEEP_INTERVAL_US           (120ULL * 60ULL * 1000000ULL)
//#define SLEEP_INTERVAL_US (5ULL * 60ULL * 1000000ULL) 

/* After publishing, allow time for the Zigbee stack to transmit before deep sleep */
#define TX_GRACE_MS                 60000 //15000

/* -----------------------------------------------------------
 *  Zigbee commissioning
 * -----------------------------------------------------------*/
/* If the device is not connected, stay awake longer for commissioning. */
#define JOIN_RETRY_DELAY_MS         5000
#define MAX_AWAKE_FOR_JOIN_MS       (3 * 60 * 1000)

/* During the first pairing, allow Zigbee2MQTT to complete the interview */
#define FIRST_JOIN_INTERVIEW_MS     (1 * 1000) //120

/* -----------------------------------------------------------
 *  Zigbee config
 * -----------------------------------------------------------*/
/* zigbeeModel and Vendor */
#define ZB_MODEL_ID                 "SoilSensor"
#define ZB_VENDOR_ID                "ECHOME"
#define COORDINATOR_ADDR 0x0000
#define COORDINATOR_EP   1

/* -----------------------------------------------------------
 *  Events
 * -----------------------------------------------------------*/
#define EVT_ZB_READY                BIT0
#define EVT_ZB_JOINED               BIT1

#endif
