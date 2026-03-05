#include <stdio.h>
#include <string.h>
#include <math.h>

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

/*
  Fixes for Zigbee2MQTT:
  - Battery was null because Power Configuration attributes (0x0020/0x0021) were not guaranteed to exist
    in the cluster created with empty cfg. Here we explicitly add them with static storage.
  - Sleepy end devices cannot answer Z2M "get" reads while asleep. During pairing/interview, keep the
    device awake for a grace period so Z2M can read attributes.
  - Then, rely on REPORTING: we update attributes and actively send reports before sleeping.

  You should NOT expect Z2M "get" to work when the device is sleeping.
*/

#define SENSOR_ENDPOINT           1

// ESP32-C6 ADC1 channels (verify pin mapping for your board)
#define SOIL_MOISTURE_ADC_CHAN    ADC_CHANNEL_4  // GPIO4
#define BATTERY_ADC_CHAN          ADC_CHANNEL_0  // GPIO0 (often VBAT monitor)

// Timing
#define DEEP_SLEEP_TIME_SEC       (120 * 60)      // 2 hours (change later)
#define PAIRING_GRACE_MS          (120 * 1000)   // keep awake 2 min after join for Z2M interview
#define AWAKE_AFTER_REPORT_MS     (3000)

// SEN0308 calibration (replace with your measured raw values)
static int DRY_RAW = 3000; // raw in air
static int WET_RAW = 1200; // raw in water

// Battery measurement tuning
// - VBAT is usually connected through a resistor divider to the ADC pin.
// - Default assumes VBAT/2 -> multiply by 2.
// - If your board uses a different divider, change this.
#define VBAT_DIVIDER_MULTIPLIER  (2.0f)
// Optional extra gain (keep 1.0 when ADC calibration is available)
#define VBAT_MV_GAIN             (1.0f)

// Zigbee ED behavior
#define INSTALLCODE_POLICY_ENABLE false
#define ED_AGING_TIMEOUT          ESP_ZB_ED_AGING_TIMEOUT_64MIN
#define ED_KEEP_ALIVE_MS          3000

#define ESP_ZB_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

// Config macros (match your headers: ZB_* names)
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

static const char *TAG = "plant_zb";
static volatile bool s_joined = false;
static uint32_t s_join_time_ms = 0;

// Frequency of measurements during the awake phase (ms)
#define SENSOR_POLL_MS 2000

static void sensor_update_and_report_cb(uint8_t param);
static void go_deep_sleep_cb(uint8_t param);

// ----------------- ADC -----------------
static adc_oneshot_unit_handle_t s_adc1 = NULL;
static adc_cali_handle_t s_adc1_cali = NULL;
static bool s_adc1_cali_inited = false;

static void adc_cali_init_once(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_bitwidth_t bitwidth)
{
    if (s_adc1_cali_inited) {
        return;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = bitwidth,
    };
    esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc1_cali);
    if (err == ESP_OK) {
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
    esp_err_t err = adc_cali_create_scheme_line_fitting(&cali_cfg, &s_adc1_cali);
    if (err == ESP_OK) {
        s_adc1_cali_inited = true;
        ESP_LOGI(TAG, "ADC cali: line fitting enabled");
        return;
    }
#endif

    ESP_LOGW(TAG, "ADC cali not available (using raw->mv approximation)");
}

static int adc_raw_to_mv(int raw)
{
    if (s_adc1_cali_inited && s_adc1_cali) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(s_adc1_cali, raw, &mv) == ESP_OK) {
            return mv;
        }
    }
    // Fallback approximation (uncalibrated)
    return (raw * 3300) / 4095;
}

static void adc_init_once(void) {
    if (s_adc1) return;

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

    // Init calibration for battery channel (best effort)
    adc_cali_init_once(ADC_UNIT_1, BATTERY_ADC_CHAN, chan_cfg.atten, chan_cfg.bitwidth);
}

