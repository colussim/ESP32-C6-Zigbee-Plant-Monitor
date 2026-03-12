#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_sleep.h"
#include "nvs_flash.h"

#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "esp_zigbee_core.h"

#include "config.h"


static EventGroupHandle_t s_app_events;

static adc_oneshot_unit_handle_t adc1_handle;
static adc_cali_handle_t adc_cali_handle = NULL;
static bool adc_cali_enabled = false;

static i2c_master_dev_handle_t cht_handle;
static i2c_master_dev_handle_t lux_handle;

static uint8_t s_bat_v_01V = 0;      /* 0.1V units */
static uint8_t s_bat_pct_half = 0;   /* 0.5% units */

/* -------------------- HELPER -------------------- */

static void make_zcl_string(uint8_t *dst, size_t dst_size, const char *src)
{
    size_t len = strlen(src);
    if (len > 254) len = 254;
    if (len + 1 > dst_size) len = dst_size - 1;

    dst[0] = (uint8_t)len;
    memcpy(&dst[1], src, len);
}

/* -------------------- BATTERY -------------------- */
static void battery_adc_init(void)
{
    adc_oneshot_unit_init_cfg_t a_init = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&a_init, &adc1_handle));

    adc_oneshot_chan_cfg_t a_ch = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, SOIL_ADC_CHAN, &a_ch));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, VBAT_ADC_CHAN, &a_ch));

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = VBAT_ADC_CHAN,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle) == ESP_OK) {
        adc_cali_enabled = true;
        ESP_LOGI(TAG, "ADC battery calibration enabled");
    } else {
        ESP_LOGW(TAG, "ADC battery calibration unavailable, fallback to raw conversion");
    }
#else
    ESP_LOGW(TAG, "ADC curve fitting calibration not supported by this build");
#endif
}

static int get_battery_mv(void)
{
    int raw = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, VBAT_ADC_CHAN, &raw));

    int mv_adc = 0;
    if (adc_cali_enabled) {
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc_cali_handle, raw, &mv_adc));
    } else {
        mv_adc = (raw * 3300) / 4095;
    }

    int vbat_mv = (int)(mv_adc * VBAT_DIVIDER);
    ESP_LOGI(TAG, "BAT: raw=%d mv_adc=%d vbat=%d", raw, mv_adc, vbat_mv);
    return vbat_mv;
}

static uint8_t lipo_pct_from_mv(int vbat_mv)
{
    if (vbat_mv <= 3400) return 0;
    if (vbat_mv >= 4200) return 100;

    int pct;
    if (vbat_mv < 3700) {
        pct = (vbat_mv - 3400) * 65 / 300;
    } else if (vbat_mv < 3900) {
        pct = 65 + (vbat_mv - 3700) * 15 / 200;
    } else if (vbat_mv < 4100) {
        pct = 80 + (vbat_mv - 3900) * 12 / 200;
    } else {
        pct = 92 + (vbat_mv - 4100) * 8 / 100;
    }

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
}

/* -------------------- SENSORS -------------------- */
static void read_sensors(int16_t *t, uint16_t *l, uint16_t *s)
{
    uint8_t data_rx[6] = {0};
    uint8_t cmd_meas_temp[] = {0x24, 0x00};
 

    if (i2c_master_receive(lux_handle, data_rx, 2, -1) == ESP_OK) {

    uint16_t raw_lux = (data_rx[0] << 8) | data_rx[1];
    float lux = raw_lux / 1.2f;

    uint16_t zigbee_lux;

    if (lux < 1.0f) {
        zigbee_lux = 0;
    } else {
        zigbee_lux = (uint16_t)(10000.0f * log10f(lux) + 1.0f);
    }

    *l = zigbee_lux;

    ESP_LOGI(TAG, "BH1750 raw=%u lux=%.2f zigbee=%u", raw_lux, lux, zigbee_lux);

} else {

    ESP_LOGW(TAG, "NACK Lux");
    *l = 0;

}

    /* CHT832X / compatible */
    i2c_master_transmit(cht_handle, cmd_meas_temp, 2, -1);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (i2c_master_receive(cht_handle, data_rx, 6, -1) == ESP_OK) {
        uint16_t raw_temp = (data_rx[0] << 8) | data_rx[1];
        float temp_c = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);
        *t = (int16_t)(temp_c * 100.0f);
        ESP_LOGI(TAG, "Temp: %.2f°C", temp_c);
    } else {
        ESP_LOGW(TAG, "NACK Temp");
        *t = -4000;
    }

    /* Soil ADC */
    int adc_raw = 0;
    if (adc_oneshot_read(adc1_handle, SOIL_ADC_CHAN, &adc_raw) == ESP_OK) {
        

        float ratio = (float)(VAL_AIR - adc_raw) / (float)(VAL_AIR - VAL_EAU);
        int perc = (int)(ratio * 10000.0f);
        if (perc < 0) perc = 0;
        if (perc > 10000) perc = 10000;
        *s = (uint16_t)perc;

        ESP_LOGI(TAG, "SOL RAW: %d | PERC: %.2f%%", adc_raw, (float)perc / 100.0f);
    } else {
        *s = 0;
        ESP_LOGW(TAG, "ADC soil read failed");
    }
}

