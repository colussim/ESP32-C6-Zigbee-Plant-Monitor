#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>

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
#include "zdo/esp_zigbee_zdo_common.h"

#include "config.h"



static EventGroupHandle_t s_app_events;

static adc_oneshot_unit_handle_t adc1_handle;
static adc_cali_handle_t adc_cali_handle = NULL;
static bool adc_cali_enabled = false;

static i2c_master_dev_handle_t cht_handle;
static i2c_master_dev_handle_t lux_handle;

static uint8_t s_bat_v_01V = 0;      /* 0.1V units */
static uint8_t s_bat_pct_half = 0;   /* 0.5% units */
static bool s_binding_done = false;

/* -------------------- HELPERS -------------------- */
static void make_zcl_string(uint8_t *dst, size_t dst_size, const char *src)
{
    size_t len = strlen(src);
    if (len > 254) {
        len = 254;
    }
    if (len + 1 > dst_size) {
        len = dst_size - 1;
    }

    dst[0] = (uint8_t)len;
    memcpy(&dst[1], src, len);
}

static void setup_reporting(uint16_t cluster_id, uint16_t attr_id, uint8_t endpoint, uint16_t attr_type)
{
    esp_zb_zcl_reporting_info_t reporting_info = {0};

    reporting_info.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    reporting_info.ep = endpoint;
    reporting_info.cluster_id = cluster_id;
    reporting_info.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
    reporting_info.attr_id = attr_id;
    reporting_info.dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    reporting_info.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;

    reporting_info.u.send_info.min_interval = 1;
    reporting_info.u.send_info.max_interval = 300;
    reporting_info.u.send_info.def_min_interval = 1;
    reporting_info.u.send_info.def_max_interval = 300;

    switch (attr_type) {
        case ESP_ZB_ZCL_ATTR_TYPE_S16:
            reporting_info.u.send_info.delta.s16 = 1;
            break;
        case ESP_ZB_ZCL_ATTR_TYPE_U8:
            reporting_info.u.send_info.delta.u8 = 1;
            break;
        case ESP_ZB_ZCL_ATTR_TYPE_U16:
        default:
            reporting_info.u.send_info.delta.u16 = 1;
            break;
    }

    esp_zb_zcl_update_reporting_info(&reporting_info);

    esp_zb_zcl_attr_location_info_t attr_info = {
        .endpoint_id = endpoint,
        .cluster_id = cluster_id,
        .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        .attr_id = attr_id,
    };

    esp_err_t err = esp_zb_zcl_start_attr_reporting(attr_info);
    ESP_LOGI(TAG, "Reporting setup cluster=0x%04X attr=0x%04X -> %s",
             cluster_id, attr_id, esp_err_to_name(err));
}

static void report_attr(uint16_t cluster_id, uint16_t attr_id)
{
    esp_zb_zcl_report_attr_cmd_t report_cmd = {0};

    report_cmd.zcl_basic_cmd.dst_addr_u.addr_short = COORDINATOR_ADDR;
    report_cmd.zcl_basic_cmd.src_endpoint = ENDPOINT_ID;
    report_cmd.zcl_basic_cmd.dst_endpoint = COORDINATOR_EP;
    report_cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    report_cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    report_cmd.clusterID = cluster_id;
    report_cmd.attributeID = attr_id;

    esp_zb_zcl_report_attr_cmd_req(&report_cmd);
}

static void bind_cb(esp_zb_zdp_status_t zdo_status, void *user_ctx)
{
    esp_zb_zdo_bind_req_param_t *bind_req = (esp_zb_zdo_bind_req_param_t *)user_ctx;

    if (zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "Bind success for cluster 0x%04X", bind_req->cluster_id);
    } else {
        ESP_LOGW(TAG, "Bind failed for cluster 0x%04X status=%d", bind_req->cluster_id, zdo_status);
    }

    free(bind_req);
}

static void bind_cluster_to_coordinator(uint16_t cluster_id)
{
    esp_zb_zdo_bind_req_param_t *bind_req = calloc(1, sizeof(esp_zb_zdo_bind_req_param_t));
    if (!bind_req) {
        ESP_LOGE(TAG, "bind malloc failed");
        return;
    }

    bind_req->req_dst_addr = esp_zb_get_short_address();
    bind_req->src_endp = ENDPOINT_ID;
    bind_req->dst_endp = COORDINATOR_EP;
    bind_req->cluster_id = cluster_id;
    bind_req->dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED;

    esp_zb_ieee_address_by_short(COORDINATOR_ADDR, bind_req->dst_address_u.addr_long);
    esp_zb_get_long_address(bind_req->src_address);

    esp_zb_zdo_device_bind_req(bind_req, bind_cb, bind_req);
}

