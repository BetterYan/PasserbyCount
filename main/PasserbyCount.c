/*
 * PasserbyCount —— 基于 Wi-Fi 探测请求（Probe Request）的周边人数估算器
 *
 * 数据流：
 *   Wi-Fi 混杂模式回调 ──► RX 队列 ──► sniff_rx 任务
 *                                        ├─ people_counter：MAC 去重 + 60s 滑窗
 *                                        └─ 每 10s 串口打印一行统计
 *                                      people_counter 快照
 *                                        └─ ui.c（LVGL 定时器，500ms）──► OLED
 *
 * 启动顺序有讲究：
 *   先点亮屏幕，再起 Wi-Fi。Wi-Fi 初始化要占一两百 KB 内存并停几百毫秒，
 *   放后面即使失败也不影响界面报错信息。
 */
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "oled_port.h"
#include "people_counter.h"
#include "ui.h"
#include "wifi_sniffer.h"

static const char *TAG = "app";

/*
 * 开机打印复位原因：串口在复位瞬间会掉线（USB-Serial/JTAG 会重新枚举），
 * 现场排查"为什么重启"时，这条日志是唯一可靠的证据。
 */
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "POWERON";
    case ESP_RST_EXT:        return "EXT_PIN";
    case ESP_RST_SW:         return "SW";
    case ESP_RST_PANIC:      return "PANIC(崩溃)";
    case ESP_RST_INT_WDT:    return "INT_WDT(中断看门狗)";
    case ESP_RST_TASK_WDT:   return "TASK_WDT(任务看门狗)";
    case ESP_RST_WDT:        return "WDT(其他看门狗)";
    case ESP_RST_BROWNOUT:   return "BROWNOUT(电压跌落)";
    case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH(电源毛刺)";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default:                 return "UNKNOWN";
    }
}

static void nvs_init(void)
{
    /* Wi-Fi 驱动要用 NVS 存射频校准数据 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "PasserbyCount boot, reset reason: %s", reset_reason_str(esp_reset_reason()));

    nvs_init();
    people_counter_init();

    ESP_LOGI(TAG, "start display");
    oled_port_start(ui_init);

    ESP_LOGI(TAG, "start sniffer");
    esp_err_t err = wifi_sniffer_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sniffer start failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "running, internal heap free=%u, min ever=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}
