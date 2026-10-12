/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

 #include <string.h>
 #include <freertos/FreeRTOS.h>
 #include <freertos/task.h>
 #include "driver/gpio.h"
 #include "esp_err.h"
 #include "esp_log.h"
 
 #include "esp_cam_sensor.h"
 #include "esp_cam_sensor_detect.h"
 #include "ov02c10_settings.h"
 #include "ov02c10.h"

 typedef struct {
    uint8_t dgain_fine; // digital gain fine
    uint8_t dgain_coarse; // digital gain coarse
    uint8_t analog_gain;
} ov02c10_gain_t;

typedef struct {
    uint32_t exposure_val;
    uint32_t exposure_max;
    uint32_t gain_index; // current gain index

    uint32_t vflip_en : 1;
    uint32_t hmirror_en : 1;
} ov02c10_para_t;

struct ov02c10_cam {
    ov02c10_para_t ov02c10_para;
};

#define OV02C10_VTS_MAX          0x46c // Max exposure is VTS-15
#define OV02C10_EXP_MAX_OFFSET   0x0f

// #define OV02C10_FETCH_EXP_M(val)     (((val) >> 4) & 0xFF)
// #define OV02C10_FETCH_EXP_L(val)     (((val) & 0xF) << 4)

#define OV02C10_FETCH_EXP_M(val)     (((val) & 0xFF00) >> 8)
#define OV02C10_FETCH_EXP_L(val)     (((val) & 0xFF))

#define OV02C10_FETCH_DGAIN_FINE_H(val)  (val >> 2)
#define OV02C10_FETCH_DGAIN_FINE_L(val)  (((val) & 0x01) << 6)

#define OV02C10_GROUP_HOLD_START        0x00
#define OV02C10_GROUP_HOLD_END          0x10
#define OV02C10_GROUP_HOLD_DELAY_FRAMES 0x11
 
 #define OV02C10_IO_MUX_LOCK(mux)
 #define OV02C10_IO_MUX_UNLOCK(mux)
 #define OV02C10_ENABLE_OUT_CLOCK(pin,clk)
 #define OV02C10_DISABLE_OUT_CLOCK(pin)

 #define EXPOSURE_V4L2_UNIT_US                   100
#define EXPOSURE_V4L2_TO_OV02C10(v, sf)          \
    ((uint32_t)(((double)v) * (sf)->fps * (sf)->isp_info->isp_v1_info.vts / (1000000 / EXPOSURE_V4L2_UNIT_US) + 0.5))
#define EXPOSURE_OV02C10_TO_V4L2(v, sf)          \
    ((int32_t)(((double)v) * 1000000 / (sf)->fps / (sf)->isp_info->isp_v1_info.vts / EXPOSURE_V4L2_UNIT_US + 0.5))
 
 #define OV02C10_PID         0x5602
 #define OV02C10_SENSOR_NAME "OV02C10"
 #define OV02C10_AE_TARGET_DEFAULT (0x50)
 
 #ifndef portTICK_RATE_MS
 #define portTICK_RATE_MS portTICK_PERIOD_MS
 #endif
 #define delay_ms(ms)  vTaskDelay((ms > portTICK_PERIOD_MS ? ms/ portTICK_PERIOD_MS : 1))
 #define OV02C10_SUPPORT_NUM CONFIG_CAMERA_OV02C10_MAX_SUPPORT


 static const uint8_t s_ov02c10_exp_min = 0x08;
 static const uint32_t s_limited_gain = CONFIG_CAMERA_OV02C10_ABSOLUTE_GAIN_LIMIT;
 static size_t s_limited_gain_index;

 static const char *TAG = "ov02c10";


