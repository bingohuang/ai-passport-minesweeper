// main/app_sound.h —— 扫雷音效:开局/移动/开雷/插旗/踩雷/胜利/失败。
//
// app_sound_play() 只入队,可在 LVGL 任务等非阻塞上下文调用;
// 实际发声在独立任务里完成(16kHz 16bit 单声道方波/噪声合成)。
#pragma once

#include "esp_err.h"

typedef enum {
    SFX_START = 0,  // 开局:上行三音
    SFX_MOVE,       // 光标移动:短促轻滴答
    SFX_REVEAL,     // 开雷:两连跳音
    SFX_FLAG,       // 插旗/拔旗:双响叮
    SFX_BOOM,       // 踩雷:衰减噪声爆音
    SFX_WIN,        // 胜利:上行琶音收尾长音
    SFX_LOSE,       // 失败:下行三音
} app_sfx_t;

// 创建音效队列与发声任务。音频硬件在首次发声时才初始化;
// 失败(内存不足)返回错误,之后 app_sound_play() 为无操作。
esp_err_t app_sound_init(void);

// 播放一个音效;非阻塞,队列满时丢弃(快速连按的移动音允许丢)。
void app_sound_play(app_sfx_t sfx);
