/*
 * MAC 去重 + 滑动窗口人数统计。
 *
 * 线程模型：
 *   people_counter_feed() / people_counter_tick()  只在抓包处理任务里调用；
 *   people_counter_get()                           任意任务可调用（临界区拷贝快照）。
 */
#pragma once

#include <stdint.h>
#include "esp_timer.h"

#include "app_config.h"

/* 抓包回调送过来的一条记录 */
typedef struct {
    uint8_t mac[6];
    int8_t  rssi;
} pc_rx_t;

/* 给界面用的只读快照 */
typedef struct {
    uint16_t current;                // 当前人数（窗口内唯一 MAC 数）
    uint16_t peak;                   // 开机以来 current 的峰值
    uint32_t total;                  // 开机以来累计去重 MAC 数
    uint32_t frames;                 // 累计计入的探测帧数
    uint16_t rate;                   // 上一秒的帧速率
    int8_t   rssi_avg;               // 窗口内设备的平均 RSSI
    uint8_t  hist[PC_HIST_LEN];      // hist[0] 最新，hist[i] 为 i 秒前
    uint8_t  hist_max;               // hist[] 的最大值，用于柱子归一化
} pc_snapshot_t;

/* 单调毫秒时钟，约 49 天回绕，比较时必须用有符号差值 */
static inline uint32_t pc_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void people_counter_init(void);

/* 处理一条抓到的记录；now_ms 传 pc_now_ms() */
void people_counter_feed(const pc_rx_t *rx, uint32_t now_ms);

/* 每秒调用一次：重算窗口人数、推进历史曲线 */
void people_counter_tick(uint32_t now_ms);

/* 取快照（可在任意任务调用） */
void people_counter_get(pc_snapshot_t *out);
