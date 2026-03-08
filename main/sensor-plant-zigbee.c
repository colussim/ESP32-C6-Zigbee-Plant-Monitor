#include "driver/i2c.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_sleep.h"
#include "nvs_flash.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

// Zigbee
#include "esp_zigbee_core.h"
#include "esp_zigbee_attribute.h"
#include "esp_zigbee_cluster.h"
#include "platform/esp_zigbee_platform.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_basic.h"
#include "zcl/esp_zigbee_zcl_humidity_meas.h"
#include "zcl/esp_zigbee_zcl_power_config.h"

#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_SCL_IO           8
#define I2C_MASTER_SDA_IO           9
#define I2C_MASTER_FREQ_HZ          100000
#define I2C_MASTER_TX_BUF_DISABLE   0
#define I2C_MASTER_RX_BUF_DISABLE   0

#define SEN0546_ADDR                0x44
#define U136_BH1750_ADDR            0x23

#define ZCL_CLUSTER_ID_TEMP_MEASUREMENT 0x0402
#define ZCL_CLUSTER_ID_ILLUMINANCE      0x0400

#define SENSOR_ENDPOINT           1
#define SOIL_MOISTURE_ADC_CHAN    ADC_CHANNEL_4
#define BATTERY_ADC_CHAN          ADC_CHANNEL_0

#define DEEP_SLEEP_TIME_SEC       (120 * 60)
#define PAIRING_GRACE_MS          (120 * 1000)
#define AWAKE_AFTER_REPORT_MS     (3000)

#define VBAT_DIVIDER_MULTIPLIER   (2.0f)
#define VBAT_MV_GAIN              (1.0f)

#define INSTALLCODE_POLICY_ENABLE false
#define ED_AGING_TIMEOUT          ESP_ZB_ED_AGING_TIMEOUT_64MIN
#define ED_KEEP_ALIVE_MS          3000

#define ESP_ZB_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

#define ESP_ZB_ZED_CONFIG()                 \
{                                           \
    .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,   \
    .install_code_policy = INSTALLCODE_POLICY_ENABLE, \
    .nwk_cfg = {                            \
        .zed_cfg = {                        \
            .ed_timeout = ED_AGING_TIMEOUT, \
            .keep_alive = ED_KEEP_ALIVE_MS, \
        },                                  \
    },                                      \
}

#define ESP_ZB_DEFAULT_RADIO_CONFIG()       \
{                                           \
    .radio_mode = ZB_RADIO_MODE_NATIVE,     \
}

#define ESP_ZB_DEFAULT_HOST_CONFIG()        \
{                                           \
    .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE, \
}

#define SENSOR_POLL_MS                2000
#define FIRST_SENSOR_POLL_DELAY_MS    2000

static const char *TAG = "plant_zb";
static volatile bool s_joined = false;
static uint32_t s_join_time_ms = 0;
static TaskHandle_t s_sensor_task_handle = NULL;

// Zigbee attribute storage
static uint16_t s_soil_humidity_001 = 0;   // keep old behavior: soil moisture published in humidity cluster
static int16_t  s_temperature_01C = 0;     // 0.01 °C
static uint16_t s_illuminance_lux = 0;     // lux
static uint8_t  s_bat_v_01V = 0;           // 0.1 V
static uint8_t  s_bat_pct_half = 0;        // 0.5 %

// Log-only value for air humidity from SEN0546
static uint16_t s_air_humidity_001 = 0;    // 0.01 %RH

static int DRY_RAW = 3000;
static int WET_RAW = 1200;

// ADC state
static adc_oneshot_unit_handle_t s_adc1 = NULL;
static adc_cali_handle_t s_adc1_cali = NULL;
static bool s_adc1_cali_inited = false;

