/**
 * @file    main.c
 * @brief   ESP32-C3-MINI-1 双模 IO 控制 — 主程序
 *
 * ── 启动流程 ──
 *   1. NVS 初始化（WiFi 凭据存储）
 *   2. IO 初始化（GPIO0 = 输入输出，默认低电平）
 *   3. WiFi 初始化（NVS 有凭据 → STA 连接；无凭据/失败 → AP 网页配网）
 *   4. BLE GATT 服务器启动（WiFi 无关，始终可用）
 *   5. TCP Server 8080 启动（WiFi 就绪后）
 *
 * ── 双模通信 ──
 *   WiFi TCP :  PC ──TCP 8080──→ ESP32 ──protocol_process()──→ GPIO0
 *   BLE GATT :  手机 ──BLE GATT──→ ESP32 ──protocol_process()──→ GPIO0
 *   同一协议帧格式，两个传输通道共享同一个协议处理器
 *
 * ── 硬件平台 ──
 *   芯片: ESP32-C3-MINI-1 (单核 RISC-V 160MHz, 2MB Flash)
 *   IO:   GPIO0 (MODE_INPUT_OUTPUT)
 *   通信: WiFi STA 2.4GHz + BLE 5.0
 *
 * ── 配网 ──
 *   AP 网页: 首次开机开热点 XRay_Config → 浏览器 192.168.4.1
 *   TCP 远程: 发送 F1 03 00 03 清除凭据并重启进配网
 *
 * 协议详见 protocol.h
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"          /* LWIP socket API (与 POSIX socket 兼容) */
#include "wifi_config.h"           /* WiFi STA 连接 */
#include "protocol.h"              /* 自定义协议 + IO 控制 */
#include "ble_config.h"            /* BLE GATT 服务器 */

static const char *TAG = "main";

/* ── TCP Server 配置 ── */
#define TCP_PORT            8080                /* 监听端口，PC 端连接此端口 */
#define TCP_RX_BUF_SIZE     256                 /* 接收缓冲区 (覆盖多帧) */
#define TCP_SERVER_PRIO     (tskIDLE_PRIORITY + 3)
#define TCP_SERVER_STACK    4096

/* 函数声明 */
static void tcp_server_task(void *arg);

/* ═══════════════════════════════════════════════════
 *  程序入口
 * ═══════════════════════════════════════════════════ */
void app_main(void)
{
    /* ── 第 1 步: 初始化 NVS (WiFi 驱动需要) ── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS 分区损坏或版本不匹配，擦除后重试 */
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* ── 第 2 步: 初始化 IO (GPIO0 设为输出，默认低电平) ── */
    io_init();

    /* ── 第 3 步: 连接 WiFi（自动 NVS 读取 / AP 配网回退） ── */
    int wifi_ret = wifi_init();

    /* ── 第 4 步: 启动 BLE（WiFi 无关，始终可用） ── */
    ble_init();

    /* ── 第 5 步: WiFi 就绪后启动 TCP Server ── */
    if (wifi_ret == 0 && g_wifi_state.connected) {
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "  TCP Server1 已就绪");
        ESP_LOGI(TAG, "  端口: %d", TCP_PORT);
        ESP_LOGI(TAG, "  IP  : %s", g_wifi_state.ip_str);
        ESP_LOGI(TAG, "  等待 PC 连接...");
        ESP_LOGI(TAG, "========================================");

        /* 创建 TCP 服务器任务 */
        xTaskCreate(tcp_server_task, "tcp_server",
                    TCP_SERVER_STACK, NULL, TCP_SERVER_PRIO, NULL);
    } else {
        ESP_LOGW(TAG, "WiFi 未连接，仅 BLE 可用");
    }
}

/* ═══════════════════════════════════════════════════
 *  TCP Server 任务
 *  接收 PC 发来的协议帧 → 控制 IO → 返回响应
 * ═══════════════════════════════════════════════════ */
static void tcp_server_task(void *arg)
{
    int listen_sock;                            /* 监听 socket */
    int client_sock;                            /* 客户端连接 socket */
    struct sockaddr_in server_addr;             /* 服务器地址 */
    struct sockaddr_in client_addr;             /* 客户端地址 */
    socklen_t addr_len = sizeof(client_addr);

    uint8_t rx_buf[TCP_RX_BUF_SIZE];            /* 接收缓冲区 */
    uint8_t tx_buf[FRAME_LEN];                  /* 发送缓冲区 (4 字节) */

    /* ── 创建 socket ── */
    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "创建 socket 失败");
        vTaskDelete(NULL);
        return;
    }

    /* ── 设置地址重用 (避免重启后端口被占用) ── */
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* ── 绑定到所有网络接口的 TCP_PORT ── */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);   /* 0.0.0.0，监听所有网卡 */
    server_addr.sin_port = htons(TCP_PORT);            /* 端口号，网络字节序 */

    if (bind(listen_sock,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "绑定端口 %d 失败", TCP_PORT);
        closesocket(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    /* ── 开始监听 (backlog=1, 只允许 1 个排队连接) ── */
    if (listen(listen_sock, 1) < 0) {
        ESP_LOGE(TAG, "监听失败");
        closesocket(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP Server 已启动，等待客户端连接...");

    /* ── 主循环: 接受客户端 → 收发数据 → 断开 → 重新等待 ── */
    while (1) {

        /* 阻塞等待客户端连接 */
        client_sock = accept(listen_sock,
                             (struct sockaddr *)&client_addr,
                             &addr_len);

        if (client_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));     /* 出错，短暂延时后重试 */
            continue;
        }

        ESP_LOGI(TAG, "客户端已连接!");
        ESP_LOGI(TAG, "可以发送协议帧控制 GPIO%d 了", IO_PIN);
        ESP_LOGI(TAG, "示例: F1 01 01 00 → GPIO%d=高电平", IO_PIN);
        ESP_LOGI(TAG, "示例: F1 01 00 01 → GPIO%d=低电平", IO_PIN);

        /* ── 客户端连接后的收发循环 ── */
        while (1) {

            /* 接收数据 */
            int len = recv(client_sock, rx_buf, TCP_RX_BUF_SIZE, 0);

            if (len <= 0) {
                /* 客户端断开连接 (len=0) 或出错 (len<0) */
                ESP_LOGI(TAG, "客户端断开连接");
                break;
            }

            /* 打印收到的原始数据 (调试用) */
            ESP_LOGI(TAG, "收到 %d 字节:", len);
            for (int i = 0; i < len; i++) {
                printf("%02X ", rx_buf[i]);
            }
            printf("\n");

            /*
             * 逐帧滑动解析：
             *   如果收到多帧数据 (如 8 字节 = 2 帧)，
             *   滑动窗口依次处理每一帧。
             */
            for (int i = 0; i <= len - FRAME_LEN; i++) {
                int resp_len = protocol_process(PROTO_WIFI, &rx_buf[i], FRAME_LEN, tx_buf);

                if (resp_len > 0) {
                    /* 协议解析成功 → 发送响应帧给 PC */
                    send(client_sock, tx_buf, resp_len, 0);

                    /* 打印响应内容 */
                    ESP_LOGI(TAG, "→ 响应: CMD=0x%02X IO状态=%d",
                             tx_buf[1], tx_buf[2] & 0x01);
                }
            }
        }

        /* 关闭客户端连接，回到 accept 等待下一个连接 */
        closesocket(client_sock);
    }

    /* 正常不会走到这里 */
    closesocket(listen_sock);
    vTaskDelete(NULL);
}
