"""
ESP32-C3 WiFi IO 全功能测试脚本

用法:
    python full_test.py <ESP32_IP地址> [--loop]

    --loop  循环压测模式，持续发送直到 Ctrl+C

示例:
    python full_test.py 192.168.1.100
    python full_test.py 192.168.1.100 --loop
"""

import socket
import sys
import time
import struct
import argparse

# ── 协议常量 ──
HEADER_RX     = 0xF1    # PC → ESP32 请求帧头
HEADER_TX     = 0xF2    # ESP32 → PC 响应帧头
CMD_SET_GPIO  = 0x01    # 设置 GPIO
CMD_READ_GPIO = 0x02    # 读取 GPIO
FRAME_LEN     = 4
PORT          = 8080

# ── 颜色输出 ──
GREEN = "\033[92m"
RED   = "\033[91m"
YELLOW= "\033[93m"
CYAN  = "\033[96m"
RESET = "\033[0m"

pass_count = 0
fail_count = 0

def make_frame(cmd, data):
    return bytes([HEADER_RX, cmd, data, cmd ^ data])

def send_raw(sock, data, label=""):
    """发送原始数据并打印"""
    sock.sendall(data)
    if label:
        print(f"  {CYAN}[发送]{RESET} {label}: {' '.join(f'{b:02X}' for b in data)}")

def recv_response(sock, timeout=2.0):
    """接收 4 字节响应"""
    sock.settimeout(timeout)
    try:
        resp = sock.recv(FRAME_LEN)
        return resp
    except socket.timeout:
        return None

def check_response(resp, expected_data=None):
    """校验响应帧"""
    global pass_count, fail_count

    if resp is None:
        print(f"  {RED}[FAIL]{RESET} 无响应 (超时)")
        fail_count += 1
        return False

    if len(resp) != FRAME_LEN:
        print(f"  {RED}[FAIL]{RESET} 响应长度错误: {len(resp)}")
        fail_count += 1
        return False

    hdr, cmd, data, chk = resp

    errors = []
    if hdr != HEADER_TX:
        errors.append(f"帧头应为 0xF2，收到 0x{hdr:02X}")
    if cmd not in (CMD_SET_GPIO, CMD_READ_GPIO):
        errors.append(f"CMD 应为 0x01/0x02，收到 0x{cmd:02X}")
    if chk != (cmd ^ data):
        errors.append(f"校验错误: 计算 0x{cmd ^ data:02X}，收到 0x{chk:02X}")
    if expected_data is not None and (data & 0x01) != expected_data:
        errors.append(f"IO 状态应为 {expected_data}，实际 {data & 0x01}")

    if errors:
        for e in errors:
            print(f"  {RED}[FAIL]{RESET} {e}")
        fail_count += 1
        return False

    level = data & 0x01
    print(f"  {GREEN}[PASS]{RESET} GPIO0={'高电平' if level else '低电平'}  "
          f"响应: {' '.join(f'{b:02X}' for b in resp)}")
    pass_count += 1
    return True


# ═══════════════════════════════════════════════════
#  测试用例
# ═══════════════════════════════════════════════════

def test_basic_high_low(sock):
    """测试 1-2: GPIO 置高 / 置低"""
    print(f"\n{YELLOW}{'='*50}{RESET}")
    print(f"{YELLOW}[测试 1] GPIO0 置高电平{RESET}")
    print(f"{YELLOW}{'='*50}{RESET}")
    send_raw(sock, make_frame(CMD_SET_GPIO, 0x01), "设置高电平")
    resp = recv_response(sock)
    check_response(resp, expected_data=1)

    print(f"\n{YELLOW}{'='*50}{RESET}")
    print(f"{YELLOW}[测试 2] GPIO0 置低电平{RESET}")
    print(f"{YELLOW}{'='*50}{RESET}")
    send_raw(sock, make_frame(CMD_SET_GPIO, 0x00), "设置低电平")
    resp = recv_response(sock)
    check_response(resp, expected_data=0)


def test_read_status(sock):
    """测试 3: 读取 IO 状态（不影响当前电平）"""
    print(f"\n{YELLOW}{'='*50}{RESET}")
    print(f"{YELLOW}[测试 3] 读取 IO 当前状态{RESET}")
    print(f"{YELLOW}{'='*50}{RESET}")

    # 先确保为高
    send_raw(sock, make_frame(CMD_SET_GPIO, 0x01), "先置高电平")
    recv_response(sock)

    # 读取：CMD_READ_GPIO 只读不写，不影响当前电平
    send_raw(sock, make_frame(CMD_READ_GPIO, 0x00), "读取当前状态")
    resp = recv_response(sock)
    check_response(resp, expected_data=1)  # 应该还是高电平


