/*
 * 1.3 寸 128x64 单色 OLED（SSD1306 / SH1106 通用）移植层。
 *
 * 硬件链路（见 app_config.h）：
 *   OLED VCC -> 3V3   GND -> GND   SDA -> GPIO23   SCL -> GPIO24
 *
 * 为什么不用 esp_lcd 的 SSD1306 驱动：
 *   1.3 寸 4 脚 I2C 模块绝大多数是 SH1106（132 列显存，只支持页寻址）。
 *   SH1106 不支持 SSD1306 的 0x20/0x21/0x22 寻址命令，用 SSD1306 驱动会雪花。
 *   这里统一用两种芯片都支持的写法：逐页 0xB0|page -> 设列地址 -> 连续写 128 字节。
 *
 * 软件链路：
 *   I2C 主机驱动 -> esp_lcd panel_io -> LVGL(渲染 1bpp) -> 页寻址刷屏
 */
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "app_config.h"
#include "oled_port.h"

static const char *TAG = "oled";

#define OLED_PAGES          (OLED_V_RES / 8)
#define LVGL_TICK_MS        5
#define LVGL_TASK_STACK     (6 * 1024)
#define LVGL_TASK_PRIO      2
#define LVGL_PALETTE_SIZE   8       // LVGL 在 I1 缓冲前面预留的调色板长度

static esp_lcd_panel_io_handle_t s_io;

/* 1bpp 显存镜像：128*64/8 = 1024 字节，一字节纵向对应 8 行，bit0 在最上 */
static uint8_t s_fb[OLED_H_RES * OLED_V_RES / 8];

/* ---------------- 底层：命令与刷屏 ---------------- */

static void oled_cmd(uint8_t cmd)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, cmd, NULL, 0));
}

static void oled_cmd_arg(uint8_t cmd, uint8_t arg)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, cmd, &arg, 1));
}

/* 上电初始化：SH1106 与 SSD1306 共用的命令集 */
static void oled_init_panel(void)
{
    oled_cmd(0xAE);                                     // 关显示
    oled_cmd_arg(0xD5, 0x80);                           // 时钟分频
    oled_cmd_arg(0xA8, OLED_V_RES - 1);                 // 多路复用率 = 行数-1
    oled_cmd_arg(0xD3, 0x00);                           // 显示偏移
    oled_cmd(0x40);                                     // 起始行 = 0
    oled_cmd_arg(0x8D, 0x14);                           // 电荷泵开启（不开就是全黑）
    oled_cmd_arg(0x20, 0x02);                           // SSD1306：页寻址；SH1106：空操作
    oled_cmd_arg(0xDA, OLED_V_RES == 64 ? 0x12 : 0x02); // COM 引脚配置
    oled_cmd_arg(0x81, OLED_CONTRAST);
    oled_cmd_arg(0xD9, 0xF1);                           // 预充电周期
    oled_cmd_arg(0xDB, 0x40);                           // VCOMH
    oled_cmd(0xA4);                                     // 显示内容跟随显存
    oled_cmd(0xA6);                                     // 正常显示（非反色）
    oled_cmd(OLED_MIRROR_X ? 0xA1 : 0xA0);              // 列方向
    oled_cmd(OLED_MIRROR_Y ? 0xC8 : 0xC0);              // 行扫描方向
    oled_cmd(0xAF);                                     // 开显示
}

/* 把 s_fb 整屏推给屏：逐页写，避开 SH1106 不支持的 0x21/0x22 */
static void oled_refresh(void)
{
    for (int page = 0; page < OLED_PAGES; page++) {
        uint8_t col = OLED_COL_OFFSET;
        oled_cmd(0xB0 | page);                  // 页地址
        oled_cmd(0x00 | (col & 0x0F));          // 列地址低 4 位
        oled_cmd(0x10 | ((col >> 4) & 0x0F));   // 列地址高 4 位
        // 页内列地址会自动 +1，所以一次连续写 128 字节即可
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(s_io, -1, s_fb + page * OLED_H_RES, OLED_H_RES));
    }
}

static void oled_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
    oled_refresh();
}

/* ---------------- LVGL 刷屏回调 ---------------- */
/* LVGL 的 I1 缓冲是"行优先、MSB 在左"，屏的显存是"按页纵向堆叠"，
 * 这里做一次位序重排；LVGL 用 FULL 模式渲染，所以每次都是整屏重画。 */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    px_map += LVGL_PALETTE_SIZE;    // 跳过 LVGL 预留的调色板

    for (int y = 0; y < OLED_V_RES; y++) {
        for (int x = 0; x < OLED_H_RES; x++) {
            bool on = px_map[(OLED_H_RES >> 3) * y + (x >> 3)] & (1 << (7 - x % 8));
            uint8_t *buf = &s_fb[(OLED_H_RES * (y >> 3)) + x];
            if (on) {
                *buf |= (1 << (y % 8));
            } else {
                *buf &= ~(1 << (y % 8));
            }
        }
    }

    oled_refresh();
    lv_display_flush_ready(disp);   // I2C 是阻塞发送，函数返回时数据已发出
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_MS);
}

static void lvgl_task(void *arg)
{
    while (1) {
        uint32_t delay_ms = lv_timer_handler();
        delay_ms = MAX(delay_ms, 1000 / CONFIG_FREERTOS_HZ);    // 至少跑 1 个 tick
        delay_ms = MIN(delay_ms, 500);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/* ---------------- 对外接口 ---------------- */

void oled_port_start(oled_ui_build_cb_t build_ui)
{
    ESP_LOGI(TAG, "init I2C bus (SDA=%d, SCL=%d)", OLED_PIN_SDA, OLED_PIN_SCL);
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = OLED_I2C_PORT,
        .sda_io_num = OLED_PIN_SDA,
        .scl_io_num = OLED_PIN_SCL,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    // 先用两种常见地址探测一下，日志能直接告诉你屏在哪
    for (uint8_t addr = 0x3C; addr <= 0x3D; addr++) {
        esp_err_t err = i2c_master_probe(i2c_bus, addr, 100);
        ESP_LOGI(TAG, "probe 0x%02X -> %s", addr, err == ESP_OK ? "ACK(在线)" : "无应答");
    }

    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = OLED_I2C_ADDR,
        .scl_speed_hz = OLED_I2C_HZ,
        .control_phase_bytes = 1,               // SSD1306/SH1106：1 字节控制字
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,                     // Co/D#C 位在控制字的 bit6
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &s_io));

    ESP_LOGI(TAG, "init panel (offset=%d)", OLED_COL_OFFSET);
    oled_init_panel();
    oled_clear();

    ESP_LOGI(TAG, "init LVGL");
    lv_init();

    lv_display_t *disp = lv_display_create(OLED_H_RES, OLED_V_RES);

    // 单色屏必须用 I1 格式，缓冲大小 = 像素数/8，再加 LVGL 的 8 字节调色板
    size_t buf_size = OLED_H_RES * OLED_V_RES / 8 + LVGL_PALETTE_SIZE;
    void *draw_buf = heap_caps_calloc(1, buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(draw_buf);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(disp, draw_buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);

    // LVGL 需要一个毫秒时基
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_MS * 1000));

    // 此时还没有 LVGL 任务在跑，界面代码可以放心直接建控件
    build_ui();

    xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, LVGL_TASK_PRIO, NULL);
    ESP_LOGI(TAG, "display started");
}