// hassctrl: the gain is the analog gain alone, 1x to 15.5x in steps of 1/16: register 0x3508
// holds the whole part, 0x3509 the sixteenths in its high nibble (as the Linux driver writes
// gain << 4 to the pair); the digital gain stays at 1x from the init table. The vendor's tables
// (analog 1..6 taken for 1, 2, 4 ... 32x, digital 1.5..2x) did not match the sensor: at index
// 31 -> 32 the picture got 52 % brighter where the table said 1.6 %, and the exposure control
// jumped back and forth between the two.
#define OV02C10_ANALOG_GAIN_MIN 0x10 // 1x, in 1/16
static const uint32_t ov02c10_total_gain_val_map[] = {
    1000, 1062, 1125, 1188, 1250, 1312, 1375, 1438, 1500, 1562,
    1625, 1688, 1750, 1812, 1875, 1938, 2000, 2062, 2125, 2188,
    2250, 2312, 2375, 2438, 2500, 2562, 2625, 2688, 2750, 2812,
    2875, 2938, 3000, 3062, 3125, 3188, 3250, 3312, 3375, 3438,
    3500, 3562, 3625, 3688, 3750, 3812, 3875, 3938, 4000, 4062,
    4125, 4188, 4250, 4312, 4375, 4438, 4500, 4562, 4625, 4688,
    4750, 4812, 4875, 4938, 5000, 5062, 5125, 5188, 5250, 5312,
    5375, 5438, 5500, 5562, 5625, 5688, 5750, 5812, 5875, 5938,
    6000, 6062, 6125, 6188, 6250, 6312, 6375, 6438, 6500, 6562,
    6625, 6688, 6750, 6812, 6875, 6938, 7000, 7062, 7125, 7188,
    7250, 7312, 7375, 7438, 7500, 7562, 7625, 7688, 7750, 7812,
    7875, 7938, 8000, 8062, 8125, 8188, 8250, 8312, 8375, 8438,
    8500, 8562, 8625, 8688, 8750, 8812, 8875, 8938, 9000, 9062,
    9125, 9188, 9250, 9312, 9375, 9438, 9500, 9562, 9625, 9688,
    9750, 9812, 9875, 9938, 10000, 10062, 10125, 10188, 10250, 10312,
    10375, 10438, 10500, 10562, 10625, 10688, 10750, 10812, 10875, 10938,
    11000, 11062, 11125, 11188, 11250, 11312, 11375, 11438, 11500, 11562,
    11625, 11688, 11750, 11812, 11875, 11938, 12000, 12062, 12125, 12188,
    12250, 12312, 12375, 12438, 12500, 12562, 12625, 12688, 12750, 12812,
    12875, 12938, 13000, 13062, 13125, 13188, 13250, 13312, 13375, 13438,
    13500, 13562, 13625, 13688, 13750, 13812, 13875, 13938, 14000, 14062,
    14125, 14188, 14250, 14312, 14375, 14438, 14500, 14562, 14625, 14688,
    14750, 14812, 14875, 14938, 15000, 15062, 15125, 15188, 15250, 15312,
    15375, 15438, 15500,
};
 
 static const esp_cam_sensor_isp_info_t ov02c10_isp_info[] = {
     {
         .isp_v1_info = {
             .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
             .pclk = 81666700,
             .vts = 1164,
             .hts = 2280,
             .tline_ns = 28637, // 1e9 / (30 fps * vts): esp_cam_sensor 2.x
             .gain_def = 0x01,
             .exp_def = 0x46c,
             .bayer_type = ESP_CAM_SENSOR_BAYER_GBRG,
         }
     },
     {
         .isp_v1_info = {
             .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
             .pclk = 81666700,
            // .pclk = 88333333,
             .vts = 1164,
             .hts = 2280,
             .tline_ns = 28637, // 1e9 / (30 fps * vts): esp_cam_sensor 2.x
             .gain_def = 0x01,
             .exp_def = 0x46c,
             .bayer_type = ESP_CAM_SENSOR_BAYER_GBRG,
         }
     },
     {
         .isp_v1_info = {
             .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
            //  .pclk = 80000000,
             .pclk = 81666700,
             .vts = 2328,
             .hts = 1140,
             .tline_ns = 14318, // 1e9 / (30 fps * vts): esp_cam_sensor 2.x
             .gain_def = 0x01,
             .exp_def = 0x46c,
             .bayer_type = ESP_CAM_SENSOR_BAYER_GBRG,
         }
     },
 };
 
 static const esp_cam_sensor_format_t ov02c10_format_info[] = {
     {
         .name = "MIPI_1lane_24Minput_RAW10_1288x728_30fps",
         .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
         .port = ESP_CAM_SENSOR_MIPI_CSI,
         .xclk = 24000000,
         .width = 1288,
         .height = 728,
         .regs = ov02c10_input_24M_MIPI_1lane_raw10_1288x728_30fps,
         .regs_size = ARRAY_SIZE(ov02c10_input_24M_MIPI_1lane_raw10_1288x728_30fps),
         .fps = 30,
         .isp_info = &ov02c10_isp_info[0],
         .mipi_info = {
             .mipi_clk = OV02C10_MIPI_CSI_LINE_RATE_800x640_50FPS,
             .lane_num = 1,
             .line_sync_en = CONFIG_CAMERA_OV02C10_CSI_LINESYNC_ENABLE ? true : false,
         },
         .reserved = NULL,
     },
     {
         .name = "MIPI_1lane_24Minput_RAW10_1920x1080_30fps",
         .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
         .port = ESP_CAM_SENSOR_MIPI_CSI,
         .xclk = 24000000,
         .width = 1920,
         .height = 1080,
         .regs = ov02c10_input_24M_MIPI_1lane_raw10_1920x1080_30fps,
         .regs_size = ARRAY_SIZE(ov02c10_input_24M_MIPI_1lane_raw10_1920x1080_30fps),
         .fps = 30,
         .isp_info = &ov02c10_isp_info[1],
         .mipi_info = {
             .mipi_clk = OV02C10_MIPI_CSI_LINE_RATE_1920x1080_30FPS,
             .lane_num = 1,
             .line_sync_en = CONFIG_CAMERA_OV02C10_CSI_LINESYNC_ENABLE ? true : false,
         },
         .reserved = NULL,
     },
     {
         .name = "MIPI_2lane_24Minput_RAW10_1920x1080_30fps",
         .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
         .port = ESP_CAM_SENSOR_MIPI_CSI,
         .xclk = 24000000,
         .width = 1920,
         .height = 1080,
         .regs = ov02c10_input_24M_MIPI_2lane_raw10_1920x1080_30fps,
         .regs_size = ARRAY_SIZE(ov02c10_input_24M_MIPI_2lane_raw10_1920x1080_30fps),
         .fps = 30,
         .isp_info = &ov02c10_isp_info[2],
         .mipi_info = {
             .mipi_clk = OV02C10_MIPI_CSI_LINE_RATE_1920x1080_30FPS,
             .lane_num = 2,
             .line_sync_en = CONFIG_CAMERA_OV02C10_CSI_LINESYNC_ENABLE ? true : false,
         },
         .reserved = NULL,
     },
 };
 
 static esp_err_t ov02c10_read(esp_sccb_io_handle_t sccb_handle, uint16_t reg, uint8_t *read_buf)
 {
     return esp_sccb_transmit_receive_reg_a16v8(sccb_handle, reg, read_buf);
 }
 
 static esp_err_t ov02c10_write(esp_sccb_io_handle_t sccb_handle, uint16_t reg, uint8_t data)
 {
     return esp_sccb_transmit_reg_a16v8(sccb_handle, reg, data);
 }
 
 /* write a array of registers */
 static esp_err_t ov02c10_write_array(esp_sccb_io_handle_t sccb_handle, const ov02c10_reginfo_t *regarray)
 {
     int i = 0;
     esp_err_t ret = ESP_OK;
     while ((ret == ESP_OK) && regarray[i].reg != OV02C10_REG_END) {
         if (regarray[i].reg != OV02C10_REG_DELAY) {
             ret = ov02c10_write(sccb_handle, regarray[i].reg, regarray[i].val);
         } else {
             delay_ms(regarray[i].val);
         }
         i++;
     }
     ESP_LOGD(TAG, "count=%d", i);
     return ret;
 }
 
 static esp_err_t ov02c10_set_reg_bits(esp_sccb_io_handle_t sccb_handle, uint16_t reg, uint8_t offset, uint8_t length, uint8_t value)
 {
     esp_err_t ret = ESP_OK;
     uint8_t reg_data = 0;
 
     ret = ov02c10_read(sccb_handle, reg, &reg_data);
     if (ret != ESP_OK) {
         return ret;
     }
     uint8_t mask = ((1 << length) - 1) << offset;
     value = (reg_data & ~mask) | ((value << offset) & mask);
     ret = ov02c10_write(sccb_handle, reg, value);
     return ret;
 }
 
 static esp_err_t ov02c10_set_test_pattern(esp_cam_sensor_device_t *dev, int enable)
 {
     ESP_LOGI(TAG,"test color = %d",enable);
     return ov02c10_set_reg_bits(dev->sccb_handle, 0x4503, 7, 1, enable ? 0x01 : 0x00);
 }
 
 static esp_err_t ov02c10_hw_reset(esp_cam_sensor_device_t *dev)
 {
     if (dev->reset_pin >= 0) {
         gpio_set_level(dev->reset_pin, 0);
         delay_ms(10);
         gpio_set_level(dev->reset_pin, 1);
         delay_ms(10);
     }
     return 0;
 }
 
 static esp_err_t ov02c10_soft_reset(esp_cam_sensor_device_t *dev)
 {
     esp_err_t ret = ov02c10_set_reg_bits(dev->sccb_handle, 0x0103, 0, 1, 0x01);
     delay_ms(5);
     return ret;
 }
 
 static esp_err_t ov02c10_get_sensor_id(esp_cam_sensor_device_t *dev, esp_cam_sensor_id_t *id)
 {
     uint8_t pid_h, pid_l;
     esp_err_t ret = ov02c10_read(dev->sccb_handle, OV02C10_REG_SENSOR_ID_H, &pid_h);
     ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "read pid_h failed");
 
     ret = ov02c10_read(dev->sccb_handle, OV02C10_REG_SENSOR_ID_L, &pid_l);
     ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "read pid_l failed");
 
     uint16_t pid = (pid_h << 8) | pid_l;
     if (pid) {
         id->pid = pid;
     }
     return ret;
 }
 
 static esp_err_t ov02c10_set_stream(esp_cam_sensor_device_t *dev, int enable)
 {
     esp_err_t ret;
     uint8_t val = OV02C10_MIPI_CTRL00_BUS_IDLE;
     if (enable) {
 #if CSI2_NONCONTINUOUS_CLOCK
         val |= OV02C10_MIPI_CTRL00_CLOCK_LANE_GATE | OV02C10_MIPI_CTRL00_LINE_SYNC_ENABLE;
 #endif
     } else {
         val |= OV02C10_MIPI_CTRL00_CLOCK_LANE_GATE | OV02C10_MIPI_CTRL00_CLOCK_LANE_DISABLE;
     }
 
     ret = ov02c10_write(dev->sccb_handle, 0x4800, CONFIG_CAMERA_OV02C10_CSI_LINESYNC_ENABLE ? 0x64 : 0x00);
     ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "write pad out failed");
 
 #if CONFIG_CAMERA_OV02C10_ISP_AF_ENABLE
     ret = ov02c10_write(dev->sccb_handle, 0x3002, enable ? 0x01 : 0x00);
     ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "write pad out failed");
 
     ret = ov02c10_write(dev->sccb_handle, 0x3010, enable ? 0x01 : 0x00);
     ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "write pad out failed");
 
     ret = ov02c10_write(dev->sccb_handle, 0x300D, enable ? 0x01 : 0x00);
     ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "write pad out failed");
 #endif
 
     ret = ov02c10_write(dev->sccb_handle, 0x0100, enable ? 0x01 : 0x00);
     ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "write pad out failed");
 
     dev->stream_status = enable;
 
     ESP_LOGD(TAG, "Stream=%d", enable);
     return ret;
 }
 