static int adc_read_avg(adc_channel_t ch, int samples, int delay_ms) {
    int acc = 0;
    for (int i = 0; i < samples; i++) {
        int raw = 0;
        ESP_ERROR_CHECK(adc_oneshot_read(s_adc1, ch, &raw));
        acc += raw;
        if (delay_ms) vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    return acc / samples;
}

// RAW -> 0.01% (0..10000)
static uint16_t soil_pct_001_from_raw(int raw) {
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

static uint16_t read_soil_humidity_001(void) {
    int raw = adc_read_avg(SOIL_MOISTURE_ADC_CHAN, 32, 2);
    return soil_pct_001_from_raw(raw);
}

static int read_vbat_mv(void) {
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
    // y = y0 + (x-x0)*(y1-y0)/(x1-x0)
    return y0 + (int)(((int64_t)(x - x0) * (y1 - y0)) / (x1 - x0));
}

static uint8_t lipo_pct_from_mv(int vbat_mv) {
    // Heuristic LiPo curve (SOC estimate from voltage, under light load).
    // Note: when USB is connected and the charger is active, VBAT can be biased higher.
    // If you need a "true" SOC, you need coulomb counting; otherwise this keeps UI sane.

    if (vbat_mv <= 3400) return 0;
    if (vbat_mv >= 4200) return 100;

    int pct;
    if (vbat_mv < 3700) {
        // 3.40V..3.70V -> 0..65%
        pct = lerp_pct_int(vbat_mv, 3400, 3700, 0, 65);
    } else if (vbat_mv < 3900) {
        // 3.70V..3.90V -> 65..80%
        pct = lerp_pct_int(vbat_mv, 3700, 3900, 65, 80);
    } else if (vbat_mv < 4100) {
        // 3.90V..4.10V -> 80..92%
        pct = lerp_pct_int(vbat_mv, 3900, 4100, 80, 92);
    } else {
        // 4.10V..4.20V -> 92..100%
        pct = lerp_pct_int(vbat_mv, 4100, 4200, 92, 100);
    }

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
}

// ----------------- Zigbee commissioning retry callback (no cast) -----------------
static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask) {
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
}

// ----------------- Zigbee signal handler -----------------
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
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
            s_join_time_ms = (uint32_t) (esp_log_timestamp());
            esp_zb_scheduler_alarm(sensor_update_and_report_cb, 0, 100);
        } else {
            ESP_LOGW(TAG, "Network steering failed (status: %s), retry...", esp_err_to_name(status));
            esp_zb_scheduler_alarm(bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;

    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s",
                 esp_zb_zdo_signal_to_string(sig), sig, esp_err_to_name(status));
        break;
    }
}

// ----------------- Zigbee endpoint setup -----------------
// Keep attribute storage static (must stay valid)
static uint16_t s_humidity_001 = 0;      // 0.01% units
static uint8_t  s_bat_v_01V = 0;         // 0.1V units
static uint8_t  s_bat_pct_half = 0;      // 0.5% units

// One-shot report command templates (kept static in case the stack queues pointers)
static bool s_report_cmds_inited = false;
static esp_zb_zcl_report_attr_cmd_t s_report_hum_cmd;
static esp_zb_zcl_report_attr_cmd_t s_report_bat_v_cmd;
static esp_zb_zcl_report_attr_cmd_t s_report_bat_pct_cmd;
static bool s_bat_v_report_enabled = true;

static void init_report_cmd(esp_zb_zcl_report_attr_cmd_t *cmd, uint16_t cluster_id, uint16_t attr_id)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->zcl_basic_cmd.dst_endpoint = 1;
    cmd->zcl_basic_cmd.src_endpoint = SENSOR_ENDPOINT;
    cmd->address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd->zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;
    cmd->manuf_specific = 0;
    cmd->direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    cmd->dis_default_resp = 1;
    cmd->manuf_code = 0;
    cmd->clusterID = cluster_id;
    cmd->attributeID = attr_id;
}

static bool attr_exists(uint16_t cluster_id, uint16_t attr_id)
{
    return esp_zb_zcl_get_attribute(SENSOR_ENDPOINT,
                                   cluster_id,
                                   ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                   attr_id) != NULL;
}

static void zigbee_register_endpoints(void) {
    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();

    // Basic cluster
    esp_zb_basic_cluster_cfg_t basic_cfg = {0};
    basic_cfg.zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE;
    basic_cfg.power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_BATTERY;
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, "ECHOME");
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, "SoilBeetleC6");
    esp_zb_cluster_list_add_basic_cluster(cl, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Humidity cluster: use the standard create() so mandatory attrs (min/max) exist.
    // Some stack validation paths assume those attributes are present.
    esp_zb_humidity_meas_cluster_cfg_t hum_cfg = {
        .measured_value = ESP_ZB_ZCL_REL_HUMIDITY_MEASUREMENT_MEASURED_VALUE_DEFAULT,
        .min_value = ESP_ZB_ZCL_REL_HUMIDITY_MEASUREMENT_MIN_MEASURED_VALUE_MINIMUM,
        .max_value = ESP_ZB_ZCL_REL_HUMIDITY_MEASUREMENT_MAX_MEASURED_VALUE_MAXIMUM,
    };
    esp_zb_attribute_list_t *hum = esp_zb_humidity_meas_cluster_create(&hum_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_update_attr(hum,
                                              ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID,
                                              &s_humidity_001));
    esp_zb_cluster_list_add_humidity_meas_cluster(cl, hum, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Power Configuration cluster: explicitly add voltage + percentage attributes
    esp_zb_attribute_list_t *pwr = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG);
    // Attribute IDs: 0x0020 batteryVoltage (0.1V), 0x0021 batteryPercentageRemaining (0.5%)
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

// ----------------- Sleep -----------------
static void go_deep_sleep(void) {
    ESP_LOGI(TAG, "Deep sleep %d sec", DEEP_SLEEP_TIME_SEC);
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)DEEP_SLEEP_TIME_SEC * 1000000ULL));
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
}

