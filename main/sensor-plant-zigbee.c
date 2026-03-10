#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_zigbee_core.h"

/* --- CONFIG BEETLE C6 --- */
#define I2C_SDA_PIN          GPIO_NUM_19
#define I2C_SCL_PIN          GPIO_NUM_20
#define SOIL_ADC_CHAN        ADC_CHANNEL_4 
#define I2C_PORT             I2C_NUM_0
#define ENDPOINT_ID          1

static const char *TAG = "ZIGBEE_PLANT";

static adc_oneshot_unit_handle_t adc1_handle;
static i2c_master_dev_handle_t cht_handle;
static i2c_master_dev_handle_t lux_handle;

static uint8_t s_bat_v_01V = 0;         // 0.1V units
static uint8_t s_bat_pct_half = 0;      // 0.5% units

// --- Fonctions batterie ---
static int read_battery_adc_raw(void) {
    int raw = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_0, &raw)); // GPIO0
    return raw;
}

static int adc_raw_to_mv(int raw) {
    // Approximation non calibrée
    return (raw * 3300) / 4095;
}

static int get_battery_mv(void) {
    int raw = read_battery_adc_raw();
    int mv_adc = adc_raw_to_mv(raw);
    // Si diviseur de tension, adapter ici
    int vbat_mv = (int)((float)mv_adc * 2.0f); // Multiplie par 2 si diviseur VBAT/2
    ESP_LOGI(TAG, "BAT: raw=%d mv_adc=%d -> vbat=%dmV", raw, mv_adc, vbat_mv);
    return vbat_mv;
}

static uint8_t lipo_pct_from_mv(int vbat_mv) {
    if (vbat_mv <= 3400) return 0;
    if (vbat_mv >= 4200) return 100;
    int pct;
    if (vbat_mv < 3700) pct = (vbat_mv - 3400) * 65 / 300;
    else if (vbat_mv < 3900) pct = 65 + (vbat_mv - 3700) * 15 / 200;
    else if (vbat_mv < 4100) pct = 80 + (vbat_mv - 3900) * 12 / 200;
    else pct = 92 + (vbat_mv - 4100) * 8 / 100;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
}

/* --- LECTURE CAPTEURS --- */
/* --- LECTURE CAPTEURS (CORRIGÉE) --- */
static void read_sensors(int16_t *t, uint16_t *l, uint16_t *s) {
    uint8_t data_rx[6] = {0}; // Augmenté pour le buffer complet
    uint8_t cmd_meas_temp[] = {0x24, 0x00}; // Commande de mesure pour CHT832X
    uint8_t cmd_power_on = 0x01;
    uint8_t cmd_measure_lux = 0x10;

    // --- SECTION LUX (BH1750) ---
    i2c_master_transmit(lux_handle, &cmd_power_on, 1, -1);
    vTaskDelay(pdMS_TO_TICKS(10));
    i2c_master_transmit(lux_handle, &cmd_measure_lux, 1, -1);
    vTaskDelay(pdMS_TO_TICKS(200)); 

    if (i2c_master_receive(lux_handle, data_rx, 2, -1) == ESP_OK) {
        uint16_t raw_lux = (data_rx[0] << 8) | data_rx[1];
        *l = (uint16_t)(raw_lux / 1.2);
    } else {
        ESP_LOGW(TAG, "NACK Lux");
        *l = 0;
    }

    // --- SECTION TEMP (SEN0546 / CHT832X) ---
    // 1. Envoyer commande de mesure
    i2c_master_transmit(cht_handle, cmd_meas_temp, 2, -1);
    vTaskDelay(pdMS_TO_TICKS(50)); // Attendre la conversion

    // 2. Lire les 6 octets (Temp MSB, Temp LSB, CRC, Hum MSB, Hum LSB, CRC)
    if (i2c_master_receive(cht_handle, data_rx, 6, -1) == ESP_OK) {
        uint16_t raw_temp = (data_rx[0] << 8) | data_rx[1];
        // Formule correcte SEN0546 : T = -45 + 175 * (raw / 65535)
        float temp_c = -45.0 + 175.0 * ((float)raw_temp / 65535.0);
        *t = (int16_t)(temp_c * 100); // Zigbee 0.01°C
    } else {
        ESP_LOGW(TAG, "NACK Temp");
        *t = -4000; // Valeur par défaut erreur
    }

    // --- SECTION ADC (SEN0308) ---
   /* int adc_raw = 0;
    if (adc_oneshot_read(adc1_handle, SOIL_ADC_CHAN, &adc_raw) == ESP_OK) {
        // Calibration basée sur tes logs (1120 en l'air)
        int val_air = 1120;   
        int val_eau = 500;   // À ajuster en trempant dans l'eau
        int perc = (val_air - adc_raw) * 10000 / (val_air - val_eau);
        if (perc < 0) perc = 0;
        if (perc > 10000) perc = 10000;
        *s = (uint16_t)perc;
    }*/

   int adc_raw = 0;
if (adc_oneshot_read(adc1_handle, SOIL_ADC_CHAN, &adc_raw) == ESP_OK) {
    // Calibration SEN0546 :
    // Mesure dans l'air (val_air) et dans l'eau (val_eau) pour obtenir les valeurs réelles
    int val_air = 2944;   // Mesure réelle en l'air
    int val_eau = 256;    // Mesure réelle dans l'eau

    // Formule : 0% = eau, 100% = air
    float ratio = (float)(adc_raw - val_eau) / (float)(val_air - val_eau);
    int perc = (int)(ratio * 10000);
    if (perc < 0) perc = 0;
    if (perc > 10000) perc = 10000;
    *s = (uint16_t)perc;

    ESP_LOGI(TAG, "SOL RAW: %d | PERC: %.2f%%", adc_raw, (float)perc/100.0);
    // Pour calibrer :
    // 1. Place la sonde dans l'air, note adc_raw => val_air
    // 2. Place la sonde dans l'eau, note adc_raw => val_eau
    // 3. Mets à jour les valeurs ci-dessus
}

}


