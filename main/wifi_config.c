/**
 * @file    wifi_config.c
 * @brief   WiFi STA 连接 + AP 网页配网
 *
 * 启动逻辑:
 *   - NVS 有凭据 → STA 连接 → 成功返回
 *   - NVS 无凭据/连接失败 → 开热点 XRay_Config → HTTP 配网 → 保存重启
 */

#include <stdio.h>
#include <string.h>
#include "wifi_config.h"
#include "lwip/sockets.h"
#include "esp_mac.h"

static void mdns_start(uint32_t ip);

static const char *TAG = "wifi";

static EventGroupHandle_t wifi_event;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* 全局状态 */
wifi_state_t g_wifi_state = {0};

/* ── NVS 中存储的凭据 ── */
static char g_ssid[33] = {0};
static char g_pwd[65]  = {0};
static int32_t g_ip_octet = -1;         /* -1 = DHCP, 0~254 = 静态 IP 末位 */

/* ═══════════════════════════════════════════════════
 *  NVS 读写
 * ═══════════════════════════════════════════════════ */

static int load_creds_from_nvs(void)
{
    nvs_handle_t handle;
    size_t len;

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 无 WiFi 凭据");
        return -1;
    }

    len = sizeof(g_ssid);
    err = nvs_get_str(handle, NVS_KEY_SSID, g_ssid, &len);
    if (err != ESP_OK) { nvs_close(handle); return -1; }

    len = sizeof(g_pwd);
    err = nvs_get_str(handle, NVS_KEY_PWD, g_pwd, &len);
    if (err != ESP_OK) { nvs_close(handle); return -1; }

    g_ip_octet = -1;
    nvs_get_i32(handle, NVS_KEY_IP, &g_ip_octet);

    nvs_close(handle);
    ESP_LOGI(TAG, "从 NVS 读取: SSID=%s IP末位=%ld", g_ssid, (long)g_ip_octet);
    return 0;
}

static void save_creds_to_nvs(const char *ssid, const char *pwd, int32_t ip_octet)
{
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle));
    nvs_set_str(handle, NVS_KEY_SSID, ssid);
    nvs_set_str(handle, NVS_KEY_PWD,  pwd);
    nvs_set_i32(handle, NVS_KEY_IP,   ip_octet);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "凭据已保存: SSID=%s IP末位=%ld", ssid, (long)ip_octet);
}

/* ═══════════════════════════════════════════════════
 *  WiFi 事件回调
 * ═══════════════════════════════════════════════════ */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    static int retry = 0;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_state.connected = 0;

        if (retry < WIFI_RETRY_MAX) {
            esp_wifi_connect();
            retry++;
            ESP_LOGI(TAG, "重连中... (%d/%d)", retry, WIFI_RETRY_MAX);
        } else {
            xEventGroupSetBits(wifi_event, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "WiFi 连接失败，已超过最大重试次数");
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        snprintf(g_wifi_state.ip_str, sizeof(g_wifi_state.ip_str),
                 IPSTR, IP2STR(&ev->ip_info.ip));
        g_wifi_state.connected = 1;
        retry = 0;
        xEventGroupSetBits(wifi_event, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "已获取 IP: %s", g_wifi_state.ip_str);
        uint32_t ip = ev->ip_info.ip.addr;
        mdns_start(ip);

        /* 配置了静态 IP 末位 → 改用固定 IP（仅执行一次） */
        static int ip_set_done = 0;
        if (!ip_set_done && g_ip_octet >= 2 && g_ip_octet <= 250) {
            ip_set_done = 1;
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif) {
                esp_netif_ip_info_t info;
                info.ip.addr = (ev->ip_info.ip.addr & 0x00FFFFFF) | ((uint32_t)g_ip_octet << 24);
                info.netmask.addr = ev->ip_info.netmask.addr;
                info.gw.addr = ev->ip_info.gw.addr;
                esp_netif_dhcpc_stop(netif);
                esp_netif_set_ip_info(netif, &info);
                snprintf(g_wifi_state.ip_str, sizeof(g_wifi_state.ip_str),
                         IPSTR, IP2STR(&info.ip));
                ESP_LOGI(TAG, "静态 IP: %s", g_wifi_state.ip_str);
            }
        }
        ESP_LOGI(TAG, "主机名: %s.local", MDNS_HOSTNAME);
    }
}

/* ═══════════════════════════════════════════════════
 *  STA 连接
 * ═══════════════════════════════════════════════════ */

