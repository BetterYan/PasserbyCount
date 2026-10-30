#pragma once

/* 建界面 + 挂 500ms 刷新定时器。必须在 LVGL 初始化之后、LVGL 任务启动之前调用 */
void ui_init(void);