// hassctrl: mirror and flip are bits 3 and 4 of 0x3820 (the vendor's bit 1 of 0x3821 and 0x3820
// broke the readout: repeated stripes). Turned, the readout starts one pixel later on the color
// filter, so the window starts one pixel further (0x3811 x, 0x3813 y; the tables start it at 7, 4)
// and the ISP keeps its GBRG order; without that the picture is magenta.
#define OV02C10_X_START 0x07
#define OV02C10_Y_START 0x04

 static esp_err_t ov02c10_set_mirror(esp_cam_sensor_device_t *dev, int enable)
 {
     esp_err_t ret = ov02c10_write(dev->sccb_handle, 0x3811, OV02C10_X_START + (enable ? 1 : 0));
     ret |= ov02c10_set_reg_bits(dev->sccb_handle, 0x3820, 3, 1, enable ? 0x01 : 0x00);
     return ret;
 }

 static esp_err_t ov02c10_set_vflip(esp_cam_sensor_device_t *dev, int enable)
 {
     // one row earlier: one row later (5) gave a first frame and then none
     esp_err_t ret = ov02c10_write(dev->sccb_handle, 0x3813, OV02C10_Y_START - (enable ? 1 : 0));
     ret |= ov02c10_set_reg_bits(dev->sccb_handle, 0x3820, 4, 1, enable ? 0x01 : 0x00);
     return ret;
 }
 