static int wifi_connect_sta(const char *ssid, const char *pwd)
{
    wifi_event = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               &wifi_event_handler, NULL);

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = "",
            .password = "",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_cfg.sta.ssid, ssid, 32);
    strncpy((char *)wifi_cfg.sta.password, pwd, 64);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg);
    esp_wifi_start();

    ESP_LOGI(TAG, "正在连接 %s ...", ssid);

    EventBits_t bits = xEventGroupWaitBits(wifi_event,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi 连接成功! IP: %s", g_wifi_state.ip_str);
        return 0;
    } else {
        ESP_LOGE(TAG, "WiFi 连接失败");
        return -1;
    }
}

/* ═══════════════════════════════════════════════════
 *  AP 模式 + HTTP 配网服务器
 * ═══════════════════════════════════════════════════ */

static const char *g_config_page =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>XRay WiFi 配置</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font:16px/1.5 system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;"
    "display:flex;justify-content:center;align-items:center;min-height:100vh}"
    ".card{background:#16213e;padding:32px 24px;border-radius:12px;width:100%;max-width:360px;"
    "box-shadow:0 4px 24px rgba(0,0,0,.4)}"
    "h1{font-size:22px;text-align:center;margin-bottom:24px;color:#0f3460}"
    "h1 span{color:#e94560}"
    "label{display:block;margin-bottom:6px;font-size:13px;color:#8892b0}"
    "input{width:100%;padding:10px 12px;border:1px solid #0f3460;border-radius:8px;"
    "background:#1a1a2e;color:#e0e0e0;font-size:15px;margin-bottom:16px;outline:none}"
    "input:focus{border-color:#e94560}"
    "button{width:100%;padding:12px;background:#e94560;color:#fff;border:none;border-radius:8px;"
    "font-size:16px;cursor:pointer;font-weight:600}"
    "button:hover{background:#d63850}"
    ".tip{text-align:center;font-size:12px;color:#8892b0;margin-top:16px}"
    "</style></head><body>"
    "<div class='card'>"
    "<h1><span>XRay</span> WiFi Config</h1>"
    "<form method='POST' action='/'>"
    "<label>WiFi SSID</label>"
    "<input name='ssid' placeholder='请输入医院 WiFi 名称' required>"
    "<label>WiFi Password</label>"
    "<input name='pwd' type='password' placeholder='请输入 WiFi 密码'>"
    "<label>固定 IP (可选, 留空用 DHCP)</label>"
    "<input name='ip4' type='number' min='2' max='250' placeholder='如 200, 留空则自动获取'>"
    "<button type='submit'>保存并重启</button>"
    "</form>"
    "<div class='tip'>配置完成后设备将自动重启并连接</div>"
    "</div></body></html>";

static const char *g_saved_page =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='5;url=/'>"
    "<title>XRay - 已保存</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font:16px/1.5 system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;"
    "display:flex;justify-content:center;align-items:center;min-height:100vh}"
    ".card{background:#16213e;padding:32px 24px;border-radius:12px;width:100%;max-width:360px;"
    "box-shadow:0 4px 24px rgba(0,0,0,.4);text-align:center}"
    "h1{font-size:22px;color:#4ecca3;margin-bottom:12px}"
    "p{color:#8892b0;font-size:14px}"
    "</style></head><body>"
    "<div class='card'>"
    "<h1>Saved</h1>"
    "<p>WiFi Setup Saved. Restarting and connecting...</p>"
    "</div></body></html>";