// Forward declarations
static void i2c_master_init(void);
static void adc_init_once(void);
static int adc_read_avg(adc_channel_t ch, int samples, int delay_ms);
static int adc_raw_to_mv(int raw);
static uint16_t soil_pct_001_from_raw(int raw);
static uint16_t read_soil_humidity_001(void);
static int read_vbat_mv(void);
static int lerp_pct_int(int x, int x0, int x1, int y0, int y1);
static uint8_t lipo_pct_from_mv(int vbat_mv);

static esp_err_t sen0546_read_temp_hum(int16_t *temp_01C, uint16_t *hum_001);
static esp_err_t u136_read_lux(uint16_t *lux);
static void read_environment_sensors(void);
static void update_battery_values(int vbat_mv, uint8_t bat_pct);
static void log_sensor_values(uint16_t soil_001, int vbat_mv, uint8_t bat_pct);
static void update_zcl_attributes(uint16_t soil_001);

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask);
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct);
static void zigbee_register_endpoints(void);
static void go_deep_sleep(void);
static void go_deep_sleep_cb(uint8_t param);
static void schedule_next_sensor_cycle(void);
static void sensor_update_and_report_cb(uint8_t param);
static void sensor_task(void *pv);
static void zigbee_task(void *pv);

// ----------------- I2C -----------------
static void i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
        .clk_flags = 0,
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM,
                                       conf.mode,
                                       I2C_MASTER_RX_BUF_DISABLE,
                                       I2C_MASTER_TX_BUF_DISABLE,
                                       0));
}

static esp_err_t sen0546_read_temp_hum(int16_t *temp_01C, uint16_t *hum_001)
{
    uint8_t data[6];
    i2c_cmd_handle_t cmd;
    esp_err_t ret;

    // Start measurement: CHT832X command 0x24 0x00
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SEN0546_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0x24, true);
    i2c_master_write_byte(cmd, 0x00, true);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(60));

    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SEN0546_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, sizeof(data), I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        return ret;
    }

    uint16_t temp_raw = ((uint16_t)data[0] << 8) | data[1];
    uint16_t hum_raw  = ((uint16_t)data[3] << 8) | data[4];

    *temp_01C = (int16_t)(-4500 + ((17500 * (int32_t)temp_raw) / 65535));
    *hum_001  = (uint16_t)((10000 * (uint32_t)hum_raw) / 65535);
    return ESP_OK;
}

static esp_err_t u136_read_lux(uint16_t *lux)
{
    uint8_t data[2];
    i2c_cmd_handle_t cmd;
    esp_err_t ret;

    // Continuous H-Resolution Mode
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (U136_BH1750_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0x10, true);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(180));

    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (U136_BH1750_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, sizeof(data), I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        return ret;
    }

    uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    *lux = (uint16_t)(raw / 1.2f);
    return ESP_OK;
}

// ----------------- ADC -----------------
static void adc_cali_init_once(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_bitwidth_t bitwidth)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = bitwidth,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc1_cali) == ESP_OK) {
        s_adc1_cali_inited = true;
        ESP_LOGI(TAG, "ADC cali: curve fitting enabled");
        return;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .atten = atten,
        .bitwidth = bitwidth,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_adc1_cali) == ESP_OK) {
        s_adc1_cali_inited = true;
        ESP_LOGI(TAG, "ADC cali: line fitting enabled");
        return;
    }
#endif

    ESP_LOGW(TAG, "ADC cali not available (using raw->mv approximation)");
}

static int adc_raw_to_mv(int raw)
{
    if (s_adc1_cali_inited && s_adc1_cali != NULL) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(s_adc1_cali, raw, &mv) == ESP_OK) {
            return mv;
        }
    }
    return (raw * 3300) / 4095;
}

static void adc_init_once(void)
{
    if (s_adc1 != NULL) {
        return;
    }

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &s_adc1));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };

    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc1, SOIL_MOISTURE_ADC_CHAN, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc1, BATTERY_ADC_CHAN, &chan_cfg));

    adc_cali_init_once(ADC_UNIT_1, BATTERY_ADC_CHAN, chan_cfg.atten, chan_cfg.bitwidth);
}