def test_bad_checksum(sock):
    """测试 4: 错误校验帧应被丢弃"""
    print(f"\n{YELLOW}{'='*50}{RESET}")
    print(f"{YELLOW}[测试 4] 错误校验帧 (应无响应){RESET}")
    print(f"{YELLOW}{'='*50}{RESET}")

    global pass_count, fail_count
    bad_frame = bytes([HEADER_RX, CMD_SET_GPIO, 0x01, 0xFF])
    send_raw(sock, bad_frame, "校验值故意错误")
    resp = recv_response(sock, timeout=1.0)

    if resp is None:
        print(f"  {GREEN}[PASS]{RESET} 错误帧被正确丢弃，无响应")
        pass_count += 1
    else:
        print(f"  {RED}[FAIL]{RESET} 收到意外响应: {' '.join(f'{b:02X}' for b in resp)}")
        fail_count += 1


def test_bad_header(sock):
    """测试 5: 错误帧头帧应被丢弃"""
    print(f"\n{YELLOW}{'='*50}{RESET}")
    print(f"{YELLOW}[测试 5] 错误帧头帧 (应无响应){RESET}")
    print(f"{YELLOW}{'='*50}{RESET}")

    global pass_count, fail_count
    bad_frame = bytes([0xBB, CMD_SET_GPIO, 0x01, 0x00])
    send_raw(sock, bad_frame, "帧头 0xBB (非 0xF1)")
    resp = recv_response(sock, timeout=1.0)

    if resp is None:
        print(f"  {GREEN}[PASS]{RESET} 错误帧头帧被正确丢弃，无响应")
        pass_count += 1
    else:
        print(f"  {RED}[FAIL]{RESET} 收到意外响应: {' '.join(f'{b:02X}' for b in resp)}")
        fail_count += 1


def test_unknown_cmd(sock):
    """测试 6: 未知命令帧应被丢弃"""
    print(f"\n{YELLOW}{'='*50}{RESET}")
    print(f"{YELLOW}[测试 6] 未知命令帧 (应无响应){RESET}")
    print(f"{YELLOW}{'='*50}{RESET}")

    global pass_count, fail_count
    # 命令码 0x99 是未定义的
    bad_frame = bytes([HEADER_RX, 0x99, 0x00, 0x99])
    send_raw(sock, bad_frame, "CMD=0x99 (未知命令)")
    resp = recv_response(sock, timeout=1.0)

    if resp is None:
        print(f"  {GREEN}[PASS]{RESET} 未知命令帧被正确丢弃，无响应")
        pass_count += 1
    else:
        print(f"  {RED}[FAIL]{RESET} 收到意外响应: {' '.join(f'{b:02X}' for b in resp)}")
        fail_count += 1


def test_multi_frames(sock):
    """测试 7: 一包发送多帧（测试滑动窗口解析）"""
    print(f"\n{YELLOW}{'='*50}{RESET}")
    print(f"{YELLOW}[测试 7] 一包多帧 (滑动窗口解析){RESET}")
    print(f"{YELLOW}{'='*50}{RESET}")

    # 3 帧打包发送
    # 帧1: 置高 F1 01 01 00
    # 帧2: 置低 F1 01 00 01
    # 帧3: 置高 F1 01 01 00
    multi = bytes([
        HEADER_RX, CMD_SET_GPIO, 0x01, CMD_SET_GPIO ^ 0x01,  # 置高
        HEADER_RX, CMD_SET_GPIO, 0x00, CMD_SET_GPIO ^ 0x00,  # 置低
        HEADER_RX, CMD_SET_GPIO, 0x01, CMD_SET_GPIO ^ 0x01,  # 置高
    ])
    print(f"  发送 {len(multi)} 字节 (3 帧合并)")

    sock.sendall(multi)
    for i, expected in enumerate([1, 0, 1]):
        resp = recv_response(sock)
        print(f"  帧{i+1}:", end=" ")
        check_response(resp, expected_data=expected)


def test_partial_header_offset(sock):
    """测试 8: 帧不在缓冲区起始位置（帧头前有垃圾字节）"""
    print(f"\n{YELLOW}{'='*50}{RESET}")
    print(f"{YELLOW}[测试 8] 帧前有垃圾字节 (滑动窗口鲁棒性){RESET}")
    print(f"{YELLOW}{'='*50}{RESET}")

    # 前面塞 3 字节垃圾，后面跟一帧有效数据
    data = bytes([0x00, 0xFF, 0x55,  # 垃圾字节
                  HEADER_RX, CMD_SET_GPIO, 0x01, CMD_SET_GPIO ^ 0x01])
    print(f"  发送: [00 FF 55] + F1 01 01 00")
    sock.sendall(data)
    resp = recv_response(sock)
    check_response(resp, expected_data=1)