/* -------------------- POWER -------------------- */
static void enter_deep_sleep_hourly(void)
{
    ESP_LOGI(TAG, "Preparing deep sleep for 1 hour");
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(SLEEP_INTERVAL_US));
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "Entering deep sleep now");
    esp_deep_sleep_start();
}

/* -------------------- ZIGBEE -------------------- */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    esp_err_t status = signal_struct->esp_err_status;

    ESP_LOGI(TAG, "Zigbee signal: type=0x%x status=%s", (unsigned int)sig_type, esp_err_to_name(status));

    switch (sig_type) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            ESP_LOGI(TAG, "Skip startup -> start commissioning");
            xEventGroupSetBits(s_app_events, EVT_ZB_READY);
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            break;

#ifdef ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START
        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
#endif
#ifdef ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
#endif
            xEventGroupSetBits(s_app_events, EVT_ZB_READY);
            if (status == ESP_OK && esp_zb_bdb_dev_joined()) {
                ESP_LOGI(TAG, "Device joined network");
                xEventGroupSetBits(s_app_events, EVT_ZB_JOINED);
            }
            break;

        default:
            if (esp_zb_bdb_dev_joined()) {
                xEventGroupSetBits(s_app_events, EVT_ZB_JOINED);
            }
            break;
    }
}

