/*
 * 全项目可调参数集中在这里。
 * 换板子/换屏/调灵敏度只需要改这一个文件。
 */
#pragma once

/* ======================= OLED（1.3 寸 128x64 I2C） ================= */
#define OLED_I2C_PORT       0        // I2C 控制器编号
#define OLED_PIN_SDA        23       // SDA -> GPIO23
#define OLED_PIN_SCL        24       // SCL -> GPIO24
#define OLED_I2C_ADDR       0x3C     // 模块背面标 0x3D 就改成 0x3D
#define OLED_I2C_HZ         (400 * 1000)
#define OLED_H_RES          128
#define OLED_V_RES          64       // 128x32 的屏改成 32

// SH1106：132 列显存，128 像素居中 -> 偏移 2；SSD1306：128 列显存 -> 偏移 0
// 显示整体右移 2 像素 / 右侧缺 2 列，说明是 SSD1306，把这里改成 0
#define OLED_COL_OFFSET     2
#define OLED_CONTRAST       0xCF     // 0x00~0xFF，太暗就调大
#define OLED_MIRROR_X       1        // 0xA1 / 0xA0
#define OLED_MIRROR_Y       1        // 0xC8 / 0xC0
/* ================================================================= */

/* ======================= 人数统计 ================= */
// 判定"还在附近"的滑动窗口：最近这么多毫秒内出现过的 MAC 都算
#define PC_ACTIVE_WINDOW_MS (60 * 1000)

// 低于这个 RSSI 的帧直接丢弃。-85 大约覆盖 30~50m 空旷环境
// 想只统计"门口经过"的人，收紧到 -75 左右
#define PC_RSSI_MIN         (-85)

// 去重表：128 组 x 4 路 = 512 个 MAC 槽位（约 8KB 静态 RAM）
// 组数必须是 2 的幂
#define PC_TABLE_SETS       128
#define PC_TABLE_WAYS       4

// 丢掉本地管理位为 1 的随机 MAC。
// 手机开启随机化后同一台设备会换着 MAC 发探测帧，不开这个会高估人数；
// 但开了会漏掉大部分现代手机，默认关闭。
#define PC_IGNORE_RANDOM_MAC 0

// 历史曲线保留多少秒（= 屏幕上柱状图的柱子数量）
#define PC_HIST_LEN         42
/* ================================================== */

/* ======================= 抓包 ================= */
// 每个信道停留多久再跳下一个。太短漏帧多，太长则轮一圈太慢
#define PC_HOP_DWELL_MS     300

// 是否加入 5GHz 信道。C5 支持 2.4G + 5G 双频，但跨频段跳频会多丢一点帧。
// 打开后 band mode 会自动切成 WIFI_BAND_MODE_AUTO
#define PC_HOP_5G           0

// 是否把数据帧也算作"有人在附近"（只取 ToDS 的上行帧，addr2 才是终端）
// 咖啡馆这类数据流量大的场景能明显提高召回率，代价是回调次数增加
#define PC_FILTER_DATA_FRAMES 0

// 回调 -> 处理任务 的队列长度。溢出只丢帧，不影响统计正确性
#define PC_RX_QUEUE_LEN     128

// Wi-Fi RX 缓冲数量（同时覆盖 sdkconfig 里的默认值）
#define PC_WIFI_STATIC_RX_BUF  16
#define PC_WIFI_DYNAMIC_RX_BUF 64
/* ============================================== */

/* ======================= 界面 ================= */
#define PC_UI_REFRESH_MS    500      // 屏幕刷新周期
/* ============================================== */