static int adc_read_avg(adc_channel_t ch, int samples, int delay_ms)
{
    int acc = 0;
    for (int i = 0; i < samples; i++) {
        int raw = 0;
        ESP_ERROR_CHECK(adc_oneshot_read(s_adc1, ch, &raw));
        acc += raw;
        if (delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
    return acc / samples;
}

static uint16_t soil_pct_001_from_raw(int raw)
{
    float pct = 0.0f;

    if (DRY_RAW == WET_RAW) {
        pct = 0.0f;
    } else if (DRY_RAW > WET_RAW) {
        pct = ((float)(DRY_RAW - raw) * 100.0f) / (float)(DRY_RAW - WET_RAW);
    } else {
        pct = ((float)(raw - DRY_RAW) * 100.0f) / (float)(WET_RAW - DRY_RAW);
    }

    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;

    return (uint16_t)lroundf(pct * 100.0f);
}

static uint16_t read_soil_humidity_001(void)
{
    int raw = adc_read_avg(SOIL_MOISTURE_ADC_CHAN, 32, 2);
    return soil_pct_001_from_raw(raw);
}

static int read_vbat_mv(void)
{
    int raw = adc_read_avg(BATTERY_ADC_CHAN, 16, 2);
    int mv_adc = adc_raw_to_mv(raw);
    int vbat_mv = (int)lroundf((float)mv_adc * VBAT_DIVIDER_MULTIPLIER * VBAT_MV_GAIN);
    ESP_LOGI(TAG, "BAT: raw=%d mv_adc=%d -> vbat=%dmV", raw, mv_adc, vbat_mv);
    return vbat_mv;
}

static int lerp_pct_int(int x, int x0, int x1, int y0, int y1)
{
    if (x <= x0) return y0;
    if (x >= x1) return y1;
    return y0 + (int)(((int64_t)(x - x0) * (y1 - y0)) / (x1 - x0));
}

static uint8_t lipo_pct_from_mv(int vbat_mv)
{
    if (vbat_mv <= 3400) return 0;
    if (vbat_mv >= 4200) return 100;

    int pct;
    if (vbat_mv < 3700) {
        pct = lerp_pct_int(vbat_mv, 3400, 3700, 0, 65);
    } else if (vbat_mv < 3900) {
        pct = lerp_pct_int(vbat_mv, 3700, 3900, 65, 80);
    } else if (vbat_mv < 4100) {
        pct = lerp_pct_int(vbat_mv, 3900, 4100, 80, 92);
    } else {
        pct = lerp_pct_int(vbat_mv, 4100, 4200, 92, 100);
    }

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
}

// ----------------- Zigbee app signal -----------------
static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig = (esp_zb_app_signal_type_t)*p_sg_p;

    switch (sig) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack initialized");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (status == ESP_OK) {
            ESP_LOGI(TAG, "Start network steering");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGW(TAG, "Failed to init Zigbee (status: %s)", esp_err_to_name(status));
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (status == ESP_OK) {
            ESP_LOGI(TAG, "Joined network successfully");
            s_joined = true;
            s_join_time_ms = (uint32_t)esp_log_timestamp();
            esp_zb_scheduler_alarm(sensor_update_and_report_cb, 0, FIRST_SENSOR_POLL_DELAY_MS);
        } else {
            ESP_LOGW(TAG, "Network steering failed (status: %s), retry...", esp_err_to_name(status));
            esp_zb_scheduler_alarm(bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING,
                                   1000);
        }
        break;

    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s",
                 esp_zb_zdo_signal_to_string(sig), sig, esp_err_to_name(status));
        break;
    }
}

