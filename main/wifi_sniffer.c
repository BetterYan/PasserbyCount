/*
 * 混杂模式抓包。
 *
 * 关键点：
 *   1. WIFI_MODE_NULL + esp_wifi_start()，不开任何 netif，只借 Wi-Fi 射频收包；
 *   2. 过滤器只放管理帧（可选放数据帧），CRC 错的帧默认不会送上来；
 *   3. 回调在 Wi-Fi 驱动任务里跑，只做"类型/子类型/RSSI"三个判断就往队列里塞，
 *      解析和统计全部交给 sniff_rx 任务，否则会拖垮 Wi-Fi 任务导致丢帧；
 *   4. C5 是单射频，只能守一个信道，靠 wifi_hop 任务每 300ms 换一个信道。
 *
 * 帧格式（IEEE 802.11-2020）：
 *   管理帧头 24 字节：FC(2) Duration(2) Addr1(6) Addr2(6) Addr3(6) Seq(2)
 *   Addr2 在偏移 10，管理帧里它就是发射端（终端自己）。
 *   Probe Request 子类型 = 4，Association Request = 0，Reassociation = 2。
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"

#include "app_config.h"
#include "people_counter.h"
#include "wifi_sniffer.h"

static const char *TAG = "sniffer";

#define IEEE80211_HDR_LEN   24
#define IEEE80211_ADDR2_OFS 10
#define IEEE80211_FCS_LEN   4

#define RX_TASK_STACK       4096
#define RX_TASK_PRIO        5
#define HOP_TASK_STACK      3072
#define HOP_TASK_PRIO       4
#define RX_WAIT_MS          200
#define LOG_PERIOD_MS       10000

/* 跳频表：1/6/11 是 2.4G 三个互不重叠的主信道，覆盖九成 AP。
 * 打开 PC_HOP_5G 后再加上 5G 信道，band mode 会自动切 AUTO。
 * 中国区 5G 合法信道是 36~64（含 DFS）和 149~165，
 * 就是开机日志里 wifi_5g_channel_mask=0x3e001fe 的那几位。
 * 信道越多轮一圈越慢，按现场 AP 分布删减。 */
static const uint8_t s_hop_list[] = {
    1, 6, 11,
#if PC_HOP_5G
    36, 40, 44, 48, 149, 153, 157, 161,
#endif
};
#define HOP_COUNT   (sizeof(s_hop_list) / sizeof(s_hop_list[0]))

static QueueHandle_t s_queue;
static volatile uint8_t s_channel;
static volatile uint32_t s_dropped;

/* ---------------- 抓包回调（Wi-Fi 任务上下文） ---------------- */

static inline void push_mac(const uint8_t *frame, int8_t rssi)
{
    pc_rx_t rec;
    memcpy(rec.mac, &frame[IEEE80211_ADDR2_OFS], sizeof(rec.mac));
    rec.rssi = rssi;

    if (xQueueSend(s_queue, &rec, 0) != pdTRUE) {
        s_dropped++;
    }
}

static void sniffer_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;

    if (pkt->rx_ctrl.sig_len < IEEE80211_HDR_LEN + IEEE80211_FCS_LEN) {
        return;
    }
    if (pkt->rx_ctrl.rssi < PC_RSSI_MIN) {
        return;     // 太远，不算"周边"
    }

    const uint8_t *f = pkt->payload;
    const uint8_t fc0 = f[0];
    const uint8_t ftype = (fc0 >> 2) & 0x03;
    const uint8_t subtype = (fc0 >> 4) & 0x0F;

    if (type == WIFI_PKT_MGMT && ftype == 0) {
        /* 4=Probe Request, 0=Assoc Request, 2=Reassoc Request */
        if (subtype == 4 || subtype == 0 || subtype == 2) {
            push_mac(f, pkt->rx_ctrl.rssi);
        }
        return;
    }

#if PC_FILTER_DATA_FRAMES
    /* 只取 ToDS 的上行帧：这种情况 addr2 才是终端，否则是 AP 的 BSSID */
    if (type == WIFI_PKT_DATA && ftype == 2 && (f[1] & 0x01)) {
        push_mac(f, pkt->rx_ctrl.rssi);
    }
