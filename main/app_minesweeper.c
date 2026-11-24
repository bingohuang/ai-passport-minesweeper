// main/app_minesweeper.c —— 扫雷应用实现(见 app_minesweeper.h)。
//
// 界面为暗色主题的全新设计,不使用基线 demo 的 ui_pixel 外壳,中文界面:
//   标题页:字标 + 战绩面板 + 开始按钮;
//   游戏页:状态条(剩余旗数/表情/计时)、9x9 棋盘(I4 调色板 canvas 自绘)、底部提示;
//   结算浮层:胜负 + 本局用时 + 战绩。
// 中文字形走应用子集字体 assets/fonts/app_font_cjk_{14,20}.c
// (思源黑体 SC,lv_font_conv 1.5.3 生成;Montserrat 仅作 ASCII/符号 fallback),见 app_fonts_init()。
//
// 按键(全局三键):
//   短按 UP/DOWN = 抬起时移一格,连按连走;方向默认上/下,横向模式下为左/右
//   长按 UP = 揭示;长按 DOWN(中间键)= 暂停菜单:UP/DOWN 选择,单击 OK 确认
//   (继续/重新开始/返回主页),再长按 DOWN 直接继续
//   单击 OK = 切换纵向/横向移动;长按 OK = 插旗/拔旗
//
// 首次启动先显示操作指南页,短按下键(OK)确认后写入 NVS 标记,
// 之后启动直接进入标题页。
#include "app_minesweeper.h"

#include <stdio.h>
#include <string.h>

#include "bsp_battery.h"
#include "bsp_storage.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "minesweeper_model.h"

static const char *TAG = "minesweeper";

// ---------------------------------------------------------------------------
// 布局与配色(240x320,圆角屏;内容避开四角 R30 蒙版区)
// ---------------------------------------------------------------------------
#define SCREEN_W     240
#define SCREEN_H     320
#define BOARD_X      12
#define BOARD_Y      66
#define CELL         24
#define BOARD_PX     (MSW_BOARD_W * CELL)  // 216
#define HDR_Y        30
#define FOOT_Y       288
#define FOOT2_Y      304

#define COLOR_BG          0x0D1117
#define COLOR_PANEL       0x161D27
#define COLOR_PANEL_EDGE  0x2A3442
#define COLOR_TEXT        0xE8EEF6
#define COLOR_DIM         0x7C8794
#define COLOR_ACCENT      0xF2D024

// I4 棋盘调色板,恰好 16 色:0-7 语义色,8-15 = 数字 1..8。
enum {
    PAL_BG = 0,   // 画布底
    PAL_HIDDEN,   // 未揭示格
    PAL_EDGE,     // 未揭示格边框(暗)
    PAL_OPEN,     // 已揭示格
    PAL_CURSOR,   // 光标描边
    PAL_FLAG,     // 旗
    PAL_MINE,     // 雷体
    PAL_BOOM,     // 踩中的雷底
    PAL_DIGIT1, PAL_DIGIT2, PAL_DIGIT3, PAL_DIGIT4,
    PAL_DIGIT5, PAL_DIGIT6, PAL_DIGIT7, PAL_DIGIT8,
};

static const uint32_t PALETTE[] = {
    [PAL_BG]     = COLOR_BG,
    [PAL_HIDDEN] = 0x2E3A4E,
    [PAL_EDGE]   = 0x1F2837,
    [PAL_OPEN]   = 0x11161D,
    [PAL_CURSOR] = COLOR_ACCENT,
    [PAL_FLAG]   = 0xF2555A,
    [PAL_MINE]   = 0x05070A,
    [PAL_BOOM]   = 0x8C1D18,
    [PAL_DIGIT1] = 0x58A6FF,
    [PAL_DIGIT2] = 0x3FB950,
    [PAL_DIGIT3] = 0xF2555A,
    [PAL_DIGIT4] = 0xB78CFF,
    [PAL_DIGIT5] = 0xFFB224,
    [PAL_DIGIT6] = 0x39C5CF,
    [PAL_DIGIT7] = 0xFF7EB6,
    [PAL_DIGIT8] = 0xA6B0BC,
};

#define INPUT_DEPTH   12
#define NS            "mines"  // NVS 命名空间
#define KEY_BEST      "best_s"
#define KEY_WINS      "wins"
#define KEY_GAMES     "games"
#define KEY_GUIDE     "guide"  // 操作指南已确认标记