// ----------------- Zigbee endpoints -----------------
static void zigbee_register_endpoints(void)
{
    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();

    esp_zb_basic_cluster_cfg_t basic_cfg = {0};
    basic_cfg.zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE;
    basic_cfg.power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_BATTERY;
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, "ECHOME");
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, "SoilBeetleC6");
    esp_zb_cluster_list_add_basic_cluster(cl, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Keep humidity cluster mapped to soil moisture for backward compatibility
    esp_zb_humidity_meas_cluster_cfg_t hum_cfg = {
        .measured_value = ESP_ZB_ZCL_REL_HUMIDITY_MEASUREMENT_MEASURED_VALUE_DEFAULT,
        .min_value = ESP_ZB_ZCL_REL_HUMIDITY_MEASUREMENT_MIN_MEASURED_VALUE_MINIMUM,
        .max_value = ESP_ZB_ZCL_REL_HUMIDITY_MEASUREMENT_MAX_MEASURED_VALUE_MAXIMUM,
    };
    esp_zb_attribute_list_t *hum = esp_zb_humidity_meas_cluster_create(&hum_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_update_attr(hum,
                    ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID,
                    &s_soil_humidity_001));
    esp_zb_cluster_list_add_humidity_meas_cluster(cl, hum, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_temperature_meas_cluster_cfg_t temp_cfg = {
        .measured_value = ESP_ZB_ZCL_TEMP_MEASUREMENT_MEASURED_VALUE_DEFAULT,
        .min_value = ESP_ZB_ZCL_TEMP_MEASUREMENT_MIN_MEASURED_VALUE_MINIMUM,
        .max_value = ESP_ZB_ZCL_TEMP_MEASUREMENT_MAX_MEASURED_VALUE_MAXIMUM,
    };
    esp_zb_attribute_list_t *temp = esp_zb_temperature_meas_cluster_create(&temp_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_update_attr(temp,
                    ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
                    &s_temperature_01C));
    esp_zb_cluster_list_add_temperature_meas_cluster(cl, temp, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_illuminance_meas_cluster_cfg_t illu_cfg = {
        .measured_value = ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_DEFAULT,
        .min_value = ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MIN_MEASURED_VALUE_MIN_VALUE,
        .max_value = ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MAX_MEASURED_VALUE_MAX_VALUE,
    };
    esp_zb_attribute_list_t *illu = esp_zb_illuminance_meas_cluster_create(&illu_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_update_attr(illu,
                    ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID,
                    &s_illuminance_lux));
    esp_zb_cluster_list_add_illuminance_meas_cluster(cl, illu, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_attribute_list_t *pwr = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG);
    esp_zb_power_config_cluster_add_attr(pwr, 0x0020, &s_bat_v_01V);
    esp_zb_power_config_cluster_add_attr(pwr, 0x0021, &s_bat_pct_half);
    esp_zb_cluster_list_add_power_config_cluster(cl, pwr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = SENSOR_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };

    ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(ep_list, cl, ep_cfg));
    ESP_ERROR_CHECK(esp_zb_device_register(ep_list));
}

// ----------------- Runtime helpers -----------------
static void go_deep_sleep(void)
{
    ESP_LOGI(TAG, "Deep sleep %d sec", DEEP_SLEEP_TIME_SEC);
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)DEEP_SLEEP_TIME_SEC * 1000000ULL));
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
}

static void go_deep_sleep_cb(uint8_t param)
{
    (void)param;
    go_deep_sleep();
}

static void schedule_next_sensor_cycle(void)
{
    uint32_t now = (uint32_t)esp_log_timestamp();
    if (now - s_join_time_ms < PAIRING_GRACE_MS) {
        esp_zb_scheduler_alarm(sensor_update_and_report_cb, 0, SENSOR_POLL_MS);
    } else {
        esp_zb_scheduler_alarm(go_deep_sleep_cb, 0, AWAKE_AFTER_REPORT_MS);
    }
}

static void read_environment_sensors(void)
{
    esp_err_t ret = sen0546_read_temp_hum(&s_temperature_01C, &s_air_humidity_001);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SEN0546 read failed: %s", esp_err_to_name(ret));
    }

 /* ret = u136_read_lux(&s_illuminance_lux);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "U136 read failed: %s", esp_err_to_name(ret));
    }*/
}