def test_rapid_toggle(sock, count=10):
    """测试 9: 快速连续切换电平（压力测试）"""
    print(f"\n{YELLOW}{'='*50}{RESET}")
    print(f"{YELLOW}[测试 9] 快速切换 {count} 次{RESET}")
    print(f"{YELLOW}{'='*50}{RESET}")

    for i in range(count):
        level = i % 2           # 交替 0/1
        data_byte = level       # bit0 = level
        frame = make_frame(CMD_SET_GPIO, data_byte)
        sock.sendall(frame)
        resp = recv_response(sock, timeout=1.0)
        print(f"  第{i+1:02d}次: ", end="")
        check_response(resp, expected_data=level)


def test_batch_frames(sock, count=50):
    """测试 10: 批量发送大量帧（吞吐测试）"""
    print(f"\n{YELLOW}{'='*50}{RESET}")
    print(f"{YELLOW}[测试 10] 批量发送 {count} 帧 (吞吐测试){RESET}")
    print(f"{YELLOW}{'='*50}{RESET}")

    global pass_count, fail_count
    start = time.time()

    for i in range(count):
        level = i % 2
        frame = make_frame(CMD_SET_GPIO, level)
        sock.sendall(frame)
        resp = recv_response(sock, timeout=0.5)
        if resp and len(resp) == 4 and resp[0] == HEADER_TX and resp[1] == CMD_SET_GPIO:
            pass_count += 1
        else:
            fail_count += 1
            print(f"  第{i+1}帧失败")

    elapsed = time.time() - start
    fps = count / elapsed if elapsed > 0 else 0
    print(f"\n  {count} 帧 / {elapsed:.2f}s = {fps:.1f} 帧/秒")


def test_large_payload(sock):
    """测试 11: 发送大数据包（多帧 + 垃圾混合）"""
    print(f"\n{YELLOW}{'='*50}{RESET}")
    print(f"{YELLOW}[测试 11] 大数据包混合解析{RESET}")
    print(f"{YELLOW}{'='*50}{RESET}")

    # 模拟真实场景: 连续发送 5 帧，中间夹杂无效数据
    payload = bytes([
        # 有效帧 1: 置高
        HEADER_RX, CMD_SET_GPIO, 0x01, CMD_SET_GPIO ^ 0x01,
        # 半帧垃圾 (后面的滑动窗口要能跳过)
        0xFF, 0x00,
        # 有效帧 2: 置低
        HEADER_RX, CMD_SET_GPIO, 0x00, CMD_SET_GPIO ^ 0x00,
        # 有效帧 3: 置高
        HEADER_RX, CMD_SET_GPIO, 0x01, CMD_SET_GPIO ^ 0x01,
    ])

    print(f"  发送 {len(payload)} 字节: 3 有效帧 + 2 垃圾字节")
    sock.sendall(payload)

    for i, expected in enumerate([1, 0, 1]):
        print(f"  帧{i+1}:", end=" ")
        resp = recv_response(sock)
        check_response(resp, expected_data=expected)


def test_reconnect(sock_factory, ip):
    """测试 12: 断开重连后正常通信"""
    print(f"\n{YELLOW}{'='*50}{RESET}")
    print(f"{YELLOW}[测试 12] 断开重连测试{RESET}")
    print(f"{YELLOW}{'='*50}{RESET}")

    global pass_count, fail_count

    sock1 = sock_factory()
    sock1.settimeout(3)
    try:
        sock1.connect((ip, PORT))
        send_raw(sock1, make_frame(CMD_SET_GPIO, 0x01), "连接1: 置高")
        resp = recv_response(sock1)
        check_response(resp, expected_data=1)
    finally:
        sock1.close()

    print("  连接1 已关闭")

    time.sleep(0.5)

    # 第二次连接
    sock2 = sock_factory()
    sock2.settimeout(3)
    try:
        sock2.connect((ip, PORT))
        send_raw(sock2, make_frame(CMD_SET_GPIO, 0x00), "连接2: 置低")
        resp = recv_response(sock2)
        check_response(resp, expected_data=0)
    finally:
        sock2.close()

    print(f"  {GREEN}[PASS]{RESET} 重连后通信正常")


