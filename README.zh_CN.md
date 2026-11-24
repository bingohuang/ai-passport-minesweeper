<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# FoloToy AI Passport 扫雷游戏

面向 FoloToy AI Passport(ESP32-C3、240×320 圆角屏、三个按键共用一根 ADC
引脚)的扫雷游戏。本分支在上游 `FoloToy/ai-passport` 之上承载该应用;
`components/bsp` 中的硬件驱动除一个小的 NVS 存储封装外保持不变。

## 玩法

经典初级棋盘:9×9,10 颗雷。首次揭示后才布雷,且该格及其 3×3 邻居一定
无雷。揭完所有安全格即获胜;踩雷则棋盘亮出所有未标记的雷。

## 操作

| 按键 | 单击 | 双击 | 长按 |
| --- | --- | --- | --- |
| UP | 光标上移(横向模式下左移) | — | — |
| DOWN | 光标下移(横向模式下右移) | — | — |
| OK | 切换移动方向(上下 ↔ 左右) | 确认扫雷(揭示) | 插旗/拔旗 |

光标在棋盘边缘环绕,底部提示会显示当前方向模式。标题页单击 OK 开始游戏;
结算浮层上单击 OK 再来一局,长按 OK 返回标题页。

## 界面语言

界面全部为简体中文。文字使用应用子集字体（`assets/fonts/app_font_cjk_14.c`、
`app_font_cjk_20.c`），由 Adobe 思源黑体 SC 经 `lv_font_conv` 1.5.3 生成，
覆盖 `main/` 中全部非 ASCII 字面值与可打印 ASCII;Montserrat 仅作为 LVGL
符号的 fallback。字形覆盖由 `tools/check_glyphs.py` 静态门禁校验。

## 战绩

最佳用时、胜场、总场次保存在 NVS 中,掉电不丢失;标题页与结算浮层都会显示。

## 代码结构

- `main/minesweeper_model.c/h` —— 纯游戏逻辑(棋盘、布雷、泛洪揭示、标记、
  胜负、战绩),由 `tests/test_minesweeper_model.c` 在主机上测试。
- `main/app_minesweeper.c/h` —— LVGL 页面、棋盘渲染、按键映射、NVS 胶水。
  模型访问与渲染统一在 LVGL 任务内完成。
- `main/main.c` —— 应用入口:外设初始化、按键分发、按键唤醒守卫。
- `components/bsp/src/bsp_storage.c` —— NVS 薄封装(`bsp_storage.h`)。

## 构建与测试

```bash
./tools/validate.sh --static    # 仓库检查 + 主机测试
./tools/validate.sh --firmware  # ESP-IDF 5.5.3 构建 + 合并镜像校验
./tools/validate.sh             # 完整门禁
```
