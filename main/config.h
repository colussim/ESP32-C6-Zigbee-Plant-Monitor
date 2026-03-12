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
#define VBAT_DIVIDER                2.35f

/* -----------------------------------------------------------
 *  Soil moisture calibration
 * -----------------------------------------------------------*/
/* Calibrated values for your setup */
#define VAL_AIR                     3056
#define VAL_EAU                     298

/* -----------------------------------------------------------
 *  Power management
 * -----------------------------------------------------------*/
/* Sensor: wake up every 1 hour */
#define SLEEP_INTERVAL_US           (60ULL * 60ULL * 1000000ULL)

/* After publishing, allow time for the Zigbee stack to transmit before deep sleep */
#define TX_GRACE_MS                 15000

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

/* -----------------------------------------------------------
 *  Events
 * -----------------------------------------------------------*/
#define EVT_ZB_READY                BIT0
#define EVT_ZB_JOINED               BIT1

#endif
