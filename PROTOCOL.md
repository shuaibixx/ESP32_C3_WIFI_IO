# ESP32-C3 WiFi IO 通信协议

## 1. 概要

PC 通过 WiFi TCP 协议控制 ESP32-C3 的 GPIO0 输出。

- **传输**: TCP 8080
- **帧长**: 固定 4 字节
- **校验**: CMD XOR DATA
- **引脚**: GPIO0
- **配网**: AP 网页，支持固定 IP，一次配置永久记忆

---

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

### HEADER (Byte 0)

| 方向 | 值 |
|------|-----|
| PC → ESP32 | `0xF1` |
| ESP32 → PC | `0xF2` |

### CMD (Byte 1)

| 值 | 宏 | 说明 |
|----|-----|------|
| `0x01` | CMD_SET_GPIO | 写 GPIO 电平 |
| `0x02` | CMD_READ_GPIO | 读 GPIO 电平（只读不写）|
| `0x03` | CMD_RESET_WIFI | 清除 WiFi 凭据并重启 |

### DATA (Byte 2)

bit0 = GPIO 电平 (0 低 / 1 高)，bit1~7 保留。

### CHECKSUM (Byte 3)

`CHECKSUM = CMD ^ DATA`

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

---

## 5. 示例

| 步骤 | 方向 | 帧 | 含义 |
|------|------|-----|------|
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

## 7. 测试

```bash
python -c "import socket; s=socket.socket(); s.connect(('192.168.0.200',8080)); s.sendall(bytes([0xF1,0x03,0x00,0x03])); s.close()"               # NVS 并重启进 AP 配网模式
python pc_test.py 192.168.0.200        # 交互测试 (1/0/r/q)
python full_test.py 192.168.0.200      # 12 项全自动回归
python full_test.py 192.168.0.200 --loop  # 持续压测
python simulate_test.py                # 本地模拟（无需硬件）
```

---

## 8. PC 端参考代码

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