// ---------------------------------------------------------------------------
// 内部状态
// ---------------------------------------------------------------------------
typedef enum {
    SCR_TITLE = 0,
    SCR_GAME,
    SCR_GUIDE,
} app_screen_t;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} input_event_t;

static msw_game_t s_game;
static msw_stats_t s_stats;
static app_screen_t s_screen = SCR_TITLE;
static uint8_t s_cx = MSW_BOARD_W / 2;
static uint8_t s_cy = MSW_BOARD_H / 2;
static bool s_horizontal;         // false = 上/下键纵向移动,true = 横向(OK 单击切换)
static bool s_paused;             // 暂停菜单打开中,计时冻结
static int64_t s_pause_start_us;  // 进入暂停的时刻,继续时补偿计时起点
static uint8_t s_pause_sel;       // 暂停菜单当前选中项
static bool s_step_armed[3];      // 该键在游戏内按下,抬起时走一步;长按则消费掉
static int8_t s_boom_x = -1;      // 终局被踩中的雷格(-1 = 无)
static int8_t s_boom_y = -1;
static bool s_board_dirty = true;
static bool s_end_recorded;
static int64_t s_start_us;
static uint32_t s_elapsed_s;
static uint32_t s_last_tick_ms;   // 计时条刷新节流
static int s_last_soc = -2;       // 电量刷新节流;-2 = 尚未读

static lv_obj_t *s_title_scr;
static lv_obj_t *s_game_scr;
static lv_obj_t *s_guide_scr;
static lv_obj_t *s_best_label;
static lv_obj_t *s_wins_label;
static lv_obj_t *s_games_label;
static lv_obj_t *s_batt_title_label;
static lv_obj_t *s_batt_game_label;
static lv_obj_t *s_mines_label;
static lv_obj_t *s_face_label;
static lv_obj_t *s_timer_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_hint2_label;
static lv_obj_t *s_overlay;
static lv_obj_t *s_overlay_title;
static lv_obj_t *s_overlay_line;
static lv_obj_t *s_pause_overlay;
static lv_obj_t *s_pause_items[3];
static lv_obj_t *s_board_canvas;
static lv_timer_t *s_pump_timer;

static QueueHandle_t s_input_queue;

LV_DRAW_BUF_DEFINE_STATIC(s_board_buf, BOARD_PX, BOARD_PX, LV_COLOR_FORMAT_I4);

// ---------------------------------------------------------------------------
// 字体:应用子集字体(思源黑体 SC,覆盖界面全部汉字与 ASCII)为主,
// Montserrat 只补 LVGL 符号等子集外的字形。子集由 lv_font_conv 1.5.3 生成,
// 可写描述符 + fallback 的写法见 docs/development/engineering/lvgl-chinese-fonts.md。
// ---------------------------------------------------------------------------
LV_FONT_DECLARE(app_font_cjk_14);
LV_FONT_DECLARE(app_font_cjk_20);
static lv_font_t s_font_14;
static lv_font_t s_font_20;

static void app_fonts_init(void) {
    s_font_14 = app_font_cjk_14;
    s_font_14.fallback = &lv_font_montserrat_14;
    s_font_20 = app_font_cjk_20;
    s_font_20.fallback = &lv_font_montserrat_20;
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static lv_obj_t *label_create(lv_obj_t *parent, const lv_font_t *font,
                              uint32_t color, lv_align_t align, int x, int y) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_align(label, align, x, y);
    return label;
}

// ---------------------------------------------------------------------------
// 棋盘 canvas 绘制(调色板下标写像素,同 tetris 的 canvas_px 手法)
// ---------------------------------------------------------------------------
static void canvas_px(int x, int y, uint8_t color) {
    // 防御:坐标越界说明渲染逻辑有回归,宁可丢点不可写穿画布。
    if ((unsigned)x >= BOARD_PX || (unsigned)y >= BOARD_PX) return;
    lv_draw_buf_t *draw_buf = lv_canvas_get_draw_buf(s_board_canvas);
    uint8_t *data = lv_draw_buf_goto_xy(draw_buf, x, y);
    if (!data) return;
    uint8_t shift = (uint8_t)(4 - 4 * (x & 1));
    *data = (uint8_t)((*data & ~(0x0FU << shift)) | ((color & 0x0FU) << shift));
}

static void canvas_fill(int x, int y, int w, int h, uint8_t color) {
    for (int py = y; py < y + h; py++)
        for (int px = x; px < x + w; px++) canvas_px(px, py, color);
}