static void zigbee_task_runner(void *pvParameters)
{

    static uint8_t zb_model[32];
    static uint8_t zb_vendor[32];

    make_zcl_string(zb_model, sizeof(zb_model), ZB_MODEL_ID);
    make_zcl_string(zb_vendor, sizeof(zb_vendor), ZB_VENDOR_ID);

    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,
        .install_code_policy = false,
        .nwk_cfg.zed_cfg = {
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,
            .keep_alive = 3000,
        },
    };

    esp_zb_init(&zb_nwk_cfg);

    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();

    esp_zb_basic_cluster_cfg_t basic_cfg = {0};
    basic_cfg.zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE;
    basic_cfg.power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_BATTERY;
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,zb_vendor );
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,  zb_model);
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_attribute_list_t *pwr = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG);
    esp_zb_power_config_cluster_add_attr(pwr, 0x0020, &s_bat_v_01V);
    esp_zb_power_config_cluster_add_attr(pwr, 0x0021, &s_bat_pct_half);
    esp_zb_cluster_list_add_power_config_cluster(cluster_list, pwr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_temperature_meas_cluster_cfg_t t_cfg = {
        .measured_value = 0,
        .min_value = -4000,
        .max_value = 12500,
    };
    esp_zb_cluster_list_add_temperature_meas_cluster(
        cluster_list,
        esp_zb_temperature_meas_cluster_create(&t_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_illuminance_meas_cluster_cfg_t l_cfg = {
        .measured_value = 0,
        .min_value = 0,
        .max_value = 0xFFFF,
    };
    esp_zb_cluster_list_add_illuminance_meas_cluster(
        cluster_list,
        esp_zb_illuminance_meas_cluster_create(&l_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_humidity_meas_cluster_cfg_t s_cfg = {
        .measured_value = 0,
        .min_value = 0,
        .max_value = 10000,
    };
    esp_zb_cluster_list_add_humidity_meas_cluster(
        cluster_list,
        esp_zb_humidity_meas_cluster_create(&s_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t ep_config = {
        .endpoint = ENDPOINT_ID,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_ep_list_add_ep(ep_list, cluster_list, ep_config);
    esp_zb_device_register(ep_list);

    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));
    ESP_LOGI(TAG, "Zigbee stack started, entering main loop");

    xEventGroupSetBits(s_app_events, EVT_ZB_READY);
    esp_zb_stack_main_loop();
}

static void sensor_cycle_task(void *pvParameters)
{
    bool joined_on_boot = false;
    bool first_join_this_boot = false;

    EventBits_t bits = xEventGroupWaitBits(
        s_app_events,
        EVT_ZB_READY,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(15000));

    if ((bits & EVT_ZB_READY) == 0) {
        ESP_LOGW(TAG, "Zigbee stack not ready in time, retry on next boot");
        enter_deep_sleep_hourly();
    }

    /* if already connected to NVRAM, no need to wait for a new commissioning */
    joined_on_boot = esp_zb_bdb_dev_joined();
    if (joined_on_boot) {
        ESP_LOGI(TAG, "Device already joined on boot");
        xEventGroupSetBits(s_app_events, EVT_ZB_JOINED);
    }

    bits = xEventGroupWaitBits(
        s_app_events,
        EVT_ZB_JOINED,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(MAX_AWAKE_FOR_JOIN_MS));

    if ((bits & EVT_ZB_JOINED) == 0) {
        ESP_LOGW(TAG, "Device not joined after %d ms, sleeping and retrying next hour",
                 MAX_AWAKE_FOR_JOIN_MS);
        enter_deep_sleep_hourly();
    }

    first_join_this_boot = (!joined_on_boot && esp_zb_bdb_dev_joined());
    if (first_join_this_boot) {
        ESP_LOGI(TAG,
                 "First join detected, keeping device awake for %d ms so Zigbee2MQTT can finish interview",
                 FIRST_JOIN_INTERVIEW_MS);
        vTaskDelay(pdMS_TO_TICKS(FIRST_JOIN_INTERVIEW_MS));
    } else {
        /* Small margin to allow network stabilization */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Joined OK, reading sensors and publishing");

    int16_t t = 0;
    uint16_t l = 0;
    uint16_t s = 0;
    read_sensors(&t, &l, &s);

    int vbat_mv = get_battery_mv();
    float pct = (float)lipo_pct_from_mv(vbat_mv);
    s_bat_v_01V = (uint8_t)((vbat_mv + 50) / 100);
    s_bat_pct_half = (uint8_t)(pct * 2.0f);

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_set_attribute_val(
        ENDPOINT_ID,
        ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
        &t,
        false);

    esp_zb_zcl_set_attribute_val(
        ENDPOINT_ID,
        ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID,
        &l,
        false);

    esp_zb_zcl_set_attribute_val(
        ENDPOINT_ID,
        ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID,
        &s,
        false);

    esp_zb_zcl_set_attribute_val(
        ENDPOINT_ID,
        ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        0x0020,
        &s_bat_v_01V,
        false);

    esp_zb_zcl_set_attribute_val(
        ENDPOINT_ID,
        ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        0x0021,
        &s_bat_pct_half,
        false);
    esp_zb_lock_release();

    ESP_LOGI(TAG,
             "Published -> T:%.2f L:%u S:%.2f Bat:%dmV BatAttr:%u(0.1V) %u(0.5%%)",
             (float)t / 100.0f, l, (float)s / 100.0f, vbat_mv, s_bat_v_01V, s_bat_pct_half);

    /* Important: do not sleep immediately after set_attribute. */
    ESP_LOGI(TAG, "Keeping radio awake for %d ms to let Zigbee transmit", TX_GRACE_MS);
    vTaskDelay(pdMS_TO_TICKS(TX_GRACE_MS));

    enter_deep_sleep_hourly();
}

/* -------------------- MAIN -------------------- */
void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_app_events = xEventGroupCreate();
    assert(s_app_events != NULL);

    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Wake cause: %d", (int)wake_cause);

    i2c_master_bus_handle_t bus_h;
    i2c_master_bus_config_t b_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_PORT,
        .scl_io_num = I2C_SCL_PIN,
        .sda_io_num = I2C_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&b_cfg, &bus_h));

    i2c_device_config_t d_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TEMP_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_h, &d_cfg, &cht_handle));

    d_cfg.device_address = LUX_I2C_ADDR;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_h, &d_cfg, &lux_handle));

    battery_adc_init();

    esp_zb_platform_config_t p_config = {
        .radio_config = {.radio_mode = ZB_RADIO_MODE_NATIVE},
        .host_config = {.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE},
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&p_config));

    xTaskCreate(zigbee_task_runner, "zb_task", 8192, NULL, 5, NULL);
    xTaskCreate(sensor_cycle_task, "sensor_cycle", 6144, NULL, 4, NULL);
}
