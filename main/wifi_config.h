/**
 * @file    wifi_config.h
 * @brief   WiFi STA 配置 & AP 网页配网
 *
 * 连接策略:
 *   1. 从 NVS 读取上次保存的 SSID/密码
 *   2. 有凭据 → 尝试 STA 连接
 *   3. 无凭据或连接失败 → 启动 AP 热点 + HTTP 配网页
 *   4. 用户通过 192.168.4.1 提交新凭据 → 保存 NVS → 重启
 */

#ifndef __WIFI_CONFIG_H__
#define __WIFI_CONFIG_H__

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/* ── 默认凭据 (NVS 无记录时回退) ── */
#define DEFAULT_SSID    ""
#define DEFAULT_PWD     ""

/* ── AP 配网参数 ── */
#define AP_SSID         "XRay_Config"
#define AP_MAX_CONNECT  4
#define MDNS_HOSTNAME   "xray-io"       /* PC 用 xray-io.local 连接 */

/* ── NVS 命名空间 & 键 ── */
#define NVS_NAMESPACE   "wifi"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PWD     "pwd"
#define NVS_KEY_IP      "ip4"       /* IP 末位, -1 = 用 DHCP */

/* ── 重试参数 ── */
#define FW_VERSION      "1.0.0"  /* 固件版本，开机包上报 */
#define WIFI_RETRY_MAX  10
#define AP_RETRY_COUNT   3           /* STA 连不上重试 3 次就开 AP */

/* ── WiFi 连接状态 ── */
typedef struct {
    int connected;
    char ip_str[16];
} wifi_state_t;

extern wifi_state_t g_wifi_state;

/* ── 函数声明 ── */

/**
 * @brief   初始化 WiFi（自动 NVS 读取 / AP 回退 / HTTP 配网）
 * @retval  0 = STA 连接成功，非 0 = 进入 AP 配网模式
 *          （AP 模式下不会返回，配网成功后调用 esp_restart）
 */
int wifi_init(void);

/**
 * @brief   发送开机包 (CMD_BOOT_INFO)
 *          内容: 固件版本,设备MAC,IP地址 (逗号分隔)
 * @param   sock : TCP 客户端 socket
 */
void send_boot_packet(int sock);

#endif /* __WIFI_CONFIG_H__ */