// 3x5 点阵数字,放大 3 倍后 9x15,在 24px 格内居中。
static const uint8_t DIGITS[8][5] = {
    {0x02, 0x06, 0x02, 0x02, 0x07},  // 1
    {0x07, 0x01, 0x07, 0x04, 0x07},  // 2
    {0x07, 0x01, 0x07, 0x01, 0x07},  // 3
    {0x05, 0x05, 0x07, 0x01, 0x01},  // 4
    {0x07, 0x04, 0x07, 0x01, 0x07},  // 5
    {0x07, 0x04, 0x07, 0x05, 0x07},  // 6
    {0x07, 0x01, 0x01, 0x02, 0x02},  // 7
    {0x07, 0x05, 0x07, 0x05, 0x07},  // 8
};

static void draw_digit(int ox, int oy, uint8_t digit) {
    if (digit < 1 || digit > 8) return;
    const uint8_t *rows = DIGITS[digit - 1];
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 3; col++) {
            if (!(rows[row] & (1 << (2 - col)))) continue;
            canvas_fill(ox + col * 3, oy + row * 3, 3, 3,
                        (uint8_t)(PAL_DIGIT1 + digit - 1));
        }
    }
}

static void draw_flag(int ox, int oy) {
    canvas_fill(ox + 8, oy + 4, 2, 16, PAL_FLAG);   // 旗杆
    for (int i = 0; i < 9; i++) {                   // 三角旗面(右向)
        canvas_fill(ox + 10, oy + 4 + i, 9 - i, 1, PAL_FLAG);
    }
    canvas_fill(ox + 5, oy + 20, 8, 2, PAL_FLAG);   // 底座
}

static void draw_mine(int ox, int oy) {
    const int cx = ox + 12;
    const int cy = oy + 12;
    for (int dy = -7; dy <= 7; dy++) {              // 实心圆雷体
        for (int dx = -7; dx <= 7; dx++) {
            if (dx * dx + dy * dy > 49) continue;
            canvas_px(cx + dx, cy + dy, PAL_MINE);
        }
    }
    canvas_fill(cx - 4, cy - 4, 2, 2, PAL_OPEN);    // 左上高光
    canvas_px(cx - 3, cy + 3, PAL_OPEN);            // 右下微光,避免一团黑
}

// 画一格;cursor=true 时叠加黄色描边。
static void draw_cell(uint8_t x, uint8_t y, bool cursor) {
    const int ox = x * CELL;
    const int oy = y * CELL;
    const msw_cell_t *c = &s_game.cells[y][x];

    if (!c->revealed) {
        canvas_fill(ox, oy, CELL, CELL, PAL_HIDDEN);
        canvas_fill(ox, oy, CELL, 1, PAL_EDGE);
        canvas_fill(ox, oy, 1, CELL, PAL_EDGE);
        canvas_fill(ox, oy + CELL - 1, CELL, 1, PAL_EDGE);
        canvas_fill(ox + CELL - 1, oy, 1, CELL, PAL_EDGE);
        if (c->flagged) draw_flag(ox, oy);
    } else if (c->is_mine) {
        // 被踩中的那格用爆底,其余亮出的雷同底色。
        const bool boom = (s_game.state == MSW_LOST && x == s_boom_x && y == s_boom_y);
        canvas_fill(ox, oy, CELL, CELL, boom ? PAL_BOOM : PAL_OPEN);
        draw_mine(ox, oy);
    } else {
        canvas_fill(ox, oy, CELL, CELL, PAL_OPEN);
        if (c->adjacent > 0) draw_digit(ox + 7, oy + 4, c->adjacent);
    }

    if (cursor) {
        canvas_fill(ox + 1, oy + 1, CELL - 2, 2, PAL_CURSOR);
        canvas_fill(ox + 1, oy + CELL - 3, CELL - 2, 2, PAL_CURSOR);
        canvas_fill(ox + 1, oy + 1, 2, CELL - 2, PAL_CURSOR);
        canvas_fill(ox + CELL - 3, oy + 1, 2, CELL - 2, PAL_CURSOR);
    }
}

static void render_board(void) {
    for (uint8_t y = 0; y < MSW_BOARD_H; y++)
        for (uint8_t x = 0; x < MSW_BOARD_W; x++)
            draw_cell(x, y, x == s_cx && y == s_cy);
    lv_obj_invalidate(s_board_canvas);
}

