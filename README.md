<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Minesweeper for FoloToy AI Passport

A minesweeper game for the FoloToy AI Passport (ESP32-C3, 240×320 display with
rounded corners, three buttons sharing one ADC pin). This branch carries the
application on top of upstream `FoloToy/ai-passport`; the hardware drivers in
`components/bsp` are unchanged except for a small NVS storage helper.

## Game

Classic beginner board: 9×9 with 10 mines. Mines are placed after your first
reveal, and that cell plus its 3×3 neighborhood are guaranteed mine-free. Reveal
all safe cells to win; hit a mine and the board shows every unflagged mine.

## Controls

| Button | Click | Long press (hold to repeat) |
| --- | --- | --- |
| UP | Move cursor up | Move cursor left |
| DOWN | Move cursor down | Move cursor right |
| OK | Reveal | Flag / unflag |

The cursor wraps around the board edges. On the title screen, OK starts a game.
On the end overlay, OK starts a new game and a long press on OK returns to the
title screen.

## Records

Best win time, win count, and total games are stored in NVS and survive power
loss. They are shown on the title screen and the end overlay.

## Layout

- `main/minesweeper_model.c/h` — pure game logic (board, mine placement, flood
  reveal, flags, win/loss, stats). Host-tested by `tests/test_minesweeper_model.c`.
- `main/app_minesweeper.c/h` — LVGL screens, board rendering, key mapping, and
  the NVS glue. All model access and rendering run in the LVGL task.
- `main/main.c` — application entry: peripheral init, key dispatch, key-wake guard.
- `components/bsp/src/bsp_storage.c` — thin NVS wrapper (`bsp_storage.h`).

## Build and test

```bash
./tools/validate.sh --static    # repository checks + host tests
./tools/validate.sh --firmware  # ESP-IDF 5.5.3 build + merged-image verification
./tools/validate.sh             # complete gate
```
