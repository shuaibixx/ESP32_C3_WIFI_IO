# ESP32-C3 WiFi + BLE IO 通信协议

## 1. 概要

ESP32-C3 提供 **WiFi TCP** 和 **BLE GATT** 两条通道，共用同一个 4 字节协议帧控制 GPIO0。

```
         WiFi TCP 8080                    BLE GATT
  PC ──────────────────→ ESP32 ←─── 手机/nRF Connect
              │                  │
              └── protocol_process() ──→ GPIO0
```

- **传输**: TCP 8080 (帧头 F1/F2) + BLE GATT (帧头 E1/E2)
- **帧长**: 固定 4 字节
- **校验**: CMD XOR DATA
- **引脚**: GPIO0
- **配网**: AP 网页，支持固定 IP，一次配置永久记忆

### 双模架构

```
app_main()
  ├─ nvs_flash_init()
  ├─ io_init()
  ├─ wifi_init()          ← STA 连接 / AP 配网回退
  ├─ ble_init()           ← BLE 独立启动（WiFi 无关）
  └─ TCP Server 8080      ← WiFi 就绪后启动
```

---
### wifi软件架构
app_main()
  │
  ├─ nvs_flash_init()          ← NVS 初始化（WiFi 凭据存储）
  ├─ io_init()                 ← GPIO0 配置为输入输出
  │
  ├─ wifi_init()               ← WiFi 初始化
  │     │
  │     ├─ 读 NVS 有无凭据
  │     │     │
  │     │     ├─ 有 → STA 连 WiFi（3 轮重试）
  │     │     │     ├─ 成功 → 返回 0
  │     │     │     └─ 失败 → 开 AP 配网
  │     │     │
  │     │     └─ 无 → 开 AP 配网
  │     │           │
  │     │           └─ 热点 XRay_Config → HTTP :80 配网页
  │     │                   │
  │     │                   用户浏览器 192.168.4.1
  │     │                   填 SSID+密码+固定IP(可选)
  │     │                   → 存 NVS → esp_restart()
  │     │
  │     └─ 连接成功 → g_wifi_state.connected = 1
  │
  └─ TCP Server 任务（端口 8080）
        │
        accept() 等 PC 连接
          │
          ├─ recv() 收数据（缓冲区 256 字节）
          ├─ 滑动窗口逐帧解析（步长 1 字节）
          │     │
          │     └─ protocol_process() 每帧：
          │           1. 长度 ≥ 4 字节
          │           2. 帧头 = 0xF1（否则跳过）
          │           3. XOR 校验 CMD ^ DATA
          │           4. switch(cmd)
          │              ├─ 0x01 → io_set_from_byte() → io_read_to_byte()
          │              ├─ 0x02 → io_read_to_byte()（只读）
          │              └─ 0x03 → nvs_erase("wifi") → esp_restart()
          │           5. 组装响应帧（头 0xF2 + 回显 CMD + IO 状态 + 校验）
          │
          └─ send() 响应帧回 PC

---
### 蓝牙软件架构
上电 → ble_init()
         │
         ├─ 回调设置 (reset_cb + sync_cb)
         ├─ nimble_port_init()       ← NimBLE 栈初始化
         ├─ GAP + GATT 服务注册
         ├─ 设备名 "XRay_BLE"
         └─ nimble 主机任务启动
               │
               └─ nimble_port_run()  ← 死循环处理协议栈事件
                     │
                     ├─ ble_on_sync() 触发
                     │     └─ 启动广播 ──────────────────┐
                     │                                    │
      ┌──────────────┘                                    │
      │         客户端搜索 → 发现 "XRay_BLE"                │
      │         客户端点连接                                │
      │              │                                    │
      │    BLE_GAP_EVENT_CONNECT                           │  客户端
      │    "客户端已连接"                                    │  写 RX 特征
      │              │                                    │  F1 01 01 00
      │    ←── rx_char_access() 接收 4 字节 ──────────────┘
      │              │
      │    protocol_process() 解析帧 → 控制 GPIO0
      │    生成响应 tx_buf = F2 01 01 00
      │              │
      │    ble_gatts_notify_custom() 推送响应 ──→ 客户端 TX 通知
      │              │
      │    BLE_GAP_EVENT_DISCONNECT
      │    "客户端断开"
      │              │
      │    └── 恢复广播 ──────────────→ 等待下一个客户端