// ---------------------------------------------------------------------------
// 战绩持久化
// ---------------------------------------------------------------------------
static void stats_load(void) {
    msw_stats_init(&s_stats);
    if (bsp_storage_get_u32(KEY_BEST, &s_stats.best_s) != ESP_OK ||
        bsp_storage_get_u32(KEY_WINS, &s_stats.wins) != ESP_OK ||
        bsp_storage_get_u32(KEY_GAMES, &s_stats.games) != ESP_OK) {
        ESP_LOGW(TAG, "战绩读取失败,使用空记录");
        msw_stats_init(&s_stats);
    }
}

static void stats_save(void) {
    if (bsp_storage_set_u32(KEY_BEST, s_stats.best_s) != ESP_OK ||
        bsp_storage_set_u32(KEY_WINS, s_stats.wins) != ESP_OK ||
        bsp_storage_set_u32(KEY_GAMES, s_stats.games) != ESP_OK) {
        ESP_LOGW(TAG, "战绩写入失败,本次记录未保存");
    }
}

static void refresh_stats_labels(void) {
    char buf[24];
    if (s_stats.best_s == 0) {
        lv_label_set_text(s_best_label, "最佳 --:--");
    } else {
        snprintf(buf, sizeof(buf), "最佳 %02u:%02u",
                 (unsigned)(s_stats.best_s / 60), (unsigned)(s_stats.best_s % 60));
        lv_label_set_text(s_best_label, buf);
    }
    snprintf(buf, sizeof(buf), "胜场 %u", (unsigned)s_stats.wins);
    lv_label_set_text(s_wins_label, buf);
    snprintf(buf, sizeof(buf), "总局 %u", (unsigned)s_stats.games);
    lv_label_set_text(s_games_label, buf);
}

// ---------------------------------------------------------------------------
// 界面搭建(全部在 LVGL 任务上下文调用)
// ---------------------------------------------------------------------------
static lv_obj_t *panel_create(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(COLOR_PANEL_EDGE), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static void battery_label_update(lv_obj_t *label) {
    int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(label, "");
    } else {
        if (soc > 100) soc = 100;
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", soc);
        lv_label_set_text(label, buf);
    }
}

// 底部提示随移动方向模式切换,箭头指示当前上/下键的实际走向;
// 动作描述统一按物理键位(上/中/下键)表述。
static void hint_update(void) {
    lv_label_set_text(s_hint_label, s_horizontal
                     ? "方向←→  短按下键切换方向"
                     : "方向↑↓  短按下键切换方向");
}

// ---------------------------------------------------------------------------
// 首次启动的操作指南页:确认一次后写入 NVS,之后不再显示
// ---------------------------------------------------------------------------
static bool guide_seen(void) {
    uint32_t seen = 0;
    if (bsp_storage_get_u32(KEY_GUIDE, &seen) != ESP_OK) return false;
    return seen != 0;
}

static void guide_mark_seen(void) {
    if (bsp_storage_set_u32(KEY_GUIDE, 1) != ESP_OK) {
        ESP_LOGW(TAG, "指南标记写入失败,下次启动会再显示");
    }
}

static void guide_screen_build(void) {
    s_guide_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_guide_scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_guide_scr, LV_OPA_COVER, 0);

    lv_obj_t *title = label_create(s_guide_scr, &s_font_20, COLOR_TEXT,
                                   LV_ALIGN_TOP_MID, 0, 44);
    lv_label_set_text(title, "操作指南");

    static const char *const LINES[] = {
        "短按上/中键 移动光标",
        "短按下键 切换方向",
        "长按上键 挖雷",
        "长按中键 暂停",
        "长按下键 插旗",
    };
    for (uint8_t i = 0; i < 5; i++) {
        lv_obj_t *line = label_create(s_guide_scr, &s_font_14, COLOR_TEXT,
                                      LV_ALIGN_TOP_LEFT, 42, 92 + i * 26);
        lv_label_set_text(line, LINES[i]);
    }

    lv_obj_t *pill = panel_create(s_guide_scr, 55, 258, 130, 30);
    lv_obj_set_style_bg_color(pill, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_set_style_radius(pill, 15, 0);
    lv_obj_t *pill_label = lv_label_create(pill);
    lv_obj_set_style_text_font(pill_label, &s_font_14, 0);
    lv_obj_set_style_text_color(pill_label, lv_color_hex(COLOR_BG), 0);
    lv_label_set_text(pill_label, "短按下键 开始");
    lv_obj_center(pill_label);
}