static void update_battery_values(int vbat_mv, uint8_t bat_pct)
{
    s_bat_v_01V = (uint8_t)((vbat_mv + 50) / 100);
    s_bat_pct_half = (uint8_t)(bat_pct * 2);
}

static void log_sensor_values(uint16_t soil_001, int vbat_mv, uint8_t bat_pct)
{
    ESP_LOGI(TAG,
             "Mesures: soil=%.2f%% airHum=%.2f%% temp=%.2f°C lux=%u VBAT=%dmV (%u%%)",
             soil_001 / 100.0f,
             s_air_humidity_001 / 100.0f,
             s_temperature_01C / 100.0f,
             s_illuminance_lux,
             vbat_mv,
             bat_pct);
}

static void update_zcl_attributes(uint16_t soil_001)
{
    s_soil_humidity_001 = soil_001;

    esp_zb_zcl_set_attribute_val(SENSOR_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID,
                                 &s_soil_humidity_001,
                                 false);

    esp_zb_zcl_set_attribute_val(SENSOR_ENDPOINT,
                                 ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 0x0000,
                                 &s_temperature_01C,
                                 false);

    esp_zb_zcl_set_attribute_val(SENSOR_ENDPOINT,
                                 ZCL_CLUSTER_ID_ILLUMINANCE,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 0x0000,
                                 &s_illuminance_lux,
                                 false);

    esp_zb_zcl_set_attribute_val(SENSOR_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 0x0020,
                                 &s_bat_v_01V,
                                 false);

    esp_zb_zcl_set_attribute_val(SENSOR_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 0x0021,
                                 &s_bat_pct_half,
                                 false);
}

// ----------------- Tasks and callbacks -----------------
static void sensor_update_and_report_cb(uint8_t param)
{
    (void)param;

    if (!s_joined) {
        schedule_next_sensor_cycle();
        return;
    }

    if (s_sensor_task_handle != NULL) {
        xTaskNotifyGive(s_sensor_task_handle);
    } else {
        schedule_next_sensor_cycle();
    }
}

static void sensor_task(void *pv)
{
    (void)pv;

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!s_joined) {
            continue;
        }

        uint16_t soil = read_soil_humidity_001();
        int vbat_mv = read_vbat_mv();
        uint8_t bat_pct = lipo_pct_from_mv(vbat_mv);

        read_environment_sensors();
        update_battery_values(vbat_mv, bat_pct);
        log_sensor_values(soil, vbat_mv, bat_pct);

        esp_zb_lock_acquire(portMAX_DELAY);
        update_zcl_attributes(soil);
        schedule_next_sensor_cycle();
        esp_zb_lock_release();
    }
}

static void zigbee_task(void *pv)
{
    (void)pv;

    ESP_LOGI(TAG, "zigbee_task started");

    adc_init_once();

    esp_zb_cfg_t zb_cfg = ESP_ZB_ZED_CONFIG();
    esp_zb_init(&zb_cfg);

    zigbee_register_endpoints();
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));

    while (1) {
        esp_zb_stack_main_loop_iteration();
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    esp_zb_platform_config_t platform_cfg = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform_cfg));

    i2c_master_init();

    BaseType_t sensor_task_result = xTaskCreate(sensor_task,
                                                "sensor_task",
                                                4096,
                                                NULL,
                                                4,
                                                &s_sensor_task_handle);
    if (sensor_task_result == pdPASS) {
        ESP_LOGI(TAG, "sensor_task started");
    } else {
        ESP_LOGE(TAG, "sensor_task not started (xTaskCreate=%d)", sensor_task_result);
    }

    BaseType_t zigbee_task_result = xTaskCreate(zigbee_task,
                                                "zigbee",
                                                8192,
                                                NULL,
                                                5,
                                                NULL);
    if (zigbee_task_result == pdPASS) {
        ESP_LOGI(TAG, "zigbee_task created");
    } else {
        ESP_LOGE(TAG, "zigbee_task not started (xTaskCreate=%d)", zigbee_task_result);
    }

    vTaskDelete(NULL);
}