//  static esp_err_t ov02c10_set_AE_target(esp_cam_sensor_device_t *dev, int target)
//  {
//      esp_err_t ret = ESP_OK;
//      /* stable in high */
//      int fast_high, fast_low;
//      int AE_low = target * 23 / 25;  /* 0.92 */
//      int AE_high = target * 27 / 25; /* 1.08 */
 
//      fast_high = AE_high << 1;
//      if (fast_high > 255) {
//          fast_high = 255;
//      }
 
//      fast_low = AE_low >> 1;
 
//      ret |= ov02c10_write(dev->sccb_handle, 0x3a0f, AE_high);
//      ret |= ov02c10_write(dev->sccb_handle, 0x3a10, AE_low);
//      ret |= ov02c10_write(dev->sccb_handle, 0x3a1b, AE_high);
//      ret |= ov02c10_write(dev->sccb_handle, 0x3a1e, AE_low);
//      ret |= ov02c10_write(dev->sccb_handle, 0x3a11, fast_high);
//      ret |= ov02c10_write(dev->sccb_handle, 0x3a1f, fast_low);
 
//      return ret;
//  }

 static esp_err_t ov02c10_set_exp_val(esp_cam_sensor_device_t *dev, uint32_t u32_val)
{
    esp_err_t ret;
    struct ov02c10_cam *cam_ov02c10 = (struct ov02c10_cam *)dev->priv;
    uint32_t value_buf = MAX(u32_val, s_ov02c10_exp_min);
    value_buf = MIN(value_buf, cam_ov02c10->ov02c10_para.exposure_max);

    ESP_LOGI(TAG, "set exposure 0x%" PRIx32, value_buf);
    /* 4 least significant bits of expsoure are fractional part */
    // ret = ov02c10_write(dev->sccb_handle,
    //                    OV02C10_REG_SHUTTER_TIME_H,
    //                    OV02C10_FETCH_EXP_H(value_buf));

    ESP_LOGI(TAG,"OV02C10_FETCH_EXP_M(value_buf) = 0x%"PRIx32,OV02C10_FETCH_EXP_M(value_buf));
    ret = ov02c10_write(dev->sccb_handle,
                        OV02C10_REG_SHUTTER_TIME_M,
                        OV02C10_FETCH_EXP_M(value_buf));
    ESP_LOGI(TAG,"OV02C10_FETCH_EXP_M(value_buf) = 0x%"PRIx32,OV02C10_FETCH_EXP_L(value_buf));
    ret |= ov02c10_write(dev->sccb_handle,
                        OV02C10_REG_SHUTTER_TIME_L,
                        OV02C10_FETCH_EXP_L(value_buf));
    if (ret == ESP_OK) {
        cam_ov02c10->ov02c10_para.exposure_val = value_buf;
    }
    return ret;
}

 static esp_err_t ov02c10_set_total_gain_val(esp_cam_sensor_device_t *dev, uint32_t u32_val)
{
    esp_err_t ret;
    struct ov02c10_cam *cam_ov02c10 = (struct ov02c10_cam *)dev->priv;
    const uint32_t gain = OV02C10_ANALOG_GAIN_MIN + MIN(u32_val, ARRAY_SIZE(ov02c10_total_gain_val_map) - 1);

    ret = ov02c10_write(dev->sccb_handle, OV02C10_REG_ANG_COARSE_GAIN, gain >> 4);
    ret |= ov02c10_write(dev->sccb_handle, OV02C10_REG_ANG_FINE_GAIN, (gain & 0x0f) << 4);
    if (ret == ESP_OK) {
        cam_ov02c10->ov02c10_para.gain_index = u32_val;
    }
    return ret;
}

static esp_err_t ov02c10_query_para_desc(esp_cam_sensor_device_t *dev, esp_cam_sensor_param_desc_t *qdesc)
{
    esp_err_t ret = ESP_OK;
    switch (qdesc->id) {
    case ESP_CAM_SENSOR_EXPOSURE_VAL:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
        qdesc->number.minimum = s_ov02c10_exp_min;
        qdesc->number.maximum = dev->cur_format->isp_info->isp_v1_info.vts - OV02C10_EXP_MAX_OFFSET; // max = VTS-6 = height+vblank-6, so when update vblank, exposure_max must be updated
        qdesc->number.step = 1;;
        qdesc->default_value = dev->cur_format->isp_info->isp_v1_info.exp_def;
        break;
    case ESP_CAM_SENSOR_EXPOSURE_US:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
        qdesc->number.minimum = EXPOSURE_OV02C10_TO_V4L2(s_ov02c10_exp_min, dev->cur_format);
        qdesc->number.maximum = EXPOSURE_OV02C10_TO_V4L2((dev->cur_format->isp_info->isp_v1_info.vts - OV02C10_EXP_MAX_OFFSET), dev->cur_format); // max = VTS-6 = height+vblank-6, so when update vblank, exposure_max must be updated
        qdesc->number.step = MAX(EXPOSURE_OV02C10_TO_V4L2(0x01, dev->cur_format), 1);
        qdesc->default_value = EXPOSURE_OV02C10_TO_V4L2((dev->cur_format->isp_info->isp_v1_info.exp_def), dev->cur_format);
        break;
    case ESP_CAM_SENSOR_GAIN:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_ENUMERATION;
        qdesc->enumeration.count = s_limited_gain_index;
        qdesc->enumeration.elements = ov02c10_total_gain_val_map;
        qdesc->default_value = dev->cur_format->isp_info->isp_v1_info.gain_def; // gain index
        break;
    case ESP_CAM_SENSOR_GROUP_EXP_GAIN:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_U8;
        qdesc->u8.size = sizeof(esp_cam_sensor_gh_exp_gain_t);
        break;
    case ESP_CAM_SENSOR_VFLIP:
    case ESP_CAM_SENSOR_HMIRROR:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
        qdesc->number.minimum = 0;
        qdesc->number.maximum = 1;
        qdesc->number.step = 1;
        qdesc->default_value = 0;
        break;
    default: {
        ESP_LOGD(TAG, "id=%"PRIx32" is not supported", qdesc->id);
        ret = ESP_ERR_INVALID_ARG;
        break;
    }
    }
    return ret;
}

