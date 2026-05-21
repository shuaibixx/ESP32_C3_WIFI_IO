"""
PC 端测试脚本 — ESP32-C3 WiFi IO 控制

用法:
    python pc_test.py <ESP32_IP地址>

示例:
    python pc_test.py 192.168.1.100

通信协议 (4 字节帧):
    [0xF1/F2] [CMD] [DATA] [CHECKSUM]
    CMD      = 0x01 (设置 GPIO 输出)
    DATA     = bit0 控制 GPIO0 (0=低电平, 1=高电平), bit1~7 预留
    CHECKSUM = CMD XOR DATA
"""

import socket
import sys

# 协议常量
PROTO_HEADER_RX = 0xF1    # 接收帧头 (PC→ESP32)
PROTO_HEADER_TX = 0xF2    # 发送帧头 (ESP32→PC)
CMD_SET_GPIO  = 0x01      # 命令: 设置 GPIO
CMD_READ_GPIO = 0x02      # 命令: 读取 GPIO
TCP_PORT = 8080           # ESP32-C3 TCP Server 端口

def calc_checksum(cmd, data):
    """计算异或校验值: CHECKSUM = CMD ^ DATA"""
    return cmd ^ data

def make_frame(cmd, data):
    """组装 4 字节协议帧"""
    chk = calc_checksum(cmd, data)
    return bytes([PROTO_HEADER_RX, cmd, data, chk])

def send_and_recv(sock, frame):
    """发送一帧并接收响应"""
    sock.sendall(frame)
    print(f"  发送: {' '.join(f'{b:02X}' for b in frame)}")

    resp = sock.recv(4)  # 接收 4 字节响应
    if len(resp) == 4:
        print(f"  响应: {' '.join(f'{b:02X}' for b in resp)}")

        # 解析响应
        header, cmd, data, chk = resp
        calc = calc_checksum(cmd, data)

        if header != PROTO_HEADER_TX:
            print(f"  ⚠ 帧头错误: 0x{header:02X}")
        elif chk != calc:
            print(f"  ⚠ 校验错误: 计算=0x{calc:02X}, 收到=0x{chk:02X}")
        else:
            level = data & 0x01  # bit0 = IO 电平
            print(f"  ✓ GPIO0 当前状态: {'高电平' if level else '低电平'}")
    else:
        print(f"  ⚠ 未收到完整响应 (收到 {len(resp)} 字节)")

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        print("错误: 请指定 ESP32-C3 的 IP 地址")
        sys.exit(1)

    ip = sys.argv[1]
    print(f"连接到 ESP32-C3: {ip}:{TCP_PORT}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)  # 5 秒超时

    try:
        sock.connect((ip, TCP_PORT))
        print("连接成功!\n")
    except Exception as e:
        print(f"连接失败: {e}")
        sys.exit(1)

    print("命令说明:")
    print("  1     → 设置 GPIO0 高电平")
    print("  0     → 设置 GPIO0 低电平")
    print("  r     → 读取 GPIO0 状态")
    print("  q     → 退出")
    print("-" * 40)

    try:
        while True:
            cmd_input = input("> ").strip().lower()

            if cmd_input == 'q':
                print("退出")
                break

            elif cmd_input == '1':
                # 设置 GPIO0 高电平: DATA = 0x01
                frame = make_frame(CMD_SET_GPIO, 0x01)
                send_and_recv(sock, frame)

            elif cmd_input == '0':
                # 设置 GPIO0 低电平: DATA = 0x00
                frame = make_frame(CMD_SET_GPIO, 0x00)
                send_and_recv(sock, frame)

            elif cmd_input == 'r':
                # 读取当前状态: CMD_READ_GPIO 只读不写
                frame = make_frame(CMD_READ_GPIO, 0x00)
                send_and_recv(sock, frame)

            elif cmd_input == '':
                continue

            else:
                print(f"  未知命令: {cmd_input}")

    except KeyboardInterrupt:
        print("\n中断退出")
    except socket.timeout:
        print("连接超时")
    except ConnectionResetError:
        print("连接被 ESP32-C3 重置")
    finally:
        sock.close()

if __name__ == '__main__':
    main()