// ---------------------------------------------------------------------------
// 暂停菜单:继续 / 重新开始 / 返回主页
// ---------------------------------------------------------------------------
#define PAUSE_ITEM_COUNT 3
static const char *const PAUSE_ITEMS[PAUSE_ITEM_COUNT] = {
    "继续", "重新开始", "返回主页",
};

static void pause_menu_update(void) {
    for (uint8_t i = 0; i < PAUSE_ITEM_COUNT; i++) {
        lv_obj_set_style_text_color(s_pause_items[i],
                                    lv_color_hex(i == s_pause_sel ? COLOR_ACCENT : COLOR_TEXT),
                                    0);
    }
}

static void pause_enter(void) {
    s_paused = true;
    s_pause_sel = 0;
    s_pause_start_us = esp_timer_get_time();
    pause_menu_update();
    lv_obj_remove_flag(s_pause_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_pause_overlay);
}

static void pause_resume(void) {
    s_paused = false;
    lv_obj_add_flag(s_pause_overlay, LV_OBJ_FLAG_HIDDEN);
    // 把暂停时长补回计时起点,暂停期间不计时;未布雷时计时尚未开始。
    if (s_game.mines_placed) {
        s_start_us += esp_timer_get_time() - s_pause_start_us;
    }
}

static void title_screen_build(void) {
    s_title_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_title_scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_title_scr, LV_OPA_COVER, 0);

    lv_obj_t *title = label_create(s_title_scr, &s_font_20, COLOR_TEXT,
                                   LV_ALIGN_TOP_MID, 0, 64);
    lv_label_set_text(title, "扫雷");
    lv_obj_t *sub = label_create(s_title_scr, &s_font_14, COLOR_DIM,
                                 LV_ALIGN_TOP_MID, 0, 96);
    lv_label_set_text(sub, "9x9 / 10 颗雷");

    lv_obj_t *panel = panel_create(s_title_scr, 30, 136, 180, 104);
    s_best_label = label_create(panel, &s_font_20, COLOR_ACCENT,
                                LV_ALIGN_TOP_LEFT, 16, 14);
    s_wins_label = label_create(panel, &s_font_14, COLOR_TEXT,
                                LV_ALIGN_TOP_LEFT, 16, 48);
    s_games_label = label_create(panel, &s_font_14, COLOR_TEXT,
                                 LV_ALIGN_TOP_LEFT, 16, 72);
    refresh_stats_labels();

    lv_obj_t *hint = panel_create(s_title_scr, 40, 258, 160, 30);
    lv_obj_set_style_bg_color(hint, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(hint, 0, 0);
    lv_obj_set_style_radius(hint, 15, 0);
    lv_obj_t *hint_label = lv_label_create(hint);
    lv_obj_set_style_text_font(hint_label, &s_font_14, 0);
    lv_obj_set_style_text_color(hint_label, lv_color_hex(COLOR_BG), 0);
    lv_label_set_text(hint_label, "开始游戏");
    lv_obj_center(hint_label);

    s_batt_title_label = label_create(s_title_scr, &s_font_14, COLOR_DIM,
                                      LV_ALIGN_TOP_RIGHT, -14, 12);
    battery_label_update(s_batt_title_label);
}

