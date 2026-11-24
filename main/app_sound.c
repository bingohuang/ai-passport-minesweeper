// main/app_sound.c —— 音效合成与发声任务(见 app_sound.h)。
//
// 沿用 demo_audio 的发声模式:bsp_audio_write 会阻塞,故全部 PCM 输出放在
// 独立任务;16kHz 下 I2S DMA 只缓冲约 90ms,任务优先级要高于 LVGL 渲染。
// 每段方波首尾加约 1.5ms 线性包络,避免爆音;踩雷用线性衰减的噪声。
#include "app_sound.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "app_sound";

#define SAMPLE_RATE   16000
#define CHUNK_SAMPLES 512           // 32ms 一块,1KB 静态缓冲
#define VOLUME        70
#define SFX_QUEUE_LEN 6

typedef struct {
    uint16_t hz;                    // 0 = 休止
    uint16_t ms;
    int16_t amp;                    // 方波幅度(满幅 32767,移动音特意放轻)
} sfx_seg_t;

static const sfx_seg_t START_SEGS[] = {
    {523, 70, 6000}, {659, 70, 6000}, {784, 140, 6000},
};
static const sfx_seg_t MOVE_SEGS[] = {
    {1200, 25, 3500},
};
static const sfx_seg_t REVEAL_SEGS[] = {
    {660, 45, 5500}, {990, 60, 5500},
};
static const sfx_seg_t FLAG_SEGS[] = {
    {1568, 35, 5500}, {0, 30, 0}, {1568, 35, 5500},
};
static const sfx_seg_t WIN_SEGS[] = {
    {523, 90, 6000}, {659, 90, 6000}, {784, 90, 6000},
    {1046, 90, 6000}, {0, 40, 0}, {1046, 260, 6500},
};
static const sfx_seg_t LOSE_SEGS[] = {
    {392, 140, 6000}, {330, 140, 6000}, {262, 300, 6000},
};

typedef struct {
    const sfx_seg_t *segs;
    uint8_t count;
    bool noise;                     // true = 整段为衰减噪声(踩雷)
    uint16_t noise_ms;
} sfx_def_t;

static const sfx_def_t EFFECTS[] = {
    [SFX_START]  = { START_SEGS,  sizeof(START_SEGS) / sizeof(sfx_seg_t),  false, 0 },
    [SFX_MOVE]   = { MOVE_SEGS,   sizeof(MOVE_SEGS) / sizeof(sfx_seg_t),   false, 0 },
    [SFX_REVEAL] = { REVEAL_SEGS, sizeof(REVEAL_SEGS) / sizeof(sfx_seg_t), false, 0 },
    [SFX_FLAG]   = { FLAG_SEGS,   sizeof(FLAG_SEGS) / sizeof(sfx_seg_t),   false, 0 },
    [SFX_BOOM]   = { NULL, 0, true, 400 },
    [SFX_WIN]    = { WIN_SEGS,    sizeof(WIN_SEGS) / sizeof(sfx_seg_t),    false, 0 },
    [SFX_LOSE]   = { LOSE_SEGS,   sizeof(LOSE_SEGS) / sizeof(sfx_seg_t),   false, 0 },
};

static QueueHandle_t s_queue;
static int16_t s_buf[CHUNK_SAMPLES];

static bool write_buf(int n) {
    return bsp_audio_write(s_buf, (size_t)n * sizeof(int16_t)) == ESP_OK;
}

// 一段方波/休止;首尾 ramp 个采样做线性包络。
static void play_seg(const sfx_seg_t *seg) {
    int total = (int)seg->ms * SAMPLE_RATE / 1000;
    const int ramp = 24;            // 1.5ms @16k
    int pos = 0;
    while (pos < total) {
        int n = total - pos < CHUNK_SAMPLES ? total - pos : CHUNK_SAMPLES;
        for (int i = 0; i < n; i++) {
            const int idx = pos + i;
            int env = seg->amp;
            if (idx < ramp) env = env * idx / ramp;
            if (total - idx < ramp) env = env * (total - idx) / ramp;
            if (seg->hz == 0) {
                s_buf[i] = 0;
            } else {
                // 段内相位:每周期前半为正,后半为负。
                const uint32_t phase = ((uint32_t)idx * seg->hz) % SAMPLE_RATE;
                s_buf[i] = (int16_t)(phase < SAMPLE_RATE / 2 ? env : -env);
            }
        }
        if (!write_buf(n)) return;
        pos += n;
    }
}

// 踩雷:白噪声,幅度线性衰减到 0。
static void play_noise(uint16_t ms) {
    int total = (int)ms * SAMPLE_RATE / 1000;
    int pos = 0;
    while (pos < total) {
        int n = total - pos < CHUNK_SAMPLES ? total - pos : CHUNK_SAMPLES;
        for (int i = 0; i < n; i++) {
            const int idx = pos + i;
            const int amp = 8000 * (total - idx) / total;
            const int rnd = (int)(esp_random() & 0xFFFF) - 32768;
            s_buf[i] = (int16_t)(rnd * amp / 32768);
        }
        if (!write_buf(n)) return;
        pos += n;
    }
}

static void sound_task(void *arg) {
    (void)arg;
    bool ready = false;
    bool dead = false;              // 音频硬件初始化失败:静默,游戏照常
    for (;;) {
        app_sfx_t sfx;
        if (xQueueReceive(s_queue, &sfx, portMAX_DELAY) != pdTRUE) continue;
        if (dead || (unsigned)sfx > SFX_LOSE) continue;
        if (!ready) {
            if (bsp_audio_init() != ESP_OK ||
                bsp_audio_set_format(SAMPLE_RATE, 16, 1) != ESP_OK) {
                ESP_LOGW(TAG, "音频初始化失败,音效关闭");
                dead = true;
                continue;
            }
            bsp_audio_set_volume(VOLUME);
            ready = true;
        }
        const sfx_def_t *def = &EFFECTS[sfx];
        if (def->noise) {
            play_noise(def->noise_ms);
            continue;
        }
        for (uint8_t i = 0; i < def->count; i++) play_seg(&def->segs[i]);
    }
}

esp_err_t app_sound_init(void) {
    s_queue = xQueueCreate(SFX_QUEUE_LEN, sizeof(app_sfx_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    // 优先级高于 LVGL 渲染,保证 I2S 供数不被画面刷新饿死(见硬件指南 8.1)。
    if (xTaskCreate(sound_task, "app_sound", 4096, NULL, 6, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_sound_play(app_sfx_t sfx) {
    if (!s_queue) return;
    (void)xQueueSend(s_queue, &sfx, 0);
}