/* URL 解码 */
static void url_decode(char *dst, const char *src)
{
    char a, b;
    while (*src) {
        if (*src == '%' && ((a = src[1]) && (b = src[2]))
            && ((a >= '0' && a <= '9') || (a >= 'A' && a <= 'F') || (a >= 'a' && a <= 'f'))
            && ((b >= '0' && b <= '9') || (b >= 'A' && b <= 'F') || (b >= 'a' && b <= 'f'))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= 'A' - 10; else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= 'A' - 10; else b -= '0';
            *dst++ = (char)(16 * a + b);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* 简易 HTTP 解析与页面服务 */
static void handle_http_client(int sock)
{
    char buf[2048];
    int len = recv(sock, buf, sizeof(buf) - 1, 0);
    if (len <= 0) { closesocket(sock); return; }
    buf[len] = '\0';

    /* POST 请求 → 解析表单 → 保存凭据 → 重启 */
    if (strncmp(buf, "POST", 4) == 0) {
        char ssid[33] = {0}, pwd[65] = {0};
        int32_t ip4 = -1;

        char *ssid_start = strstr(buf, "ssid=");
        char *pwd_start  = strstr(buf, "&pwd=");
        char *ip4_start  = strstr(buf, "&ip4=");

        if (ssid_start) {
            ssid_start += 5;
            char *end = strchr(ssid_start, '&');
            int slen = end ? (int)(end - ssid_start) : (int)strlen(ssid_start);
            if (slen > 32) slen = 32;
            memcpy(ssid, ssid_start, slen);
            url_decode(ssid, ssid);
        }
        if (pwd_start) {
            pwd_start += 5;
            char *end = strchr(pwd_start, '&');
            if (!end) { end = strchr(pwd_start, ' '); if (!end) end = strchr(pwd_start, '\r'); }
            int plen = end ? (int)(end - pwd_start) : (int)strlen(pwd_start);
            if (plen > 64) plen = 64;
            memcpy(pwd, pwd_start, plen);
            url_decode(pwd, pwd);
        }
        if (ip4_start) {
            ip4_start += 5;
            char *end = strchr(ip4_start, '&');
            if (!end) { end = strchr(ip4_start, ' '); if (!end) end = strchr(ip4_start, '\r'); }
            int ilen = end ? (int)(end - ip4_start) : (int)strlen(ip4_start);
            if (ilen > 0 && ilen <= 3) {
                char tmp[4] = {0};
                memcpy(tmp, ip4_start, ilen);
                ip4 = atoi(tmp);
                if (ip4 < 2 || ip4 > 250) ip4 = -1;  /* 非法值回退 DHCP */
            }
        }

        ESP_LOGI(TAG, "收到配网请求: SSID=%s IP=%ld", ssid, (long)ip4);

        if (strlen(ssid) > 0) {
            save_creds_to_nvs(ssid, strlen(pwd) ? pwd : "", ip4);
            send(sock, g_saved_page, strlen(g_saved_page), 0);
            ESP_LOGI(TAG, "3 秒后重启...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        }
    }
    /* GET 请求 → 返回配置页 */
    else {
        send(sock, g_config_page, strlen(g_config_page), 0);
    }

    closesocket(sock);
}

/* HTTP 服务器任务 */
static void http_server_task(void *arg)
{
    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) { vTaskDelete(NULL); return; }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(80);

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "HTTP 绑定 80 端口失败");
        closesocket(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    listen(listen_sock, AP_MAX_CONNECT);

    ESP_LOGI(TAG, "配网页面已启动: http://192.168.4.1");

    while (1) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int client_sock = accept(listen_sock, (struct sockaddr *)&client, &client_len);
        if (client_sock >= 0) {
            handle_http_client(client_sock);
        }
    }
}

/* ═══════════════════════════════════════════════════
 *  AP 模式启动
 * ═══════════════════════════════════════════════════ */

static void start_ap_mode(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init_cfg);

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .password = "",
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = AP_MAX_CONNECT,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_cfg);
    esp_wifi_start();

    ESP_LOGI(TAG, "AP 模式已启动: %s (无密码)", AP_SSID);
    ESP_LOGI(TAG, "请连接此热点，浏览器打开 http://192.168.4.1");

    /* 启动 HTTP 配网服务器任务 */
    xTaskCreate(http_server_task, "http_srv", 4096, NULL,
                tskIDLE_PRIORITY + 2, NULL);

    /* 挂起当前上下文，HTTP 任务处理配网，成功后 esp_restart */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ═══════════════════════════════════════════════════
 *  轻量 mDNS (不依赖 mdns 组件, lwip 裸 UDP 实现)
 * ═══════════════════════════════════════════════════ */

static uint32_t g_local_ip = 0;

static void mdns_send_response(int sock, const uint8_t *query, int qlen)
{
    /* 只应答查询 "xray-io" 的 mDNS 包 */
    int found = 0;
    for (int i = 0; i <= qlen - 6; i++) {
        if (memcmp(query + i, "xray-io", 6) == 0) { found = 1; break; }
    }
    if (!found) return;

    uint8_t buf[256];
    int p = 0;

    /* DNS Header: Transaction ID = first 2 bytes of query, Flags=0x8400 (response) */
    memcpy(buf + p, query, 2); p += 2;
    buf[p++] = 0x84; buf[p++] = 0x00;  /* response, no error */
    buf[p++] = 0x00; buf[p++] = 0x00;  /* QDCOUNT = 0 (we echo query in answer) */
    buf[p++] = 0x00; buf[p++] = 0x01;  /* ANCOUNT = 1 */
    buf[p++] = 0x00; buf[p++] = 0x00;  /* NSCOUNT = 0 */
    buf[p++] = 0x00; buf[p++] = 0x00;  /* ARCOUNT = 0 */

    /* Answer: name (compressed: 0xc0 0x0c points to question name at offset 12) */
    buf[p++] = 0xc0; buf[p++] = 0x0c;
    buf[p++] = 0x00; buf[p++] = 0x01;  /* TYPE = A */
    buf[p++] = 0x00; buf[p++] = 0x01;  /* CLASS = IN */
    buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x78; /* TTL = 120s */
    buf[p++] = 0x00; buf[p++] = 0x04;  /* RDLENGTH = 4 */
    uint32_t ip = g_local_ip;
    buf[p++] = (uint8_t)(ip & 0xff);
    buf[p++] = (uint8_t)((ip >> 8) & 0xff);
    buf[p++] = (uint8_t)((ip >> 16) & 0xff);
    buf[p++] = (uint8_t)((ip >> 24) & 0xff);

    struct sockaddr_in dest = { .sin_family = AF_INET, .sin_port = htons(5353) };
    dest.sin_addr.s_addr = g_local_ip; /* unicast response back to querier */
    sendto(sock, buf, p, 0, (struct sockaddr *)&dest, sizeof(dest));
}

static void mdns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { vTaskDelete(NULL); return; }

    /* Bind to mDNS port and join multicast group */
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(5353) };
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    struct ip_mreq mreq = {
        .imr_multiaddr = { .s_addr = htonl(0xe00000fb) }, /* 224.0.0.251 */
        .imr_interface = { .s_addr = g_local_ip }
    };
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    uint8_t buf[300];
    while (1) {
        int len = recv(sock, buf, sizeof(buf), 0);
        if (len > 12) {
            mdns_send_response(sock, buf, len);
        }
    }
}

