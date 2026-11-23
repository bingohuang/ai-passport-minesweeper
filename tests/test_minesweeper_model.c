// tests/test_minesweeper_model.c —— 扫雷纯逻辑主机测试(无框架,assert 直断)。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "minesweeper_model.h"

static uint16_t count_mines(const msw_game_t *g) {
    uint16_t n = 0;
    for (uint8_t y = 0; y < MSW_BOARD_H; y++)
        for (uint8_t x = 0; x < MSW_BOARD_W; x++) n += g->cells[y][x].is_mine;
    return n;
}

static uint16_t count_revealed(const msw_game_t *g) {
    uint16_t n = 0;
    for (uint8_t y = 0; y < MSW_BOARD_H; y++)
        for (uint8_t x = 0; x < MSW_BOARD_W; x++) n += g->cells[y][x].revealed;
    return n;
}

// 首点安全:任意起点揭示都不踩雷,且首格 3x3 邻域内无雷。
static void test_first_reveal_is_safe(void) {
    for (uint8_t fy = 0; fy < MSW_BOARD_H; fy++) {
        for (uint8_t fx = 0; fx < MSW_BOARD_W; fx++) {
            msw_game_t g;
            msw_init(&g, 0xC0FFEE + fx + fy * MSW_BOARD_W);
            assert(msw_reveal(&g, fx, fy));
            assert(g.mines_placed);
            assert(g.state == MSW_PLAYING || g.state == MSW_WON);
            assert(!g.cells[fy][fx].is_mine);
            assert(count_mines(&g) == MSW_MINES);
            for (int8_t dy = -1; dy <= 1; dy++) {
                for (int8_t dx = -1; dx <= 1; dx++) {
                    const int nx = (int)fx + dx;
                    const int ny = (int)fy + dy;
                    if (nx >= 0 && nx < MSW_BOARD_W && ny >= 0 && ny < MSW_BOARD_H) {
                        assert(!g.cells[ny][nx].is_mine);
                    }
                }
            }
        }
    }
}

// 泛洪:角上首点(邻数必为 0,因 3x3 无雷)应一次性揭开一大片,且邻数自洽。
static void test_flood_fill_and_adjacency(void) {
    msw_game_t g;
    msw_init(&g, 1234);
    assert(msw_reveal(&g, 0, 0));
    assert(g.revealed_safe == count_revealed(&g));
    assert(g.revealed_safe >= 10);  // 角上开角至少揭开一片
    // 所有已揭示格邻数正确;已揭示非雷。
    for (uint8_t y = 0; y < MSW_BOARD_H; y++) {
        for (uint8_t x = 0; x < MSW_BOARD_W; x++) {
            const msw_cell_t *c = &g.cells[y][x];
            if (!c->revealed) continue;
            assert(!c->is_mine);
            uint8_t expect = 0;
            for (int8_t dy = -1; dy <= 1; dy++) {
                for (int8_t dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    const int nx = (int)x + dx;
                    const int ny = (int)y + dy;
                    if (nx >= 0 && nx < MSW_BOARD_W && ny >= 0 && ny < MSW_BOARD_H)
                        expect += g.cells[ny][nx].is_mine;
                }
            }
            assert(c->adjacent == expect);
        }
    }
}

static void test_flag_rules(void) {
    msw_game_t g;
    msw_init(&g, 42);
    // 未布雷也可插旗。
    assert(msw_toggle_flag(&g, 3, 3));
    assert(g.cells[3][3].flagged);
    assert(msw_flags_left(&g) == MSW_MINES - 1);
    assert(msw_toggle_flag(&g, 3, 3));
    assert(msw_flags_left(&g) == MSW_MINES);
    // 旗格不可揭示。
    assert(msw_toggle_flag(&g, 0, 0));
    assert(!msw_reveal(&g, 0, 0));
    assert(!g.cells[0][0].revealed);
    assert(msw_toggle_flag(&g, 0, 0));  // 摘掉
    // 旗数到顶后拒绝再加(10 面旗铺两行)。
    for (uint8_t i = 0; i < MSW_MINES; i++) assert(msw_toggle_flag(&g, i % MSW_BOARD_W, 8 - i / MSW_BOARD_W));
    assert(g.flags_placed == MSW_MINES);
    assert(!msw_toggle_flag(&g, 4, 4));
    // 已揭示格不可插旗。
    msw_game_t g2;
    msw_init(&g2, 7);
    assert(msw_reveal(&g2, 0, 0));
    assert(!msw_toggle_flag(&g2, 0, 0));
}