static void game_screen_build(void) {
    s_game_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_game_scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_game_scr, LV_OPA_COVER, 0);

    s_mines_label = label_create(s_game_scr, &s_font_20, COLOR_ACCENT,
                                 LV_ALIGN_TOP_LEFT, 16, HDR_Y);
    s_face_label = label_create(s_game_scr, &s_font_20, COLOR_TEXT,
                                LV_ALIGN_TOP_MID, 0, HDR_Y);
    s_timer_label = label_create(s_game_scr, &s_font_20, COLOR_TEXT,
                                 LV_ALIGN_TOP_RIGHT, -16, HDR_Y);
    s_batt_game_label = label_create(s_game_scr, &s_font_14, COLOR_DIM,
                                     LV_ALIGN_TOP_RIGHT, -14, 10);
    s_hint_label = label_create(s_game_scr, &s_font_14, COLOR_DIM,
                                LV_ALIGN_TOP_MID, 0, FOOT_Y);
    hint_update();
    s_hint2_label = label_create(s_game_scr, &s_font_14, COLOR_DIM,
                                 LV_ALIGN_TOP_MID, 0, FOOT2_Y);
    lv_label_set_text(s_hint2_label, "长按:上键挖雷 中键暂停 下键插旗");

    lv_obj_t *board_bg = panel_create(s_game_scr, BOARD_X - 3, BOARD_Y - 3,
                                      BOARD_PX + 6, BOARD_PX + 6);
    lv_obj_set_style_radius(board_bg, 6, 0);

    s_board_canvas = lv_canvas_create(s_game_scr);
    lv_canvas_set_draw_buf(s_board_canvas, &s_board_buf);
    lv_obj_set_pos(s_board_canvas, BOARD_X, BOARD_Y);
    lv_obj_set_size(s_board_canvas, BOARD_PX, BOARD_PX);
    for (uint8_t i = 0; i < 16; i++) {
        lv_canvas_set_palette(s_board_canvas, i,
                              lv_color_to_32(lv_color_hex(PALETTE[i]), LV_OPA_COVER));
    }
    lv_canvas_fill_bg(s_board_canvas, lv_color_hex(PALETTE[PAL_BG]), LV_OPA_COVER);

    // 结算浮层:先建后藏,胜负确定时弹出。
    s_overlay = panel_create(s_game_scr, 20, 110, 200, 100);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(COLOR_PANEL), 0);
    s_overlay_title = label_create(s_overlay, &s_font_20, COLOR_ACCENT,
                                   LV_ALIGN_TOP_MID, 0, 12);
    s_overlay_line = label_create(s_overlay, &s_font_14, COLOR_TEXT,
                                  LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_t *ov_hint = label_create(s_overlay, &s_font_14, COLOR_DIM,
                                     LV_ALIGN_TOP_MID, 0, 70);
    lv_label_set_text(ov_hint, "单击再来  长按回标题");
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    // 暂停浮层:先建后藏,长按中间键(DOWN)时弹出。
    s_pause_overlay = panel_create(s_game_scr, 20, 96, 200, 128);
    lv_obj_t *pause_title = label_create(s_pause_overlay, &s_font_20, COLOR_ACCENT,
                                         LV_ALIGN_TOP_MID, 0, 10);
    lv_label_set_text(pause_title, "已暂停");
    for (uint8_t i = 0; i < PAUSE_ITEM_COUNT; i++) {
        s_pause_items[i] = label_create(s_pause_overlay, &s_font_14, COLOR_TEXT,
                                        LV_ALIGN_TOP_MID, 0, 46 + i * 24);
        lv_label_set_text(s_pause_items[i], PAUSE_ITEMS[i]);
    }
    lv_obj_add_flag(s_pause_overlay, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// 游戏流程
// ---------------------------------------------------------------------------
static void header_refresh(void) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%02u", (unsigned)msw_flags_left(&s_game));
    lv_label_set_text(s_mines_label, buf);
    const char *face = ":)";
    if (s_game.state == MSW_WON) face = ":D";
    if (s_game.state == MSW_LOST) face = "X(";
    lv_label_set_text(s_face_label, face);
    snprintf(buf, sizeof(buf), "%02u:%02u",
             (unsigned)(s_elapsed_s / 60), (unsigned)(s_elapsed_s % 60));
    lv_label_set_text(s_timer_label, buf);
}

static void overlay_show(void) {
    char buf[48];
    lv_label_set_text(s_overlay_title, s_game.state == MSW_WON ? "你赢了!" : "踩雷了!");
    if (s_game.state == MSW_WON && s_stats.best_s == s_elapsed_s && s_elapsed_s > 0) {
        snprintf(buf, sizeof(buf), "用时 %02u:%02u  新纪录!",
                 (unsigned)(s_elapsed_s / 60), (unsigned)(s_elapsed_s % 60));
    } else if (s_stats.best_s > 0) {
        snprintf(buf, sizeof(buf), "用时 %02u:%02u  最佳 %02u:%02u",
                 (unsigned)(s_elapsed_s / 60), (unsigned)(s_elapsed_s % 60),
                 (unsigned)(s_stats.best_s / 60), (unsigned)(s_stats.best_s % 60));
    } else {
        snprintf(buf, sizeof(buf), "用时 %02u:%02u",
                 (unsigned)(s_elapsed_s / 60), (unsigned)(s_elapsed_s % 60));
    }
    lv_label_set_text(s_overlay_line, buf);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
}

static void game_start(void) {
    msw_init(&s_game, esp_random());
    s_cx = MSW_BOARD_W / 2;
    s_cy = MSW_BOARD_H / 2;
    s_horizontal = false;         // 每局从纵向移动开始
    s_boom_x = -1;
    s_boom_y = -1;
    s_elapsed_s = 0;
    s_start_us = 0;
    s_end_recorded = false;
    s_paused = false;
    s_step_armed[BSP_BTN_UP] = false;
    s_step_armed[BSP_BTN_DOWN] = false;
    s_board_dirty = true;
    if (s_screen != SCR_GAME) {
        s_screen = SCR_GAME;
        lv_screen_load(s_game_scr);
    }
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pause_overlay, LV_OBJ_FLAG_HIDDEN);
    hint_update();
    header_refresh();
    battery_label_update(s_batt_game_label);
}