static esp_err_t ov02c10_get_para_value(esp_cam_sensor_device_t *dev, uint32_t id, void *arg, size_t size)
{
    esp_err_t ret = ESP_OK;
    struct ov02c10_cam *cam_ov02c10 = (struct ov02c10_cam *)dev->priv;
    switch (id) {
    case ESP_CAM_SENSOR_EXPOSURE_VAL: {
        *(uint32_t *)arg = cam_ov02c10->ov02c10_para.exposure_val;
        break;
    }
    case ESP_CAM_SENSOR_GAIN: {
        *(uint32_t *)arg = cam_ov02c10->ov02c10_para.gain_index;
        break;
    }
    default: {
        ret = ESP_ERR_NOT_SUPPORTED;
        break;
    }
    }
    return ret;
}
 
 
//  static esp_err_t ov02c10_query_para_desc(esp_cam_sensor_device_t *dev, esp_cam_sensor_param_desc_t *qdesc)
//  {
//      esp_err_t ret = ESP_OK;
//      switch (qdesc->id) {
//      case ESP_CAM_SENSOR_VFLIP:
//      case ESP_CAM_SENSOR_HMIRROR:
//          qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
//          qdesc->number.minimum = 0;
//          qdesc->number.maximum = 1;
//          qdesc->number.step = 1;
//          qdesc->default_value = 0;
//          break;
//      case ESP_CAM_SENSOR_EXPOSURE_VAL:
//          qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
//          qdesc->number.minimum = 2;
//          qdesc->number.maximum = 235;
//          qdesc->number.step = 1;
//          qdesc->default_value = 0;
//          break;
//      default: {
//          ESP_LOGI(TAG, "id=%"PRIx32" is not supported", qdesc->id);
//          ret = ESP_ERR_INVALID_ARG;
//          break;
//      }
//      }
//      return ret;
//  }
 
//  static esp_err_t ov02c10_get_para_value(esp_cam_sensor_device_t *dev, uint32_t id, void *arg, size_t size)
//  {
//      return ESP_ERR_NOT_SUPPORTED;
//  }
 
//  static esp_err_t ov02c10_set_para_value(esp_cam_sensor_device_t *dev, uint32_t id, const void *arg, size_t size)
//  {
//      esp_err_t ret = ESP_OK;
 
//      switch (id) {
//      case ESP_CAM_SENSOR_VFLIP: {
//          int *value = (int *)arg;
 
//          ret = ov02c10_set_vflip(dev, *value);
//          break;
//      }
//      case ESP_CAM_SENSOR_HMIRROR: {
//          int *value = (int *)arg;
 
//          ret = ov02c10_set_mirror(dev, *value);
//          break;
//      }
//      case ESP_CAM_SENSOR_EXPOSURE_VAL: {
//          int *value = (int *)arg;
 
//          ret = ov02c10_set_AE_target(dev, *value);
//          break;
//      }
//      case ESP_CAM_SENSOR_GAIN: {
//         uint32_t u32_val = *(uint32_t *)arg;
//         ret = ov02c10_set_total_gain_val(dev, u32_val);
//         break;
//     }
//      default: {
//          ESP_LOGE(TAG, "set id=%" PRIx32 " is not supported", id);
//          ret = ESP_ERR_INVALID_ARG;
//          break;
//      }
//      }
 
//      return ret;
//  }