// 借助模型自身的邻数信息避开所有雷,打出一局胜利。
static void test_win_flow(void) {
    msw_game_t g;
    msw_init(&g, 2026);
    assert(msw_reveal(&g, 4, 4));
    while (g.state == MSW_PLAYING) {
        // 找一个未揭示、未标记、非雷的格揭示(测试直读内部状态驱动)。
        bool progressed = false;
        for (uint8_t y = 0; y < MSW_BOARD_H && !progressed; y++) {
            for (uint8_t x = 0; x < MSW_BOARD_W && !progressed; x++) {
                msw_cell_t *c = &g.cells[y][x];
                if (!c->revealed && !c->flagged && !c->is_mine) {
                    assert(msw_reveal(&g, x, y));
                    progressed = true;
                }
            }
        }
        assert(progressed);
    }
    assert(g.state == MSW_WON);
    assert(g.revealed_safe == g.safe_total);
    // 终局后拒绝一切操作。
    assert(!msw_reveal(&g, 0, 0));
    assert(!msw_toggle_flag(&g, 0, 0));
}

static void test_loss_reveals_mines_but_keeps_flags(void) {
    msw_game_t g;
    msw_init(&g, 99);
    assert(msw_reveal(&g, 0, 0));
    // 找一颗雷:先插旗验证踩雷后旗格保持旗标。
    uint8_t mx = 0, my = 0;
    bool found = false;
    for (uint8_t y = 0; y < MSW_BOARD_H && !found; y++) {
        for (uint8_t x = 0; x < MSW_BOARD_W && !found; x++) {
            if (g.cells[y][x].is_mine) {
                mx = x;
                my = y;
                found = true;
            }
        }
    }
    assert(found);
    assert(msw_toggle_flag(&g, mx, my));
    // 找另一颗雷踩掉。
    for (uint8_t y = 0; y < MSW_BOARD_H; y++) {
        for (uint8_t x = 0; x < MSW_BOARD_W; x++) {
            if (g.cells[y][x].is_mine && !(x == mx && y == my)) {
                assert(msw_reveal(&g, x, y));
                assert(g.state == MSW_LOST);
                // 所有雷亮出,但已旗格仍只算旗标(未 revealed)。
                assert(g.cells[my][mx].flagged);
                assert(!g.cells[my][mx].revealed);
                uint16_t revealed_mines = 0;
                for (uint8_t ry = 0; ry < MSW_BOARD_H; ry++)
                    for (uint8_t rx = 0; rx < MSW_BOARD_W; rx++)
                        if (g.cells[ry][rx].is_mine && g.cells[ry][rx].revealed) revealed_mines++;
                assert(revealed_mines == MSW_MINES - 1);
                return;
            }
        }
    }
    assert(false);  // 找不到第二颗雷,测试数据错误
}

// 同种子布局必须完全一致;不同起点布雷排除区不同。
static void test_determinism(void) {
    msw_game_t a, b;
    msw_init(&a, 777);
    msw_init(&b, 777);
    assert(msw_reveal(&a, 0, 0));
    assert(msw_reveal(&b, 0, 0));
    assert(memcmp(a.cells, b.cells, sizeof(a.cells)) == 0);
    assert(a.state == b.state);
}

static void test_stats(void) {
    msw_stats_t s;
    msw_stats_init(&s);
    assert(s.best_s == 0 && s.wins == 0 && s.games == 0);
    msw_stats_record(&s, false, 30);  // 输局不计最佳用时
    assert(s.games == 1 && s.wins == 0 && s.best_s == 0);
    msw_stats_record(&s, true, 45);
    assert(s.games == 2 && s.wins == 1 && s.best_s == 45);
    msw_stats_record(&s, true, 120);  // 更慢不刷新最佳
    assert(s.best_s == 45);
    msw_stats_record(&s, true, 12);   // 更快刷新
    assert(s.best_s == 12 && s.wins == 3 && s.games == 4);
}

int main(void) {
    test_first_reveal_is_safe();
    test_flood_fill_and_adjacency();
    test_flag_rules();
    test_win_flow();
    test_loss_reveals_mines_but_keeps_flags();
    test_determinism();
    test_stats();
    printf("test_minesweeper_model: OK\n");
    return 0;
}
