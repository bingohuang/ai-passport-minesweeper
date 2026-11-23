// main/app_minesweeper.h —— 扫雷应用:页面、棋盘渲染、三键交互、战绩持久化胶水。
//
// 生命周期:app_minesweeper_start() 由 main 在 LVGL 锁内调用一次,创建全部界面;
// 之后 app_minesweeper_on_key() 可在任意任务调用(仅入队),模型修改与渲染
// 统一在 LVGL 任务的泵定时器里完成,避免跨任务竞争。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

// 创建页面并进入标题页。需先完成 bsp_display_init()/bsp_lvgl_init()。
esp_err_t app_minesweeper_start(void);

// 上报按键事件;非阻塞,失败(队列满)直接丢弃该事件。
void app_minesweeper_on_key(bsp_btn_t btn, bsp_btn_ev_t ev);