## 2. 配网

```
首次开机 / 换网
  │
  └→ 创建热点 "XRay_Config"（无密码）
        │
        手机或 PC 连接热点 → 192.168.4.1
        │
        填写:
          WiFi SSID       (必填)
          WiFi Password   (选填)
          IP 末位         (选填, 如 200 → 固定 192.168.x.200)
        │
        保存 → 自动重启 → 连网就绪
```

---

## 3. 帧格式

```
┌──────────┬──────────┬──────────┬──────────┐
│  HEADER  │   CMD    │   DATA   │ CHECKSUM │
│0xF1/0xF2 │ 0x01~03  │  bit0=IO │ CMD^DATA │
└──────────┴──────────┴──────────┴──────────┘
```

### HEADER (Byte 0) — 传输层区分

| 传输层 | 请求 (客户端→ESP32) | 响应 (ESP32→客户端) |
|--------|--------------------|-------------------|
| WiFi TCP | `0xF1` | `0xF2` |
| BLE GATT | `0xE1` | `0xE2` |

### CMD (Byte 1)

| 值 | 宏 | 说明 |
|----|-----|------|
| `0x01` | CMD_SET_GPIO | 写 GPIO 电平 |
| `0x02` | CMD_READ_GPIO | 读 GPIO 电平（只读不写）|
| `0x03` | CMD_RESET_WIFI | 清除 WiFi 凭据并重启 |
| `0x10` | CMD_BOOT_INFO | 开机包 — ESP32→PC 推送（变长帧）|

### DATA (Byte 2)

bit0 = GPIO 电平 (0 低 / 1 高)，bit1~7 保留。

> **CMD_BOOT_INFO (0x10) 特殊说明**: 该命令使用变长帧格式，帧长 > 4 字节，规则见第 4 节。

### CHECKSUM (Byte 3)

`CHECKSUM = CMD ^ DATA`

> **CMD_BOOT_INFO 的 CHECKSUM**: PAYLOAD 逐字节 XOR 得出 1 字节，见第 4 节。

---

## 4. 命令

### CMD_SET_GPIO (0x01)

| 方向 | 帧 |
|------|-----|
| 请求 | `F1 01 <data> <0x01 ^ data>` |
| 响应 | `F2 01 <resp> <0x01 ^ resp>` |

`resp.bit0` = GPI\O 当前实际电平。

### CMD_READ_GPIO (0x02)

| 方向 | 帧 |
|------|-----|
| 请求 | `F1 02 00 02` |
| 响应 | `F2 02 <resp> <0x02 ^ resp>` |

只回读，不改变电平。

### CMD_RESET_WIFI (0x03)

| 方向 | 帧 |
|------|-----|
| 请求 | `F1 03 00 03` |

清除 NVS 中 WiFi 凭据 → 重启 → 进 AP 配网模式。

### CMD_BOOT_INFO (0x10) — 开机包

TCP 客户端连上后，ESP32 主动推送，一帧包含固件版本、设备 MAC、IP 地址。

**帧格式**（变长）：

```
┌────────┬────────┬────────┬───────────────────┬──────────┐
│ 0xF2   │  0x10  │ LENGTH │     PAYLOAD       │ CHECKSUM │
│ 帧头   │ 命令   │  1 字节 │  ASCII 逗号分隔    │  1 字节  │
└────────┴────────┴────────┴───────────────────┴──────────┘
```

**校验**: PAYLOAD 所有字节逐次 XOR（逗号参与）。

**PAYLOAD 格式**: `固件版本,MAC地址,IP地址,BSSID,RSSI`

**示例**:

> `1.0.0,AA:BB:CC:DD:EE:FF,192.168.0.200,A4:2B:B0:DC:12:34,-65`
> `─┬── ─────────┬───────── ─────┬────── ───────┬─────── ─┬─`
>  固件版本     设备MAC          IP地址         路由器MAC   信号强度(dBm)

| 方向 | 帧 (HEX) | 含义 |
|------|------|------|
| ESP32→PC | `F2 10 3D 31 2E ... 2D 36 35 4F` | 开机包 |

