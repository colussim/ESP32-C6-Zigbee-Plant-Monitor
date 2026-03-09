#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_attribute.h"
#include "esp_zigbee_cluster.h"
#include "platform/esp_zigbee_platform.h"

#define SENSOR_ENDPOINT 1
static const char *TAG = "PLANT_MONITOR";

static i2c_master_dev_handle_t temp_dev;
static i2c_master_dev_handle_t lux_dev;

/* --- INITIALISATION MATERIELLE --- */
static void init_hw(void) {
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = 7,
        .sda_io_num = 6,
        .glitch_ignore_cnt = 7,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    i2c_device_config_t dev_temp = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x44, .scl_speed_hz = 100000 };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_temp, &temp_dev));

    i2c_device_config_t dev_lux = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x23, .scl_speed_hz = 100000 };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_lux, &lux_dev));
}

/* --- LECTURE ET ENVOI --- */
static void read_and_send(void) {
    uint8_t data_rx[6];
    uint8_t cmd_t[] = {0x24, 0x00};
    
    // Température SEN0546
    if (i2c_master_transmit_receive(temp_dev, cmd_t, 2, data_rx, 6, -1) == ESP_OK) {
        int16_t val = (int16_t)((-45.0 + 175.0 * ((data_rx[0] << 8 | data_rx[1]) / 65535.0)) * 100);
        esp_zb_zcl_set_attribute_val(SENSOR_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, &val, false);
    }

    // Luminosité BH1750
    uint8_t cmd_l = 0x10;
    i2c_master_transmit(lux_dev, &cmd_l, 1, -1);
    vTaskDelay(pdMS_TO_TICKS(200));
    if (i2c_master_receive(lux_dev, data_rx, 2, -1) == ESP_OK) {
        uint16_t lux = (uint16_t)((data_rx[0] << 8 | data_rx[1]) / 1.2);
        uint16_t zb_lux = (lux > 0) ? (uint16_t)(10000 * log10(lux) + 1) : 0;
        esp_zb_zcl_set_attribute_val(SENSOR_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID, &zb_lux, false);
    }
}

/* --- ZIGBEE SIGNAL HANDLER --- */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_zb_app_signal_type_t sig_type = (esp_zb_app_signal_type_t)(*p_sg_p);
    
    // Correction : NWK_STEERING -> NETWORK_STEERING
    if (sig_type == ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP) {
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
    } else if (sig_type == ESP_ZB_BDB_SIGNAL_STEERING && signal_struct->esp_err_status == ESP_OK) {
        ESP_LOGI(TAG, "Connecté au réseau Zigbee !");
        read_and_send();
    }
}

/* --- TACHE ZIGBEE --- */
static void zigbee_task(void *pvParameters) {
    // Correction : Initialisation manuelle de la config End Device (ZED)
    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED, // End Device
        .install_code_policy = false,
        .nwk_cfg.zed_cfg = {
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,
            .keep_alive = 3000,
        },
    };
    esp_zb_init(&zb_nwk_cfg);

    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();
    esp_zb_temperature_meas_cluster_cfg_t t_cfg = {0};
    esp_zb_cluster_list_add_temperature_meas_cluster(cl, esp_zb_temperature_meas_cluster_create(&t_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    
    esp_zb_illuminance_meas_cluster_cfg_t l_cfg = {0};
    esp_zb_cluster_list_add_illuminance_meas_cluster(cl, esp_zb_illuminance_meas_cluster_create(&l_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = SENSOR_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID,
    };
    
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_ep_list_add_ep(ep_list, cl, ep_cfg);
    esp_zb_device_register(ep_list);
    
    // esp_zb_main_loop_iteration() est dépréciée mais toujours utilisable. 
    // Pour v5.4 on peut utiliser esp_zb_start(false) pour un démarrage propre.
    esp_zb_start(false);
    esp_zb_main_loop_iteration();
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    init_hw();
    
    esp_zb_platform_config_t config = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE }
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    xTaskCreate(zigbee_task, "zigbee_main", 8192, NULL, 5, NULL);
}