static esp_err_t ov02c10_set_para_value(esp_cam_sensor_device_t *dev, uint32_t id, const void *arg, size_t size)
{
    esp_err_t ret = ESP_OK;

    switch (id) {
    case ESP_CAM_SENSOR_EXPOSURE_VAL: {
        uint32_t u32_val = *(uint32_t *)arg;
        ret = ov02c10_set_exp_val(dev, u32_val);
        break;
    }
    case ESP_CAM_SENSOR_EXPOSURE_US: {
        uint32_t u32_val = *(uint32_t *)arg;
        uint32_t ori_exp = EXPOSURE_V4L2_TO_OV02C10(u32_val, dev->cur_format);
        ret = ov02c10_set_exp_val(dev, ori_exp);
        break;
    }
    case ESP_CAM_SENSOR_GAIN: {
        uint32_t u32_val = *(uint32_t *)arg;
        ret = ov02c10_set_total_gain_val(dev, u32_val);
        break;
    }
    case ESP_CAM_SENSOR_GROUP_EXP_GAIN: {
        esp_cam_sensor_gh_exp_gain_t *value = (esp_cam_sensor_gh_exp_gain_t *)arg;
        uint32_t ori_exp = EXPOSURE_V4L2_TO_OV02C10(value->exposure_us, dev->cur_format);
        ret = ov02c10_write(dev->sccb_handle, OV02C10_REG_GROUP_HOLD, OV02C10_GROUP_HOLD_START);
        ret |= ov02c10_set_exp_val(dev, ori_exp);
        ret |= ov02c10_set_total_gain_val(dev, value->gain_index);
        ret |= ov02c10_write(dev->sccb_handle, OV02C10_REG_GROUP_HOLD_DELAY, OV02C10_GROUP_HOLD_DELAY_FRAMES);
        ret |= ov02c10_write(dev->sccb_handle, OV02C10_REG_GROUP_HOLD, OV02C10_GROUP_HOLD_END);
        break;
    }
    case ESP_CAM_SENSOR_VFLIP: {
        int *value = (int *)arg;
        ret = ov02c10_set_vflip(dev, *value);
        break;
    }
    case ESP_CAM_SENSOR_HMIRROR: {
        int *value = (int *)arg;
        ret = ov02c10_set_mirror(dev, *value);
        break;
    }
    default: {
        ESP_LOGE(TAG, "set id=%" PRIx32 " is not supported", id);
        ret = ESP_ERR_INVALID_ARG;
        break;
    }
    }

    return ret;
}
 
 static esp_err_t ov02c10_query_support_formats(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_array_t *formats)
 {
     ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
     ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, formats);
 
     formats->count = ARRAY_SIZE(ov02c10_format_info);
     formats->format_array = &ov02c10_format_info[0];
     return ESP_OK;
 }
 
 static esp_err_t ov02c10_query_support_capability(esp_cam_sensor_device_t *dev, esp_cam_sensor_capability_t *sensor_cap)
 {
     ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
     ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, sensor_cap);
 
     sensor_cap->fmt_raw = 1;
     return ESP_OK;
 }
 
 static int ov02c10_get_sysclk(esp_cam_sensor_device_t *dev)
 {
     /* calculate sysclk */
     int xvclk = dev->cur_format->xclk / 10000;
     int sysclk = 0;
     uint8_t temp1, temp2;
     int pre_div02x, div_cnt7b, sdiv0, pll_rdiv, bit_div2x, sclk_div, VCO;
     const int pre_div02x_map[] = {2, 2, 4, 6, 8, 3, 12, 5, 16, 2, 2, 2, 2, 2, 2, 2};
     const int sdiv0_map[] = {16, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
     const int pll_rdiv_map[] = {1, 2};
     const int bit_div2x_map[] = {2, 2, 2, 2, 2, 2, 2, 2, 4, 2, 5, 2, 2, 2, 2, 2};
     const int sclk_div_map[] = {1, 2, 4, 1};
 
     ov02c10_read(dev->sccb_handle, 0x3037, &temp1);
     temp2 = temp1 & 0x0f;
     pre_div02x = pre_div02x_map[temp2];
     temp2 = (temp1 >> 4) & 0x01;
     pll_rdiv = pll_rdiv_map[temp2];
     ov02c10_read(dev->sccb_handle, 0x3036, &temp1);
 
     div_cnt7b = temp1;
 
     VCO = xvclk * 2 / pre_div02x * div_cnt7b;
     ov02c10_read(dev->sccb_handle, 0x3035, &temp1);
     temp2 = temp1 >> 4;
     sdiv0 = sdiv0_map[temp2];
     ov02c10_read(dev->sccb_handle, 0x3034, &temp1);
     temp2 = temp1 & 0x0f;
     bit_div2x = bit_div2x_map[temp2];
     ov02c10_read(dev->sccb_handle, 0x3106, &temp1);
     temp2 = (temp1 >> 2) & 0x03;
     sclk_div = sclk_div_map[temp2];
     sysclk = VCO * 2 / sdiv0 / pll_rdiv / bit_div2x / sclk_div;
     return sysclk;
 }
 
 static int ov02c10_get_hts(esp_cam_sensor_device_t *dev)
 {
     /* read HTS from register settings */
     int hts = 0;
     uint8_t temp1, temp2;
 
     ov02c10_read(dev->sccb_handle, 0x380c, &temp1);
     ov02c10_read(dev->sccb_handle, 0x380d, &temp2);
     hts = (temp1 << 8) + temp2;
     ESP_LOGI(TAG,"hts = 0x%x",hts);
     return hts;
 }
 
 static int ov02c10_get_vts(esp_cam_sensor_device_t *dev)
 {
     /* read VTS from register settings */
     int vts = 0;
     uint8_t temp1, temp2;
 
     /* total vertical size[15:8] high byte */
     ov02c10_read(dev->sccb_handle, 0x380e, &temp1);
     ov02c10_read(dev->sccb_handle, 0x380f, &temp2);
 
     vts = (temp1 << 8) + temp2;
     ESP_LOGI(TAG,"vts = 0x%x",vts);
     return vts;
 }
 
//  static int ov02c10_get_light_freq(esp_cam_sensor_device_t *dev)
//  {
//      /* get banding filter value */
//      uint8_t temp, temp1;
//      int light_freq = 0;
 
//      ov02c10_read(dev->sccb_handle, 0x3c01, &temp);
 
//      if (temp & 0x80) {
//          /* manual */
//          ov02c10_read(dev->sccb_handle, 0x3c00, &temp1);
//          if (temp1 & 0x04) {
//              /* 50Hz */
//              light_freq = 50;
//          } else {
//              /* 60Hz */
//              light_freq = 60;
//          }
//      } else {
//          /* auto */
//          ov02c10_read(dev->sccb_handle, 0x3c0c, &temp1);
//          if (temp1 & 0x01) {
//              /* 50Hz */
//              light_freq = 50;
//          } else {
//              light_freq = 60;
//          }
//      }
//      return light_freq;
//  }
 
//  static esp_err_t ov02c10_set_bandingfilter(esp_cam_sensor_device_t *dev)
//  {
//      esp_err_t ret = ESP_OK;
//      int prev_sysclk, prev_VTS, prev_HTS;
//      int band_step60, max_band60, band_step50, max_band50;
 
//      /* read preview PCLK */
//      prev_sysclk = ov02c10_get_sysclk(dev);
//      /* read preview HTS */
//      prev_HTS = ov02c10_get_hts(dev);
 
//      /* read preview VTS */
//      prev_VTS = ov02c10_get_vts(dev);
 
//      /* calculate banding filter */
//      /* 60Hz */
//      band_step60 = prev_sysclk * 100 / prev_HTS * 100 / 120;
//      ret = ov02c10_write(dev->sccb_handle, 0x3a0a, (uint8_t)(band_step60 >> 8));
//      ret |= ov02c10_write(dev->sccb_handle, 0x3a0b, (uint8_t)(band_step60 & 0xff));
 
//      max_band60 = (int)((prev_VTS - 4) / band_step60);
//      ret |= ov02c10_write(dev->sccb_handle, 0x3a0d, (uint8_t)max_band60);
 
//      /* 50Hz */
//      band_step50 = prev_sysclk * 100 / prev_HTS;
//      ret |= ov02c10_write(dev->sccb_handle, 0x3a08, (uint8_t)(band_step50 >> 8));
//      ret |= ov02c10_write(dev->sccb_handle, 0x3a09, (uint8_t)(band_step50 & 0xff));
 
//      max_band50 = (int)((prev_VTS - 4) / band_step50);
//      ret |= ov02c10_write(dev->sccb_handle, 0x3a0e, (uint8_t)max_band50);
//      return ret;
//  }
 
 static esp_err_t ov02c10_set_format(esp_cam_sensor_device_t *dev, const esp_cam_sensor_format_t *format)
 {
     ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    struct ov02c10_cam *cam_ov02c10 = (struct ov02c10_cam *)dev->priv;
    esp_err_t ret = ESP_OK;
    /* Depending on the interface type, an available configuration is automatically loaded.
    You can set the output format of the sensor without using query_format().*/
    if (format == NULL) {
        format = &ov02c10_format_info[CONFIG_CAMERA_OV02C10_MIPI_IF_FORMAT_INDEX_DAFAULT];
    }

    ret = ov02c10_write_array(dev->sccb_handle, (ov02c10_reginfo_t *)format->regs);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set format regs fail");
        return ESP_CAM_SENSOR_ERR_FAILED_SET_FORMAT;
    }

    dev->cur_format = format;
    // init para
    cam_ov02c10->ov02c10_para.exposure_val = dev->cur_format->isp_info->isp_v1_info.exp_def;
    cam_ov02c10->ov02c10_para.gain_index = dev->cur_format->isp_info->isp_v1_info.gain_def;
    cam_ov02c10->ov02c10_para.exposure_max = dev->cur_format->isp_info->isp_v1_info.vts - OV02C10_EXP_MAX_OFFSET;

    return ret;
 }
 
 static esp_err_t ov02c10_get_format(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_t *format)
 {
     ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
     ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, format);
 
     esp_err_t ret = ESP_FAIL;
 
     if (dev->cur_format != NULL) {
         memcpy(format, dev->cur_format, sizeof(esp_cam_sensor_format_t));
         ret = ESP_OK;
     }
     return ret;
 }
 
 static esp_err_t ov02c10_priv_ioctl(esp_cam_sensor_device_t *dev, uint32_t cmd, void *arg)
 {
     ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
 
     esp_err_t ret = ESP_FAIL;
     uint8_t regval;
     esp_cam_sensor_reg_val_t *sensor_reg;
     OV02C10_IO_MUX_LOCK(mux);
     switch (cmd) {
     case ESP_CAM_SENSOR_IOC_HW_RESET:
         ret = ov02c10_hw_reset(dev);
         break;
     case ESP_CAM_SENSOR_IOC_SW_RESET:
         ret = ov02c10_soft_reset(dev);
         break;
     case ESP_CAM_SENSOR_IOC_S_REG:
         sensor_reg = (esp_cam_sensor_reg_val_t *)arg;
         ret = ov02c10_write(dev->sccb_handle, sensor_reg->regaddr, sensor_reg->value);
         break;
     case ESP_CAM_SENSOR_IOC_S_STREAM:
         // ret = ov02c10_set_test_pattern(dev, *(int *)arg);
         ret = ov02c10_set_stream(dev, *(int *)arg);
         
         break;
     case ESP_CAM_SENSOR_IOC_S_TEST_PATTERN:
         ret = ov02c10_set_test_pattern(dev, *(int *)arg);
         break;
     case ESP_CAM_SENSOR_IOC_G_REG:
         sensor_reg = (esp_cam_sensor_reg_val_t *)arg;
         ret = ov02c10_read(dev->sccb_handle, sensor_reg->regaddr, &regval);
         if (ret == ESP_OK) {
             sensor_reg->value = regval;
         }
         break;
     case ESP_CAM_SENSOR_IOC_G_CHIP_ID:
         ret = ov02c10_get_sensor_id(dev, arg);
         break;
     default:
         ret = ESP_ERR_INVALID_ARG;
         break;
     }
     OV02C10_IO_MUX_UNLOCK(mux);
     return ret;
 }
 
 static esp_err_t ov02c10_power_on(esp_cam_sensor_device_t *dev)
 {
     esp_err_t ret = ESP_OK;
 
     if (dev->xclk_pin >= 0) {
         OV02C10_ENABLE_OUT_CLOCK(dev->xclk_pin, dev->xclk_freq_hz);
     }
 
     if (dev->pwdn_pin >= 0) {
         gpio_config_t conf = { 0 };
         conf.pin_bit_mask = 1LL << dev->pwdn_pin;
         conf.mode = GPIO_MODE_OUTPUT;
         ret = gpio_config(&conf);
         ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "pwdn pin config failed");
 
         // carefully, logic is inverted compared to reset pin
         gpio_set_level(dev->pwdn_pin, 1);
         delay_ms(10);
         gpio_set_level(dev->pwdn_pin, 0);
         delay_ms(10);
     }
 
     if (dev->reset_pin >= 0) {
         gpio_config_t conf = { 0 };
         conf.pin_bit_mask = 1LL << dev->reset_pin;
         conf.mode = GPIO_MODE_OUTPUT;
         ret = gpio_config(&conf);
         ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "reset pin config failed");
 
         gpio_set_level(dev->reset_pin, 0);
         delay_ms(10);
         gpio_set_level(dev->reset_pin, 1);
         delay_ms(10);
     }
 
     return ret;
 }
 
 static esp_err_t ov02c10_power_off(esp_cam_sensor_device_t *dev)
 {
     esp_err_t ret = ESP_OK;
 
     if (dev->xclk_pin >= 0) {
         OV02C10_DISABLE_OUT_CLOCK(dev->xclk_pin);
     }
 
     if (dev->pwdn_pin >= 0) {
         gpio_set_level(dev->pwdn_pin, 0);
         delay_ms(10);
         gpio_set_level(dev->pwdn_pin, 1);
         delay_ms(10);
     }
 
     if (dev->reset_pin >= 0) {
         gpio_set_level(dev->reset_pin, 1);
         delay_ms(10);
         gpio_set_level(dev->reset_pin, 0);
         delay_ms(10);
     }
 
     return ret;
 }
 
 static esp_err_t ov02c10_delete(esp_cam_sensor_device_t *dev)
 {
     ESP_LOGD(TAG, "del ov02c10 (%p)", dev);
     if (dev) {
         free(dev);
         dev = NULL;
     }
 
     return ESP_OK;
 }
 
 static const esp_cam_sensor_ops_t ov02c10_ops = {
     .query_para_desc = ov02c10_query_para_desc,
     .get_para_value = ov02c10_get_para_value,
     .set_para_value = ov02c10_set_para_value,
     .query_support_formats = ov02c10_query_support_formats,
     .query_support_capability = ov02c10_query_support_capability,
     .set_format = ov02c10_set_format,
     .get_format = ov02c10_get_format,
     .priv_ioctl = ov02c10_priv_ioctl,
     .del = ov02c10_delete
 };
 
 // We need manage these devices, and maybe need to add it into the private member of esp_device
 esp_cam_sensor_device_t *ov02c10_detect(esp_cam_sensor_config_t *config)
 {
    esp_cam_sensor_device_t *dev = NULL;
    struct ov02c10_cam *cam_ov02c10;
    s_limited_gain_index = ARRAY_SIZE(ov02c10_total_gain_val_map);
    if (config == NULL) {
        return NULL;
    }

    dev = calloc(1, sizeof(esp_cam_sensor_device_t));
    if (dev == NULL) {
        ESP_LOGE(TAG, "No memory for camera");
        return NULL;
    }

    cam_ov02c10 = heap_caps_calloc(1, sizeof(struct ov02c10_cam), MALLOC_CAP_DEFAULT);
    if (!cam_ov02c10) {
        ESP_LOGE(TAG, "failed to calloc cam");
        free(dev);
        return NULL;
    }

    dev->name = (char *)OV02C10_SENSOR_NAME;
    dev->sccb_handle = config->sccb_handle;
    dev->xclk_pin = config->xclk_pin;
    dev->reset_pin = config->reset_pin;
    dev->pwdn_pin = config->pwdn_pin;
    dev->sensor_port = config->sensor_port;
    dev->ops = &ov02c10_ops;
    dev->priv = cam_ov02c10;
    for (size_t i = 0; i < ARRAY_SIZE(ov02c10_total_gain_val_map); i++) {
        if (ov02c10_total_gain_val_map[i] > s_limited_gain) {
            s_limited_gain_index = i - 1;
            break;
        }
    }
    if (config->sensor_port != ESP_CAM_SENSOR_DVP) {
        dev->cur_format = &ov02c10_format_info[CONFIG_CAMERA_OV02C10_MIPI_IF_FORMAT_INDEX_DAFAULT];
    } 

    // Configure sensor power, clock, and SCCB port
    if (ov02c10_power_on(dev) != ESP_OK) {
        ESP_LOGE(TAG, "Camera power on failed");
        goto err_free_handler;
    }

    if (ov02c10_get_sensor_id(dev, &dev->id) != ESP_OK) {
        ESP_LOGE(TAG, "Get sensor ID failed");
        goto err_free_handler;
    } else if (dev->id.pid != OV02C10_PID) {
        ESP_LOGE(TAG, "Camera sensor is not OV02C10, PID=0x%x", dev->id.pid);
        goto err_free_handler;
    }
    ESP_LOGI(TAG, "Detected Camera sensor PID=0x%x", dev->id.pid);

    return dev;

err_free_handler:
    ov02c10_power_off(dev);
    free(dev->priv);
    free(dev);

    return NULL;
 }
 
 #if CONFIG_CAMERA_OV02C10_AUTO_DETECT_MIPI_INTERFACE_SENSOR
 ESP_CAM_SENSOR_DETECT_FN(ov02c10_detect, ESP_CAM_SENSOR_MIPI_CSI, OV02C10_SCCB_ADDR)
 {
     ((esp_cam_sensor_config_t *)config)->sensor_port = ESP_CAM_SENSOR_MIPI_CSI;
     return ov02c10_detect(config);
 }
 #endif
 