static void setup_bindings_once(void)
{
    if (s_binding_done) {
        return;
    }

    s_binding_done = true;
    ESP_LOGI(TAG, "Setting up bindings to coordinator");

    bind_cluster_to_coordinator(ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT);
    bind_cluster_to_coordinator(ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT);
    bind_cluster_to_coordinator(ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT);
    bind_cluster_to_coordinator(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG);
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
    int pct;
   if (vbat_mv <= 3400) {
        pct = 0;
    } else if (vbat_mv < 3600) {
        pct = (vbat_mv - 3400) * 20 / 200;   // 0 -> 20%
    } else if (vbat_mv < 3700) {
        pct = 20 + (vbat_mv - 3600) * 20 / 100;  // 20 -> 40%
    } else if (vbat_mv < 3900) {
        pct = 40 + (vbat_mv - 3700) * 25 / 200;  // 40 -> 65%
    } else if (vbat_mv < 4100) {
        pct = 65 + (vbat_mv - 3900) * 20 / 200;  // 65 -> 85%
    } else {
        pct = 85 + (vbat_mv - 4100) * 15 / 100;  // 85 -> 100%
    }

    if (pct < 0) 
        pct = 0;
    if (pct > 100) 
        pct = 100;
    return (uint8_t)pct;
}

/* -------------------- SENSORS -------------------- */
static void read_sensors(int16_t *t, uint16_t *l, uint16_t *s)
{
    uint8_t data_rx[6] = {0};
    uint8_t cmd_meas_temp[] = {0x24, 0x00};
    uint8_t cmd_power_on = 0x01;
    uint8_t cmd_reset = 0x07;
    uint8_t cmd_measure = 0x10; /* BH1750 continuous high-res */

    /* BH1750 */
    i2c_master_transmit(lux_handle, &cmd_power_on, 1, -1);
    vTaskDelay(pdMS_TO_TICKS(10));
    i2c_master_transmit(lux_handle, &cmd_reset, 1, -1);
    vTaskDelay(pdMS_TO_TICKS(10));
    i2c_master_transmit(lux_handle, &cmd_measure, 1, -1);
    vTaskDelay(pdMS_TO_TICKS(180));

    if (i2c_master_receive(lux_handle, data_rx, 2, -1) == ESP_OK) {
        uint16_t raw_lux = ((uint16_t)data_rx[0] << 8) | data_rx[1];
        float lux = raw_lux / 1.2f;

        uint16_t zigbee_lux = 0;
        if (lux >= 1.0f) {
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
        if (perc < 0) {
            perc = 0;
        }
        if (perc > 10000) {
            perc = 10000;
        }
        *s = (uint16_t)perc;
        ESP_LOGI(TAG, "SOL RAW: %d | PERC: %.2f%%", adc_raw, (float)perc / 100.0f);
    } else {
        *s = 0;
        ESP_LOGW(TAG, "ADC soil read failed");
    }
}

/* -------------------- ZIGBEE -------------------- */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    esp_err_t status = signal_struct->esp_err_status;

    ESP_LOGI(TAG, "Zigbee signal: %s (0x%x) status=%s",
             esp_zb_zdo_signal_to_string(sig_type),
             (unsigned int)sig_type,
             esp_err_to_name(status));

    switch (sig_type) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            ESP_LOGI(TAG, "Skip startup -> stack ready, start steering");
            xEventGroupSetBits(s_app_events, EVT_ZB_READY);
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            break;

#ifdef ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START
        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
            xEventGroupSetBits(s_app_events, EVT_ZB_READY);
            if (status == ESP_OK) {
                if (esp_zb_bdb_dev_joined()) {
                    ESP_LOGI(TAG, "First start OK and joined");
                    xEventGroupSetBits(s_app_events, EVT_ZB_JOINED);
                } else {
                    ESP_LOGI(TAG, "First start OK -> start steering");
                    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
                }
            } else {
                ESP_LOGW(TAG, "First start failed -> retry steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            }
            break;
#endif

#ifdef ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            xEventGroupSetBits(s_app_events, EVT_ZB_READY);
            if (status == ESP_OK && esp_zb_bdb_dev_joined()) {
                ESP_LOGI(TAG, "Device restored and joined network");
                xEventGroupSetBits(s_app_events, EVT_ZB_JOINED);
            } else {
                ESP_LOGW(TAG, "Device reboot restore failed -> start steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            }
            break;
#endif

#ifdef ESP_ZB_BDB_SIGNAL_STEERING
        case ESP_ZB_BDB_SIGNAL_STEERING:
            if (status == ESP_OK) {
                ESP_LOGI(TAG, "Network steering success");
                xEventGroupSetBits(s_app_events, EVT_ZB_JOINED);
            } else {
                ESP_LOGW(TAG, "Network steering failed");
            }
            break;
#endif

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
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, zb_vendor);
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, zb_model);
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

    setup_reporting(ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                    ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
                    ENDPOINT_ID,
                    ESP_ZB_ZCL_ATTR_TYPE_S16);
    setup_reporting(ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT,
                    ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID,
                    ENDPOINT_ID,
                    ESP_ZB_ZCL_ATTR_TYPE_U16);
    setup_reporting(ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                    ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID,
                    ENDPOINT_ID,
                    ESP_ZB_ZCL_ATTR_TYPE_U16);
    setup_reporting(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                    0x0020,
                    ENDPOINT_ID,
                    ESP_ZB_ZCL_ATTR_TYPE_U8);
    setup_reporting(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                    0x0021,
                    ENDPOINT_ID,
                    ESP_ZB_ZCL_ATTR_TYPE_U8);

    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));
    ESP_LOGI(TAG, "Zigbee stack started, entering main loop");

    xEventGroupSetBits(s_app_events, EVT_ZB_READY);
    esp_zb_stack_main_loop();
}