static void go_deep_sleep_cb(uint8_t param) {
    (void)param;
    go_deep_sleep();
}

static void sensor_update_and_report_cb(uint8_t param) {

    (void)param;

    // Always re-enable voltage reporting for each cycle
    s_bat_v_report_enabled = true;

    if (!s_joined) {
        esp_zb_scheduler_alarm(sensor_update_and_report_cb, 0, SENSOR_POLL_MS);
        return;
    }

    uint16_t soil = read_soil_humidity_001();
    int vbat_mv = read_vbat_mv();
    uint8_t bat_pct = lipo_pct_from_mv(vbat_mv);

    s_humidity_001 = soil;
    s_bat_v_01V = (uint8_t)((vbat_mv + 50) / 100); // 0.1V
    s_bat_pct_half = (uint8_t)(bat_pct * 2);       // 0.5%

    ESP_LOGI(TAG, "Mesures: soil=%.2f%% VBAT=%dmV (%u%%)", s_humidity_001 / 100.0f, vbat_mv, bat_pct);

    esp_zb_zcl_set_attribute_val(SENSOR_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID,
                                 &s_humidity_001, false);

    esp_zb_zcl_set_attribute_val(SENSOR_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 0x0020, &s_bat_v_01V, false);

    esp_zb_zcl_set_attribute_val(SENSOR_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 0x0021, &s_bat_pct_half, false);

    // Report one-shot on the coordinator (useful if Z2M has not configured reporting)
    if (!s_report_cmds_inited) {
        init_report_cmd(&s_report_hum_cmd,
                        ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                        ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID);
        init_report_cmd(&s_report_bat_v_cmd,
                        ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                        0x0020);
        init_report_cmd(&s_report_bat_pct_cmd,
                        ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                        0x0021);
        s_report_cmds_inited = true;
    }

    esp_err_t rep_err;
    if (attr_exists(s_report_hum_cmd.clusterID, s_report_hum_cmd.attributeID)) {
        rep_err = esp_zb_zcl_report_attr_cmd_req(&s_report_hum_cmd);
        if (rep_err != ESP_OK) {
            ESP_LOGW(TAG, "Report humidity failed: %s", esp_err_to_name(rep_err));
        }
    } else {
        ESP_LOGW(TAG, "Skip report humidity (attr missing)");
    }
    if (s_bat_v_report_enabled) {
        if (attr_exists(s_report_bat_v_cmd.clusterID, s_report_bat_v_cmd.attributeID)) {
            rep_err = esp_zb_zcl_report_attr_cmd_req(&s_report_bat_v_cmd);
            if (rep_err == ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGW(TAG, "BatteryVoltage reporting not supported; disabling it");
                s_bat_v_report_enabled = false;
            } else if (rep_err != ESP_OK) {
                ESP_LOGW(TAG, "Report batt voltage failed: %s", esp_err_to_name(rep_err));
            }
        } else {
            ESP_LOGW(TAG, "Skip report batt voltage (attr missing)");
            s_bat_v_report_enabled = false;
        }
    }

    if (attr_exists(s_report_bat_pct_cmd.clusterID, s_report_bat_pct_cmd.attributeID)) {
        rep_err = esp_zb_zcl_report_attr_cmd_req(&s_report_bat_pct_cmd);
        if (rep_err != ESP_OK) {
            ESP_LOGW(TAG, "Report batt pct failed: %s", esp_err_to_name(rep_err));
        }
    } else {
        ESP_LOGW(TAG, "Skip report batt pct (attr missing)");
    }

    uint32_t now = (uint32_t)esp_log_timestamp();
    if (now - s_join_time_ms < PAIRING_GRACE_MS) {
        esp_zb_scheduler_alarm(sensor_update_and_report_cb, 0, SENSOR_POLL_MS);
    } else {
        esp_zb_scheduler_alarm(go_deep_sleep_cb, 0, AWAKE_AFTER_REPORT_MS);
    }
}

// ----------------- Zigbee task -----------------
static void zigbee_task(void *pv) {
    (void)pv;

    ESP_LOGI(TAG, "DEBUG: zigbee_task started");

    adc_init_once();

    esp_zb_cfg_t zb_cfg = ESP_ZB_ZED_CONFIG();
    esp_zb_init(&zb_cfg);

    zigbee_register_endpoints();

    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));

    // Zigbee loop (infinite)
    esp_zb_stack_main_loop();

    // Should never reach here
    vTaskDelete(NULL);
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());

    esp_zb_platform_config_t platform_cfg = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform_cfg));

    BaseType_t task_result = xTaskCreate(zigbee_task, "zigbee", 8192, NULL, 5, NULL);
    if (task_result == pdPASS) {
        ESP_LOGI(TAG, "DEBUG: zigbee_task started successfully");
    } else {
        ESP_LOGE(TAG, "ERROR: zigbee_task not started (xTaskCreate=%d)", task_result);
    }
}
