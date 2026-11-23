idf_component_register(
    SRCS "main.c"
         "minesweeper_model.c"
         "app_minesweeper.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_hw_support esp_timer freertos nvs_flash
)