static void back_to_title(void) {
    s_screen = SCR_TITLE;
    s_paused = false;
    lv_obj_add_flag(s_pause_overlay, LV_OBJ_FLAG_HIDDEN);
    refresh_stats_labels();
    battery_label_update(s_batt_title_label);
    lv_screen_load(s_title_scr);
}

// 光标移动(dx/dy 取 -1/0/1),环绕棋盘。
static void cursor_move(int dx, int dy) {
    s_cx = (uint8_t)((s_cx + MSW_BOARD_W + dx) % MSW_BOARD_W);
    s_cy = (uint8_t)((s_cy + MSW_BOARD_H + dy) % MSW_BOARD_H);
    s_board_dirty = true;
}

// ---------------------------------------------------------------------------
// 泵定时器(LVGL 任务):统一处理输入、计时、渲染
// ---------------------------------------------------------------------------

// 上/下键在当前方向模式下走一步。
static void cursor_step(bsp_btn_t btn) {
    if (btn == BSP_BTN_UP) {
        if (s_horizontal) cursor_move(-1, 0); else cursor_move(0, -1);
    } else {
        if (s_horizontal) cursor_move(1, 0); else cursor_move(0, 1);
    }
}

static void pump(void) {
    input_event_t ev;
    while (xQueueReceive(s_input_queue, &ev, 0) == pdTRUE) {
        // 指南页:短按下键(OK)确认并写入 NVS,之后进入标题页。
        if (s_screen == SCR_GUIDE) {
            if (ev.ev == BSP_BTN_CLICK && ev.btn == BSP_BTN_OK) {
                guide_mark_seen();
                back_to_title();
            }
            continue;
        }

        if (s_screen == SCR_TITLE) {
            if (ev.ev == BSP_BTN_CLICK && ev.btn == BSP_BTN_OK) game_start();
            continue;
        }

        // 游戏页/结算态
        if (s_game.state == MSW_WON || s_game.state == MSW_LOST) {
            if (ev.btn == BSP_BTN_OK && ev.ev == BSP_BTN_CLICK) {
                game_start();
            } else if (ev.btn == BSP_BTN_OK && ev.ev == BSP_BTN_LONG) {
                back_to_title();
            }
            continue;
        }

        // 抬起:若本次按下未被长按消费,走一步。移动放在抬起而不是按下,
        // 长按挖雷/暂停才不会先走一格再退回(退格);连按节奏不受影响。
        if (ev.ev == BSP_BTN_RELEASE) {
            if (s_step_armed[ev.btn]) {
                s_step_armed[ev.btn] = false;
                cursor_step(ev.btn);
            }
            continue;
        }

        // 暂停菜单:UP/DOWN 按下即移动选择(连按连走),OK 单击确认,
        // 再长按 DOWN 直接继续;其余事件(含挖雷/插旗)一律忽略。
        if (s_paused) {
            if (ev.ev == BSP_BTN_PRESS &&
                (ev.btn == BSP_BTN_UP || ev.btn == BSP_BTN_DOWN)) {
                const int delta = (ev.btn == BSP_BTN_UP) ? -1 : 1;
                s_pause_sel = (uint8_t)((s_pause_sel + PAUSE_ITEM_COUNT + delta) %
                                        PAUSE_ITEM_COUNT);
                pause_menu_update();
            } else if (ev.ev == BSP_BTN_LONG && ev.btn == BSP_BTN_DOWN) {
                pause_resume();
            } else if (ev.ev == BSP_BTN_CLICK && ev.btn == BSP_BTN_OK) {
                switch (s_pause_sel) {
                case 0:  pause_resume();  break;
                case 1:  game_start();    break;
                default: back_to_title(); break;
                }
            }
            continue;
        }

        // UP/DOWN:按下先武装,抬起时走一步,连按连走。不能用单击/双击事件:
        // 它们要等按键组件分类完毕(抬起后约 300ms)才上报,而且快速连按
        // 3 次以上会被归为多次点击、不产生任何单击/双击事件。
        if (ev.ev == BSP_BTN_PRESS &&
            (ev.btn == BSP_BTN_UP || ev.btn == BSP_BTN_DOWN)) {
            s_step_armed[ev.btn] = true;
            continue;
        }

        // 长按消费本次按下(抬起不再走步):长按中间键(DOWN)= 暂停菜单;
        // 长按 UP = 挖开当前格。
        if (ev.ev == BSP_BTN_LONG && (ev.btn == BSP_BTN_UP || ev.btn == BSP_BTN_DOWN)) {
            s_step_armed[ev.btn] = false;
            if (ev.btn == BSP_BTN_DOWN) {
                pause_enter();
                continue;
            }
            const bool first = !s_game.mines_placed;
            const msw_state_t before = s_game.state;
            if (msw_reveal(&s_game, s_cx, s_cy) && first) {
                s_start_us = esp_timer_get_time();
            }
            if (before != MSW_LOST && s_game.state == MSW_LOST) {
                s_boom_x = (int8_t)s_cx;  // 终局高亮被踩中的雷
                s_boom_y = (int8_t)s_cy;
            }
            s_board_dirty = true;
            continue;
        }

        // OK:单击切方向,长按插旗/拔旗;其余事件无动作。
        if (ev.btn == BSP_BTN_OK) {
            if (ev.ev == BSP_BTN_CLICK) {
                s_horizontal = !s_horizontal;
                hint_update();
            } else if (ev.ev == BSP_BTN_LONG) {
                if (msw_toggle_flag(&s_game, s_cx, s_cy)) s_board_dirty = true;
                header_refresh();
            }
            continue;
        }
    }

    // 计时(250ms 节流):开局后、终局前;暂停期间冻结
    const uint32_t tick = now_ms();
    if (s_screen == SCR_GAME && s_game.mines_placed && !s_paused &&
        (s_game.state == MSW_PLAYING || s_game.state == MSW_READY)) {
        s_elapsed_s = (uint32_t)((esp_timer_get_time() - s_start_us) / 1000000LL);
    }
    if (tick - s_last_tick_ms >= 250) {
        s_last_tick_ms = tick;
        if (s_screen == SCR_GAME) {
            header_refresh();
            const int soc = bsp_battery_soc();
            if (soc != s_last_soc) {
                s_last_soc = soc;
                battery_label_update(s_batt_game_label);
            }
        }
    }

    // 终局一次性结算:记录战绩、存 NVS、弹浮层
    if (s_screen == SCR_GAME && !s_end_recorded &&
        (s_game.state == MSW_WON || s_game.state == MSW_LOST)) {
        s_end_recorded = true;
        if (s_game.mines_placed) {
            s_elapsed_s = (uint32_t)((esp_timer_get_time() - s_start_us) / 1000000LL);
        }
        msw_stats_record(&s_stats, s_game.state == MSW_WON, s_elapsed_s);
        stats_save();
        header_refresh();
        overlay_show();
    }

    if (s_board_dirty && s_screen == SCR_GAME) {
        s_board_dirty = false;
        render_board();
    }
}

static void pump_timer_cb(lv_timer_t *timer) {
    (void)timer;
    pump();
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
esp_err_t app_minesweeper_start(void) {
    s_input_queue = xQueueCreate(INPUT_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;

    // LV_DRAW_BUF_DEFINE_STATIC 不填 handlers,必须显式 INIT,否则
    // lv_canvas_set_draw_buf 会静默拒绝并在首次取回缓冲区时崩溃(真机复现)。
    app_fonts_init();
    LV_DRAW_BUF_INIT_STATIC(s_board_buf);

    stats_load();
    title_screen_build();
    game_screen_build();
    guide_screen_build();
    if (guide_seen()) {
        lv_screen_load(s_title_scr);
        s_screen = SCR_TITLE;
    } else {
        lv_screen_load(s_guide_scr);
        s_screen = SCR_GUIDE;
    }

    s_pump_timer = lv_timer_create(pump_timer_cb, 20, NULL);
    if (!s_pump_timer) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_minesweeper_on_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (!s_input_queue) return;
    const input_event_t event = { .btn = btn, .ev = ev };
    (void)xQueueSend(s_input_queue, &event, 0);
}
