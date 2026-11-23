idf_component_register(
    SRCS "main.c"
         "minesweeper_model.c"
         "app_minesweeper.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_hw_support esp_timer freertos nvs_flash
)

# 应用子集字体(思源黑体 SC,见 assets/README.md;生成命令记录在字体源文件头)
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_cjk_14.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_cjk_20.c"
)
