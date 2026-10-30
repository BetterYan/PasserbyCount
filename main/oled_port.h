/*
 * 1.3 寸 128x64 单色 OLED 移植层（I2C + SH1106/SSD1306 + LVGL）。
 *
 * 用法：
 *   oled_port_start(ui_init);
 * 硬件初始化、LVGL 初始化、建屏、启动 LVGL 任务会在一次调用里做完，
 * 其中 build_ui 回调在"LVGL 任务启动之前"执行，所以界面代码可以直接建控件，
 * 不需要加锁；之后的刷新必须通过 lv_timer 在 LVGL 任务里做（见 ui.c）。
 */
#pragma once

typedef void (*oled_ui_build_cb_t)(void);

void oled_port_start(oled_ui_build_cb_t build_ui);
