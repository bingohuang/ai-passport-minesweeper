// main/minesweeper_model.c —— 扫雷纯逻辑实现(见 minesweeper_model.h)。
#include "minesweeper_model.h"

#include <string.h>

static uint32_t msw_next_rand(msw_game_t *g) {
    // xorshift32:足够均匀,且完全由种子决定,便于主机测试复现。
    uint32_t x = g->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g->rng = x ? x : 0x9E3779B9u;  // 全零会锁死,兜底换一个非零常量
    return g->rng;
}

static bool msw_in_bounds(int8_t x, int8_t y) {
    return x >= 0 && x < MSW_BOARD_W && y >= 0 && y < MSW_BOARD_H;
}

uint8_t msw_adjacent_at(const msw_game_t *g, uint8_t x, uint8_t y) {
    return g->cells[y][x].adjacent;
}

void msw_init(msw_game_t *g, uint32_t seed) {
    memset(g, 0, sizeof(*g));
    g->mines_total = MSW_MINES;
    g->safe_total = MSW_BOARD_W * MSW_BOARD_H - MSW_MINES;
    g->state = MSW_READY;
    g->rng = seed ? seed : 0x1D2B3A49u;
}

// 布雷:排除 (skip_x,skip_y) 的 3x3 邻域,再随机放 mines_total 颗并算邻数。
static void msw_place_mines(msw_game_t *g, uint8_t skip_x, uint8_t skip_y) {
    // 先把候选格(排除区之外)坐标收进数组,随后清零 is_mine,再写回抽中的雷。
    uint8_t cand[MSW_BOARD_W * MSW_BOARD_H];
    uint16_t n = 0;
    for (uint8_t y = 0; y < MSW_BOARD_H; y++) {
        for (uint8_t x = 0; x < MSW_BOARD_W; x++) {
            const int8_t dx = (int8_t)x - (int8_t)skip_x;
            const int8_t dy = (int8_t)y - (int8_t)skip_y;
            if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1) continue;
            cand[n++] = (uint8_t)(y * MSW_BOARD_W + x);
        }
    }
    for (uint8_t y = 0; y < MSW_BOARD_H; y++)
        for (uint8_t x = 0; x < MSW_BOARD_W; x++) g->cells[y][x].is_mine = 0;
    // 候选头部往后做 partial Fisher–Yates,抽 mines_total 颗。
    for (uint16_t placed = 0; placed < g->mines_total; placed++) {
        const uint16_t pick = placed + (uint16_t)(msw_next_rand(g) % (n - placed));
        const uint8_t tmp = cand[pick];
        cand[pick] = cand[placed];
        cand[placed] = tmp;
        g->cells[tmp / MSW_BOARD_W][tmp % MSW_BOARD_W].is_mine = 1;
    }
    for (uint8_t y = 0; y < MSW_BOARD_H; y++) {
        for (uint8_t x = 0; x < MSW_BOARD_W; x++) {
            msw_cell_t *c = &g->cells[y][x];
            if (c->is_mine) continue;
            uint8_t count = 0;
            for (int8_t dy = -1; dy <= 1; dy++) {
                for (int8_t dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    const int8_t nx = (int8_t)x + dx;
                    const int8_t ny = (int8_t)y + dy;
                    if (msw_in_bounds(nx, ny) && g->cells[ny][nx].is_mine) count++;
                }
            }
            c->adjacent = count;
        }
    }
    g->mines_placed = true;
}

// 泛洪揭示:迭代栈,棋盘最多 81 格。入栈即认领(revealed=1),
// 保证每格最多入栈一次,定长 81 的栈不会溢出。
static void msw_flood_reveal(msw_game_t *g, uint8_t x, uint8_t y) {
    uint8_t stack[MSW_BOARD_W * MSW_BOARD_H][2];
    uint16_t sp = 0;
    g->cells[y][x].revealed = 1;
    g->revealed_safe++;
    stack[sp][0] = x;
    stack[sp][1] = y;
    sp++;
    while (sp > 0) {
        sp--;
        const uint8_t cx = stack[sp][0];
        const uint8_t cy = stack[sp][1];
        if (g->cells[cy][cx].adjacent != 0) continue;  // 数字格不扩散
        for (int8_t dy = -1; dy <= 1; dy++) {
            for (int8_t dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;
                const int8_t nx = (int8_t)cx + dx;
                const int8_t ny = (int8_t)cy + dy;
                if (!msw_in_bounds(nx, ny)) continue;
                msw_cell_t *nc = &g->cells[ny][nx];
                if (!nc->revealed && !nc->flagged) {
                    nc->revealed = 1;
                    g->revealed_safe++;
                    stack[sp][0] = (uint8_t)nx;
                    stack[sp][1] = (uint8_t)ny;
                    sp++;
                }
            }
        }
    }
}

bool msw_reveal(msw_game_t *g, uint8_t x, uint8_t y) {
    if (g->state == MSW_WON || g->state == MSW_LOST) return false;
    if (x >= MSW_BOARD_W || y >= MSW_BOARD_H) return false;
    msw_cell_t *c = &g->cells[y][x];
    if (c->revealed || c->flagged) return false;

    if (!g->mines_placed) {
        msw_place_mines(g, x, y);
        g->state = MSW_PLAYING;
    }
    if (c->is_mine) {
        c->revealed = 1;
        for (uint8_t my = 0; my < MSW_BOARD_H; my++) {
            for (uint8_t mx = 0; mx < MSW_BOARD_W; mx++) {
                msw_cell_t *mc = &g->cells[my][mx];
                if (mc->is_mine && !mc->flagged) mc->revealed = 1;
            }
        }
        g->state = MSW_LOST;
        return true;
    }
    msw_flood_reveal(g, x, y);
    if (g->revealed_safe >= g->safe_total) g->state = MSW_WON;
    return true;
}

bool msw_toggle_flag(msw_game_t *g, uint8_t x, uint8_t y) {
    if (g->state == MSW_WON || g->state == MSW_LOST) return false;
    if (x >= MSW_BOARD_W || y >= MSW_BOARD_H) return false;
    msw_cell_t *c = &g->cells[y][x];
    if (c->revealed) return false;
    if (!c->flagged && g->flags_placed >= g->mines_total) return false;
    c->flagged = !c->flagged;
    g->flags_placed += c->flagged ? 1 : -1;
    return true;
}

uint16_t msw_flags_left(const msw_game_t *g) {
    return g->flags_placed <= g->mines_total ? (uint16_t)(g->mines_total - g->flags_placed) : 0;
}

void msw_stats_init(msw_stats_t *s) {
    s->best_s = 0;
    s->wins = 0;
    s->games = 0;
}

void msw_stats_record(msw_stats_t *s, bool won, uint32_t elapsed_s) {
    s->games++;
    if (!won) return;
    s->wins++;
    if (s->best_s == 0 || elapsed_s < s->best_s) s->best_s = elapsed_s;
}