static void sensor_cycle_task(void *pvParameters)
{
    EventBits_t bits = xEventGroupWaitBits(
        s_app_events,
        EVT_ZB_READY,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(15000));

    if ((bits & EVT_ZB_READY) == 0) {
        ESP_LOGE(TAG, "Zigbee stack not ready");
        vTaskDelete(NULL);
        return;
    }

    bits = xEventGroupWaitBits(
        s_app_events,
        EVT_ZB_JOINED,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(MAX_AWAKE_FOR_JOIN_MS));

    if ((bits & EVT_ZB_JOINED) == 0) {
        ESP_LOGE(TAG, "Device not joined");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Joined OK, entering periodic sensor loop");
    vTaskDelay(pdMS_TO_TICKS(1000));

    esp_zb_lock_acquire(portMAX_DELAY);
    setup_bindings_once();
    esp_zb_lock_release();

    while (1) {
        int16_t t = 0;
        uint16_t l = 0;
        uint16_t s = 0;

        read_sensors(&t, &l, &s);

        int vbat_mv = get_battery_mv();
        uint8_t pct = lipo_pct_from_mv(vbat_mv);
        s_bat_v_01V = (uint8_t)((vbat_mv + 50) / 100);
        s_bat_pct_half = (uint8_t)(pct * 2);

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

        if (vbat_mv > 0) {
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
        }

        report_attr(ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                    ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID);
        report_attr(ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT,
                    ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID);
        report_attr(ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                    ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID);
        if (vbat_mv > 0) {
            report_attr(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG, 0x0020);
            report_attr(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG, 0x0021);
        }

        esp_zb_lock_release();

        ESP_LOGI(TAG,
                 "Published -> T:%.2f Lz:%u S:%.2f Bat:%dmV BatAttr:%u(0.1V) %u(0.5%%)",
                 (float)t / 100.0f,
                 l,
                 (float)s / 100.0f,
                 vbat_mv,
                 s_bat_v_01V,
                 s_bat_pct_half);

        ESP_LOGI(TAG, "Keeping radio awake for %d ms to let Zigbee transmit", TX_GRACE_MS);
        vTaskDelay(pdMS_TO_TICKS(TX_GRACE_MS));

        uint64_t interval_ms = SLEEP_INTERVAL_US / 1000ULL;
        if (interval_ms < TX_GRACE_MS) {
            interval_ms = TX_GRACE_MS + 1000ULL;
        }

        ESP_LOGI(TAG, "Waiting %llu ms before next measurement", interval_ms);
        vTaskDelay(pdMS_TO_TICKS((uint32_t)interval_ms));
    }
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