static void mdns_start(uint32_t ip)
{
    g_local_ip = ip;
    xTaskCreate(mdns_task, "mdns", 2560, NULL, tskIDLE_PRIORITY + 1, NULL);
}

/* ═══════════════════════════════════════════════════
 *  对外入口
 * ═══════════════════════════════════════════════════ */

/**
 * @brief  WiFi 初始化 — NVS 凭据加载 → STA 连接 → AP 配网回退
 *
 * ── 决策树 ──
 *   1. NVS 有无凭据？
 *       无 → 开 AP 热点 XRay_Config + HTTP 配网页（不返回）
 *       有 → 继续
 *   2. 用 NVS 凭据尝试 STA 连接（最多 AP_RETRY_COUNT 轮）
 *       成功 → 返回 0，调用方启动 TCP Server
 *       3 轮全失败 → 开 AP 热点（不返回）
 *
 * ── 返回值 ──
 *   0  = STA 连接成功，g_wifi_state.connected=1, g_wifi_state.ip_str 有效
 *   -1 = 启动 AP 配网（ap start_ap_mode 内部有死循环，实际不会到这里）
 *
 * ── 副作用 ──
 *   调用 esp_netif_init() / esp_event_loop_create_default()（全局仅一次）
 *   成功时 g_wifi_state 已填充
 */
int wifi_init(void)
{
    /* ── 0: 网络栈初始化（TCP/IP + 事件循环，STA 和 AP 共用）── */
    esp_netif_init();              /* 初始化 TCP/IP 协议栈 */
    esp_event_loop_create_default(); /* 创建默认事件循环（WiFi 驱动回调用） */

    /* ── 1: 从 NVS 读取上次保存的 SSID/密码/IP ── */
    if (load_creds_from_nvs() != 0) {
        ESP_LOGI(TAG, "首次启动，进入 AP 配网模式");
        start_ap_mode();           /* 开热点 XRay_Config + HTTP 配网页 → 内部死循环 */
        return -1;                 /* 不可达（start_ap_mode 不返回） */
    }

    /* ── 2: 有凭据 → 创建 STA netif → 循环尝试连接 ── */
    esp_netif_create_default_wifi_sta();  /* 创建 STA 网络接口（仅一次） */

    for (int attempt = 1; attempt <= AP_RETRY_COUNT; attempt++) {
        ESP_LOGI(TAG, "连接尝试 %d/%d: %s", attempt, AP_RETRY_COUNT, g_ssid);

        /* wifi_connect_sta: esp_wifi_init → set_mode(STA) → set_config → start → 等事件 */
        if (wifi_connect_sta(g_ssid, g_pwd) == 0) {
            return 0;              /* 连接成功，TCP Server 等 BLE 继续 */
        }

        /* 本轮失败 → 停 WiFi、反初始化硬件 → 等 2 秒再试 */
        if (attempt < AP_RETRY_COUNT) {
            esp_wifi_stop();        /* 停止 WiFi 状态机 */
            esp_wifi_deinit();      /* 释放 WiFi 驱动资源（下次 esp_wifi_init 才能成功） */
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    /* ── 3: AP_RETRY_COUNT 轮全失败 → 开 AP 配网 ── */
    ESP_LOGW(TAG, "STA 连接全部失败，进入 AP 配网模式");
    start_ap_mode();               /* 同步骤 1 */

    /* 保底死循环（start_ap_mode 内已有，这里双重保险） */
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    return -1;                     /* 不可达 */
}