PC 端解析示例:
```python
def parse_boot(frame):
    # frame[0]=F2, frame[1]=10, frame[2]=len
    plen = frame[2]
    payload = frame[3:3+plen].decode('ascii')
    chk = frame[3+plen]
    # 校验
    calc = 0
    for b in payload.encode():
        calc ^= b
    if calc != chk:
        return None
    ver, mac, ip, bssid, rssi = payload.split(',')
    return {'version': ver, 'mac': mac, 'ip': ip, 'bssid': bssid, 'rssi': int(rssi)}
```

---

## 5. 示例

| 步骤 | 方向 | 帧 | 含义 |
|------|------|-----|------|
| – | ESP32→PC | `F2 10 35 ...` | 开机包（自动推送）|
| 1 | PC→ESP32 | `F1 01 01 00` | 置高 |
| 2 | ESP32→PC | `F2 01 01 00` | 确认置高 |
| 3 | PC→ESP32 | `F1 02 00 02` | 读取 |
| 4 | ESP32→PC | `F2 02 01 03` | 状态=高 |
| 5 | PC→ESP32 | `F1 01 00 01` | 置低 |
| 6 | ESP32→PC | `F2 01 00 01` | 确认置低 |

---

## 6. 错误处理

| 错误 | ESP32 行为 |
|------|-----------|
| 帧长 < 4 | 丢弃，等待更多数据 |
| 帧头 ≠ 0xF1 | 滑动窗口跳过 |
| 校验不匹配 | 丢弃并打日志 |
| 未知 CMD | 丢弃并打日志 |

---

## 7. BLE 使用

ESP32 广播 `XRay_BLE`，GATT Service 含两个特征：

| 特征 | UUID 末位 | 属性 | 说明 |
|------|----------|------|------|
| RX | ...0E10 | Write | 写入 4 字节协议帧（帧头 E1） |
| TX | ...0E11 | Notify | 推送 4 字节响应帧（帧头 E2） |

**BLE 专用帧头**: 请求 `0xE1`，响应 `0xE2`

**操作步骤**（nRF Connect）：
1. 搜索 `XRay_BLE` → Connect
2. 订阅 TX 特征以接收响应通知
3. 向 RX 特征写入协议帧（如 `E1010100` 置高）
4. TX 通知弹出响应（如 `E2010100`）

**Python 测试**：
```bash
python ble_test.py    # 需要 PC 有蓝牙适配器
```

## 8. TCP 测试

```bash
python -c "import socket; s=socket.socket(); s.connect(('192.168.0.200',8080)); s.sendall(bytes([0xF1,0x03,0x00,0x03])); s.close()"                                         # 清除 WiFi 凭据并重启进 AP 配网
python pc_test.py 192.168.0.200           # 交互测试 (1/0/r/q)
python full_test.py 192.168.0.200         # 12 项全自动回归
python full_test.py 192.168.0.200 --loop  # 持续压测
python simulate_test.py                   # 本地模拟（无需硬件）
```

---

## 9. PC 端参考代码

```python
import socket

HEADER_RX = 0xF1
HEADER_TX = 0xF2
CMD_SET   = 0x01
CMD_READ  = 0x02

def frame(cmd, data):
    return bytes([HEADER_RX, cmd, data, cmd ^ data])

s = socket.socket()
s.connect(("192.168.0.200", 8080))

# 置高
s.sendall(frame(CMD_SET, 0x01))
r = s.recv(4)
print(f"GPIO={'高' if r[2]&1 else '低'}")

# 读取
s.sendall(frame(CMD_READ, 0x00))
r = s.recv(4)
print(f"状态={'高' if r[2]&1 else '低'}")

s.close()
```

### BLE 端（Python bleak，帧头 0xE1）
```python
import asyncio
from bleak import BleakClient

RX_UUID = "100e0d0c-0b0a-0908-0706-050403020100"
TX_UUID = "110e0d0c-0b0a-0908-0706-050403020100"

async def main():
    async with BleakClient("XRay_BLE") as client:
        def on_notify(sender, data):
            print(f"响应(E2): {' '.join(f'{b:02X}' for b in data)}")

        await client.start_notify(TX_UUID, on_notify)
        await client.write_gatt_char(RX_UUID, bytes([0xE1,0x01,0x01,0x00]))

asyncio.run(main())
```

