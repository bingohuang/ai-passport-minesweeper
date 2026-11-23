// components/bsp/src/bsp_storage.c —— NVS 封装实现(见 bsp_storage.h)。
#include "bsp_storage.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "bsp_storage";

static bool s_nvs_ready;
static char s_namespace[NVS_KEY_NAME_MAX_SIZE] = {0};

esp_err_t bsp_storage_init(const char *namespace)
{
    if (s_nvs_ready) {
        // 命名空间固定为本组件打开时的值;换名调用视为误用,保持首次语义。
        return ESP_OK;
    }
    if (!namespace || namespace[0] == '\0' || strlen(namespace) >= NVS_KEY_NAME_MAX_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 与 demo_radio 同策略:分区异常只报错,绝不替用户决定擦除。
        ESP_LOGE(TAG, "NVS 初始化失败: %s;未自动擦除分区", esp_err_to_name(err));
        return err;
    }
    if (err != ESP_OK) return err;

    strncpy(s_namespace, namespace, sizeof(s_namespace) - 1);
    s_nvs_ready = true;
    return ESP_OK;
}

static esp_err_t s_open(nvs_handle_t *out)
{
    if (!s_nvs_ready) return ESP_ERR_INVALID_STATE;
    return nvs_open(s_namespace, NVS_READWRITE, out);
}

esp_err_t bsp_storage_get_u32(const char *key, uint32_t *out)
{
    if (!key || !out) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = s_open(&h);
    if (err != ESP_OK) return err;
    err = nvs_get_u32(h, key, out);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = 0;  // 无记录视为 0,与 msw_stats_t 的初始态一致
        err = ESP_OK;
    }
    nvs_close(h);
    return err;
}

esp_err_t bsp_storage_set_u32(const char *key, uint32_t value)
{
    if (!key) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = s_open(&h);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "写 %s 失败: %s", key, esp_err_to_name(err));
    }
    nvs_close(h);
    return err;
}
