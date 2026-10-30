/*
 * MAC 去重表 + 滑窗人数统计。
 *
 * 为什么不用链表 / 动态内存：
 *   抓包回调频率可达上千帧每秒，去重路径必须 O(1) 且绝不 malloc。
 *   这里用 128 组 x 4 路的组相联表（固定 8KB 静态内存）：
 *     1. 先用 FNV-1a 算出组号；
 *     2. 在组内 4 个槽位里找相同 MAC；
 *     3. 没找到就占空位，组满了就踢掉该组里最久没见的那个。
 *   哈希冲突只会导致"提前忘记一个 MAC"，不会算错已有 MAC。
 *
 * 局限（产品化时必须知道）：
 *   - 手机 MAC 随机化：同一台手机每轮扫描换一个 MAC，会高估；
 *   - 一人多设备（手机 / 手表 / 耳机）会高估；
 *   - 设备被踢出表后再次出现会被重复计入 total（current 不受影响）。
 */
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "people_counter.h"

#define SETS    (PC_TABLE_SETS)
#define WAYS    (PC_TABLE_WAYS)
#define MASK    (SETS - 1)

typedef struct {
    uint8_t  mac[6];
    uint8_t  random;      /* 本地管理位：手机随机化 MAC */
    int8_t   rssi;
    uint16_t hits;        /* 0 表示槽位空闲 */
    uint32_t last_ms;
} pc_slot_t;

static pc_slot_t s_tab[SETS][WAYS];

/* 只在 tick 里写，get 时整块拷走 */
static pc_snapshot_t s_pub;

static uint32_t s_total;        /* 累计去重 MAC 数 */
static uint32_t s_frames;       /* 累计计入帧数 */
static uint32_t s_prev_frames;  /* 上一秒的 s_frames，用来算速率 */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static inline uint32_t mac_hash(const uint8_t mac[6])
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) {
        h = (h ^ mac[i]) * 16777619u;
    }
    return h;
}

void people_counter_init(void)
{
    memset(s_tab, 0, sizeof(s_tab));
    memset(&s_pub, 0, sizeof(s_pub));
    s_total = 0;
    s_frames = 0;
    s_prev_frames = 0;
}

void people_counter_feed(const pc_rx_t *rx, uint32_t now_ms)
{
    const uint8_t *mac = rx->mac;

    if (mac[0] & 0x01) {
        return;     /* 组播/广播地址不可能是终端 */
    }
#if PC_IGNORE_RANDOM_MAC
    if (mac[0] & 0x02) {
        return;     /* 随机化 MAC，不计入 */
    }
#endif

    s_frames++;

    pc_slot_t *set = s_tab[mac_hash(mac) & MASK];

    /* 1. 组内找相同 MAC */
    for (int w = 0; w < WAYS; w++) {
        pc_slot_t *s = &set[w];
        if (s->hits != 0 && memcmp(s->mac, mac, 6) == 0) {
            s->last_ms = now_ms;
            s->rssi = rx->rssi;
            if (s->hits != UINT16_MAX) {
                s->hits++;
            }
            return;
        }
    }

    /* 2. 找空槽 */
    pc_slot_t *dst = NULL;
    for (int w = 0; w < WAYS; w++) {
        if (set[w].hits == 0) {
            dst = &set[w];
            break;
        }
    }

    /* 3. 组满了：踢掉最久没见的那个 */
    if (dst == NULL) {
        dst = &set[0];
        for (int w = 1; w < WAYS; w++) {
            if ((int32_t)(set[w].last_ms - dst->last_ms) < 0) {
                dst = &set[w];
            }
        }
    }

    memcpy(dst->mac, mac, 6);
    dst->random = (mac[0] & 0x02) ? 1 : 0;
    dst->rssi = rx->rssi;
    dst->hits = 1;
    dst->last_ms = now_ms;
    s_total++;
}

void people_counter_tick(uint32_t now_ms)
{
    uint16_t cur = 0;
    int32_t rssi_sum = 0;

    /* 512 个槽位扫一遍，1s 一次，开销可以忽略 */
    for (int i = 0; i < SETS; i++) {
        for (int w = 0; w < WAYS; w++) {
            const pc_slot_t *s = &s_tab[i][w];
            if (s->hits == 0) {
                continue;
            }
            if ((int32_t)(now_ms - s->last_ms) <= PC_ACTIVE_WINDOW_MS) {
                cur++;
                rssi_sum += s->rssi;
            }
        }
    }

    uint32_t rate = s_frames - s_prev_frames;
    s_prev_frames = s_frames;

    taskENTER_CRITICAL(&s_mux);
    /* 历史曲线右移一格，新的放在 [0] */
    memmove(&s_pub.hist[1], &s_pub.hist[0], PC_HIST_LEN - 1);
    s_pub.hist[0] = (cur > UINT8_MAX) ? UINT8_MAX : (uint8_t)cur;

    uint8_t hmax = 1;
    for (int i = 0; i < PC_HIST_LEN; i++) {
        if (s_pub.hist[i] > hmax) {
            hmax = s_pub.hist[i];
        }
    }

    s_pub.current = cur;
    if (cur > s_pub.peak) {
        s_pub.peak = cur;
    }
    s_pub.total = s_total;
    s_pub.frames = s_frames;
    s_pub.rate = (rate > 999) ? 999 : (uint16_t)rate;
    s_pub.rssi_avg = (cur > 0) ? (int8_t)(rssi_sum / cur) : 0;
    s_pub.hist_max = hmax;
    taskEXIT_CRITICAL(&s_mux);
}

void people_counter_get(pc_snapshot_t *out)
{
    /* 单核上临界区即可保证与 tick 的写入互斥；拷贝量不到 60 字节 */
    taskENTER_CRITICAL(&s_mux);
    *out = s_pub;
    taskEXIT_CRITICAL(&s_mux);
}