#endif
}

/* ---------------- 信道跳频 ---------------- */

static void hop_task(void *arg)
{
    uint32_t i = 0;

    while (1) {
        uint8_t ch = s_hop_list[i];
        esp_err_t err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        if (err == ESP_OK) {
            s_channel = ch;
        } else {
            ESP_LOGW(TAG, "set channel %u failed: %s", ch, esp_err_to_name(err));
        }

        i = (i + 1) % HOP_COUNT;
        vTaskDelay(pdMS_TO_TICKS(PC_HOP_DWELL_MS));
    }
}

/* ---------------- 出队 + 每秒统计 ---------------- */

static void rx_task(void *arg)
{
    pc_rx_t rec;
    uint32_t last_tick = pc_now_ms();
    uint32_t last_log = last_tick;

    while (1) {
        if (xQueueReceive(s_queue, &rec, pdMS_TO_TICKS(RX_WAIT_MS)) == pdTRUE) {
            people_counter_feed(&rec, pc_now_ms());
            /* 先把队列里的都吃掉，但不要因此跳过下面的一秒一次 tick */
            while (xQueueReceive(s_queue, &rec, 0) == pdTRUE) {
                people_counter_feed(&rec, pc_now_ms());
            }
        }

        uint32_t now = pc_now_ms();
        if ((int32_t)(now - last_tick) >= 1000) {
            people_counter_tick(now);
            last_tick = now;
        }

        if ((int32_t)(now - last_log) >= LOG_PERIOD_MS) {
            pc_snapshot_t snap;
            people_counter_get(&snap);
            ESP_LOGI(TAG, "now=%u peak=%u total=%lu frames=%lu rate=%u/s rssi=%d ch=%u drop=%lu",
                     snap.current, snap.peak,
                     (unsigned long)snap.total, (unsigned long)snap.frames,
                     snap.rate, snap.rssi_avg, s_channel,
                     (unsigned long)s_dropped);
            last_log = now;
        }
    }
}

/* ---------------- 初始化 ---------------- */

esp_err_t wifi_sniffer_start(void)
{
    s_queue = xQueueCreate(PC_RX_QUEUE_LEN, sizeof(pc_rx_t));
    ESP_RETURN_ON_FALSE(s_queue != NULL, ESP_ERR_NO_MEM, TAG, "create rx queue");

    /* esp_wifi_init 会往默认事件循环注册事件，必须先建好 */
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num = PC_WIFI_STATIC_RX_BUF;
    cfg.dynamic_rx_buf_num = PC_WIFI_DYNAMIC_RX_BUF;

    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set_storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_NULL), TAG, "set_mode");

    /* 不设国家码，12/13 信道会被判为非法；顺带把发射功率限制在合规范围 */
    ESP_RETURN_ON_ERROR(esp_wifi_set_country_code("CN", true), TAG, "set_country");

    err = esp_wifi_start();
    if (err != ESP_OK) {
        /* 某些版本 NULL 模式下 start 会报错，但混杂模式仍可用，不当作致命错误 */
        ESP_LOGW(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
    }

#if PC_HOP_5G
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#else
    esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
#endif
    esp_wifi_set_ps(WIFI_PS_NONE);

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
#if PC_FILTER_DATA_FRAMES
                       | WIFI_PROMIS_FILTER_MASK_DATA
#endif
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_filter(&filter), TAG, "set_filter");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_rx_cb(sniffer_rx_cb), TAG, "set_rx_cb");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous(true), TAG, "set_promiscuous");

    s_channel = s_hop_list[0];
    esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);

    if (xTaskCreate(hop_task, "wifi_hop", HOP_TASK_STACK, NULL, HOP_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create hop task");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(rx_task, "sniff_rx", RX_TASK_STACK, NULL, RX_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create rx task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "promiscuous on, %u channels, dwell %dms, rssi_min %ddBm",
             (unsigned)HOP_COUNT, PC_HOP_DWELL_MS, PC_RSSI_MIN);
    return ESP_OK;
}

uint8_t wifi_sniffer_channel(void)
{
    return s_channel;
}

uint32_t wifi_sniffer_dropped(void)
{
    return s_dropped;
}