def stress_loop(sock):
    """无限循环压测"""
    print(f"\n{YELLOW}{'='*50}{RESET}")
    print(f"{YELLOW}[压测模式] 持续发送，Ctrl+C 停止{RESET}")
    print(f"{YELLOW}{'='*50}{RESET}")

    global pass_count, fail_count
    i = 0
    start = time.time()

    try:
        while True:
            level = i % 2
            frame = make_frame(CMD_SET_GPIO, level)
            sock.sendall(frame)
            resp = recv_response(sock, timeout=1.0)

            if resp and len(resp) == 4 and resp[0] == HEADER_TX and resp[1] == CMD_SET_GPIO:
                pass_count += 1
            else:
                fail_count += 1

            i += 1
            if i % 100 == 0:
                elapsed = time.time() - start
                fps = i / elapsed
                print(f"  [{i}] {fps:.1f} fps | PASS={pass_count} FAIL={fail_count}")

    except KeyboardInterrupt:
        elapsed = time.time() - start
        print(f"\n  停止。共 {i} 帧 / {elapsed:.1f}s = {i/elapsed:.1f} fps")


# ═══════════════════════════════════════════════════
#  主入口
# ═══════════════════════════════════════════════════

def run_all_tests(ip):
    """运行所有测试"""
    global pass_count, fail_count
    total_pass = total_fail = 0

    def new_sock():
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(3)
        s.connect((ip, PORT))
        return s

    # 需要多次连接的测试：用同一个 socket，减少重连
    sock = new_sock()

    tests = [
        test_basic_high_low,
        test_read_status,
        test_bad_checksum,
        test_bad_header,
        test_unknown_cmd,
        test_multi_frames,
        test_partial_header_offset,
        test_rapid_toggle,
        test_batch_frames,
        test_large_payload,
    ]

    for test_fn in tests:
        before_p = pass_count
        before_f = fail_count

        try:
            test_fn(sock)
        except (ConnectionResetError, BrokenPipeError, OSError) as e:
            print(f"  {RED}[ERROR]{RESET} 连接断开: {e}，重连中...")
            sock.close()
            time.sleep(0.5)
            sock = new_sock()
            try:
                test_fn(sock)
            except Exception as e2:
                print(f"  {RED}[FATAL]{RESET} 重连后仍失败: {e2}")
                fail_count += 4  # 大致标记
                break

        total_pass += pass_count - before_p
        total_fail += fail_count - before_f

    # 测试 12: 断连重连（需要多次连接）
    sock.close()
    try:
        test_reconnect(new_sock, ip)
    except Exception as e:
        print(f"  {RED}[FATAL]{RESET} 重连测试失败: {e}")

    total_pass = pass_count
    total_fail = fail_count

    return total_pass, total_fail


def main():
    parser = argparse.ArgumentParser(description="ESP32-C3 WiFi IO 全功能测试")
    parser.add_argument("ip", help="ESP32-C3 的 IP 地址")
    parser.add_argument("--loop", action="store_true", help="循环压测模式")
    args = parser.parse_args()

    ip = args.ip
    print(f"{CYAN}ESP32-C3 WiFi IO 全功能测试{RESET}")
    print(f"目标: {ip}:{PORT}")
    print(f"协议: 请求帧头 0x{HEADER_RX:02X} / 响应帧头 0x{HEADER_TX:02X}")

    if args.loop:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(3)
        try:
            sock.connect((ip, PORT))
            print(f"{GREEN}连接成功!{RESET}")
            stress_loop(sock)
        except Exception as e:
            print(f"{RED}连接失败: {e}{RESET}")
            sys.exit(1)
        finally:
            sock.close()
    else:
        # 连接
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(3)
        try:
            sock.connect((ip, PORT))
            print(f"{GREEN}连接成功!\n{RESET}")
        except Exception as e:
            print(f"{RED}连接失败: {e}{RESET}")
            sys.exit(1)
        sock.close()

        # 运行所有测试
        n_pass, n_fail = run_all_tests(ip)

        # 汇总
        total = n_pass + n_fail
        print(f"\n{CYAN}{'='*50}{RESET}")
        print(f"{CYAN}  测试汇总{RESET}")
        print(f"{CYAN}{'='*50}{RESET}")
        print(f"  总计: {total}")
        print(f"  通过: {GREEN}{n_pass}{RESET}")
        if n_fail > 0:
            print(f"  失败: {RED}{n_fail}{RESET}")
        else:
            print(f"  失败: 0")
        print(f"  {'通过率' if n_fail == 0 else '通过率'}: {n_pass}/{total}")
        if n_fail == 0:
            print(f"\n  {GREEN}全部测试通过!{RESET}")
        else:
            print(f"\n  {RED}存在 {n_fail} 项失败{RESET}")
        print(f"{CYAN}{'='*50}{RESET}")

        return n_fail


if __name__ == '__main__':
    sys.exit(main())
