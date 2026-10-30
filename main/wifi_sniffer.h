/*
 * 802.11 混杂模式抓包：监听 Probe Request / Association Request，
 * 把 <MAC, RSSI> 丢给 people_counter，另起任务负责信道跳频。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

/* 初始化 Wi-Fi 并开始抓包（会创建 2 个任务）。失败时返回错误，界面照常工作 */
esp_err_t wifi_sniffer_start(void);

/* 当前驻留的信道号，给界面显示用 */
uint8_t wifi_sniffer_channel(void);

/* 因队列满而丢弃的帧数 */
uint32_t wifi_sniffer_dropped(void);