/* --- ZIGBEE CALLBACKS --- */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    ESP_LOGI(TAG, "Zigbee signal handler: type=0x%x", (unsigned int)sig_type);
    if (sig_type == ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP) {
        ESP_LOGI(TAG, "Zigbee: SKIP_STARTUP, lancement du commissioning (network steering)");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
    } else {
        ESP_LOGI(TAG, "Zigbee: autre signal 0x%x", (unsigned int)sig_type);
    }
}

/* --- MAIN TASK --- */
static void zigbee_task_runner(void *pvParameters) {
    // 1. Initialisation Configuration
    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,
        .install_code_policy = false,
        .nwk_cfg.zed_cfg = {
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,
            .keep_alive = 3000,
        },
    };
    esp_zb_init(&zb_nwk_cfg);

    // 2. Clusters
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();

    // Cluster Basic
    esp_zb_basic_cluster_cfg_t basic_cfg = {0};
    basic_cfg.zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE;
    basic_cfg.power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_BATTERY;
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, "ECHOME");
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, "SoilBeetleC6");
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Cluster Power Configuration
    esp_zb_attribute_list_t *pwr = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG);
    esp_zb_power_config_cluster_add_attr(pwr, 0x0020, &s_bat_v_01V);
    esp_zb_power_config_cluster_add_attr(pwr, 0x0021, &s_bat_pct_half);
    esp_zb_cluster_list_add_power_config_cluster(cluster_list, pwr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Cluster Temperature
    esp_zb_temperature_meas_cluster_cfg_t t_cfg = {.measured_value = 0, .min_value = -4000, .max_value = 12500};
    esp_zb_cluster_list_add_temperature_meas_cluster(cluster_list, esp_zb_temperature_meas_cluster_create(&t_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    
    // Cluster Illuminance
    esp_zb_illuminance_meas_cluster_cfg_t l_cfg = {.measured_value = 0, .min_value = 0, .max_value = 0xFFFF};
    esp_zb_cluster_list_add_illuminance_meas_cluster(cluster_list, esp_zb_illuminance_meas_cluster_create(&l_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Cluster Humidity
    esp_zb_humidity_meas_cluster_cfg_t s_cfg = {.measured_value = 0, .min_value = 0, .max_value = 10000};
    esp_zb_cluster_list_add_humidity_meas_cluster(cluster_list, esp_zb_humidity_meas_cluster_create(&s_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // 3. Endpoint
    esp_zb_endpoint_config_t ep_config = {
        .endpoint = ENDPOINT_ID,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_ep_list_add_ep(ep_list, cluster_list, ep_config);

    esp_zb_device_register(ep_list);
    //esp_zb_set_primary_network_channel_set(0x07FFF800);
    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));
    ESP_LOGI(TAG, "Zigbee stack démarrée, attente des signaux...");
  //  vTaskDelay(pdMS_TO_TICKS(60000));

    while(1) {
        int16_t t=0; uint16_t l=0, s=0;
        read_sensors(&t, &l, &s);
        esp_zb_lock_acquire(portMAX_DELAY);
        esp_zb_zcl_set_attribute_val(ENDPOINT_ID, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, &t, false);
        esp_zb_zcl_set_attribute_val(ENDPOINT_ID, ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID, &l, false);
        esp_zb_zcl_set_attribute_val(ENDPOINT_ID, ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID, &s, false);
        // --- Mise à jour des attributs batterie ---
        int vbat_mv = get_battery_mv();
        s_bat_v_01V = (uint8_t)((vbat_mv + 50) / 100); // 0.1V
        s_bat_pct_half = (uint8_t)(lipo_pct_from_mv(vbat_mv) * 2); // 0.5%
        esp_zb_zcl_set_attribute_val(ENDPOINT_ID, ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, 0x0020, &s_bat_v_01V, false);
        esp_zb_zcl_set_attribute_val(ENDPOINT_ID, ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, 0x0021, &s_bat_pct_half, false);
        esp_zb_lock_release();
        ESP_LOGI(TAG, "Update -> T:%.2f L:%u S:%.2f", (float)t/100, l, (float)s/100);
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    
    // I2C Initialisation du Bus
    i2c_master_bus_handle_t bus_h;
    i2c_master_bus_config_t b_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT, 
        .i2c_port = I2C_PORT, 
        .scl_io_num = I2C_SCL_PIN, 
        .sda_io_num = I2C_SDA_PIN, 
        .glitch_ignore_cnt = 7, 
        .flags.enable_internal_pullup = true
        
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&b_cfg, &bus_h));
    vTaskDelay(pdMS_TO_TICKS(100)); 
    
    // Configuration commune pour les périphériques
    // AJOUT DE scl_speed_hz ICI pour corriger le crash
    i2c_device_config_t d_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, 
        .device_address = 0x44,        // CHT832X
        .scl_speed_hz = 100000,        // 100 kHz (standard)
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_h, &d_cfg, &cht_handle));

    

    // Mise à jour de l'adresse pour le deuxième capteur (M5-DLight)
    d_cfg.device_address = 0x23;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_h, &d_cfg, &lux_handle));

    // ADC Initialisation (SEN0308)
   /* adc_oneshot_unit_init_cfg_t a_init = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&a_init, &adc1_handle));
    adc_oneshot_chan_cfg_t a_ch = {.bitwidth = ADC_BITWIDTH_DEFAULT, .atten = ADC_ATTEN_DB_12};
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, SOIL_ADC_CHAN, &a_ch));*/

    adc_oneshot_unit_init_cfg_t a_init = {.unit_id = ADC_UNIT_1};
ESP_ERROR_CHECK(adc_oneshot_new_unit(&a_init, &adc1_handle));

// Correction : utiliser ADC_ATTEN_DB_12 pour lire toute la plage 0-3.3V
adc_oneshot_chan_cfg_t a_ch = {.bitwidth = ADC_BITWIDTH_DEFAULT, .atten = ADC_ATTEN_DB_12}; 
ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, SOIL_ADC_CHAN, &a_ch));


    // Configuration Radio Zigbee
    esp_zb_platform_config_t p_config = {
        .radio_config = {.radio_mode = ZB_RADIO_MODE_NATIVE},
        .host_config = {.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE}
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&p_config));

    // Enregistrement du signal handler Zigbee
    // esp_zb_set_app_signal_handler(esp_zb_app_signal_handler); // Supprimé, non nécessaire

    xTaskCreate(zigbee_task_runner, "zb_task", 4096, NULL, 5, NULL);
}
