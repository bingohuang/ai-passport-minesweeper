// components/bsp/include/bsp_storage.h
// NVS 键值存储封装:战绩、配置等掉电不丢失的小数据。初始化幂等;
// 失败只上报错误,绝不擦除分区(分区里可能已有用户数据)。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// 初始化 NVS(幂等)。返回 ESP_OK 表示可用;此后读写接口在本命名空间上工作。
// 不自动 erase,重复调用返回首次结果。
esp_err_t bsp_storage_init(const char *namespace);

// 读 u32。键不存在时 *out 置 0 并返回 ESP_OK(视为"无记录");
// 读失败返回错误码。
esp_err_t bsp_storage_get_u32(const char *key, uint32_t *out);

// 写 u32 并提交。失败返回错误码,调用方下次重试或放弃。
esp_err_t bsp_storage_set_u32(const char *key, uint32_t value);
