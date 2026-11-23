// main/minesweeper_model.h —— 扫雷纯逻辑:棋盘、布雷、揭示、标记、胜负、战绩。
//
// 不依赖 ESP-IDF/LVGL,由 tests/test_minesweeper_model.c 在主机上直接编译测试。
// 首次揭示时才布雷(点格及其 3x3 邻居排除),保证第一步必安全;布雷用 xorshift32,
// 种子由应用层从 esp_random() 取,逻辑层保持确定性以便测试。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MSW_BOARD_W 9
#define MSW_BOARD_H 9
#define MSW_MINES   10

typedef enum {
    MSW_READY = 0,  // 未布雷,等待首次揭示
    MSW_PLAYING,
    MSW_WON,
    MSW_LOST,
} msw_state_t;

typedef struct {
    uint8_t adjacent;   // 周围雷数 0..8(雷格无意义)
    uint8_t is_mine : 1;
    uint8_t revealed : 1;
    uint8_t flagged : 1;
} msw_cell_t;

typedef struct {
    msw_cell_t cells[MSW_BOARD_H][MSW_BOARD_W];
    uint16_t mines_total;    // 雷数
    uint16_t flags_placed;   // 已插旗数
    uint16_t revealed_safe;  // 已揭示的安全格数
    uint16_t safe_total;     // 总安全格数 = W*H - mines
    msw_state_t state;
    uint32_t rng;            // xorshift32 状态
    bool mines_placed;
} msw_game_t;

// 战绩:掉电不丢失,经 BSP NVS 层存取。best_s == 0 表示尚无胜利记录。
typedef struct {
    uint32_t best_s;
    uint32_t wins;
    uint32_t games;
} msw_stats_t;

void msw_init(msw_game_t *g, uint32_t seed);

// 揭示一格。首次揭示先布雷。踩雷判负并亮出所有未标记的雷;旗标无效格直接忽略。
// 返回该格是否被成功揭示(用于界面反馈)。
bool msw_reveal(msw_game_t *g, uint8_t x, uint8_t y);

// 切换旗标。已揭示格、终局、或旗数已达雷数时忽略。返回是否发生变化。
bool msw_toggle_flag(msw_game_t *g, uint8_t x, uint8_t y);

// 剩余可插旗数 = mines_total - flags_placed。
uint16_t msw_flags_left(const msw_game_t *g);

// 棋盘内 (x,y) 周围的雷数(不依赖格子是否已揭示)。
uint8_t msw_adjacent_at(const msw_game_t *g, uint8_t x, uint8_t y);

void msw_stats_init(msw_stats_t *s);

// 记录一局结束:games 总是 +1;胜利时 wins +1 并刷新最佳用时(首次胜利或更短)。
void msw_stats_record(msw_stats_t *s, bool won, uint32_t elapsed_s);
