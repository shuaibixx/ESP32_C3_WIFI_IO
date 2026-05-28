"""
ESP32-C3 WiFi IO 统一测试脚本

用法:
    python test.py <mode> [args]

模式:
    sim          本地模拟测试（无需硬件）
    boot  <IP>   仅测试开机包
    run   <IP>   交互式 IO 控制
    full  <IP>   全自动 12 项测试
    stress <IP>  循环压测（Ctrl+C 停止）

示例:
    python test.py sim
    python test.py boot 192.168.0.20
    python test.py run  192.168.0.20
    python test.py full 192.168.0.20
    python test.py stress 192.168.0.20
"""

import socket
import sys
import time
import threading

# ========== Protocol ==========
HDR_RX       = 0xF1
HDR_TX       = 0xF2
CMD_SET      = 0x01
CMD_READ     = 0x02
CMD_BOOT     = 0x10
FRAME_LEN    = 4
PORT         = 8080

GREEN = "\033[92m"
RED   = "\033[91m"
YELLOW= "\033[93m"
CYAN  = "\033[96m"
RST   = "\033[0m"

_p = _f = 0

def ok(msg): global _p; _p += 1; print(f"  {GREEN}[PASS]{RST} {msg}")
def no(msg): global _f; _f += 1; print(f"  {RED}[FAIL]{RST} {msg}")
def info(msg): print(f"  {CYAN}[INFO]{RST} {msg}")
def title(s):  print(f"\n{YELLOW}{'='*50}{RST}\n{YELLOW}{s}{RST}\n{YELLOW}{'='*50}{RST}")

def frame(cmd, data): return bytes([HDR_RX, cmd, data, cmd ^ data])

# ============================================================
#  本地模拟测试
# ============================================================
def mode_sim():
    """本地模拟：两个线程跑协议逻辑，无需硬件"""
    io_level = 0

    def server():
        nonlocal io_level
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(('127.0.0.1', PORT))
        s.listen(1)
        while True:
            c, a = s.accept()
            # Boot packet
            payload = b"1.0.0,AA:BB:CC:DD:EE:FF,192.168.0.200,D8:40:08:6B:83:D4,-64"
            chk = 0
            for b in payload: chk ^= b
            boot = bytes([HDR_TX, CMD_BOOT, len(payload)]) + payload + bytes([chk])
            c.sendall(boot)
            # Command loop
            while True:
                data = c.recv(256)
                if not data: break
                for i in range(len(data) - FRAME_LEN + 1):
                    f = data[i:i+FRAME_LEN]
                    if f[0] != HDR_RX: continue
                    cmd, d, chk = f[1], f[2], f[3]
                    if chk != (cmd ^ d): continue
                    if cmd == CMD_SET: io_level = d & 1
                    elif cmd == CMD_READ: pass
                    else: continue
                    rdata = io_level
                    c.sendall(bytes([HDR_TX, cmd, rdata, cmd ^ rdata]))
            c.close()

    t = threading.Thread(target=server, daemon=True)
    t.start()
    time.sleep(0.1)

    print("本地模拟测试\n")
    s = socket.socket(); s.connect(('127.0.0.1', PORT))

    # Boot packet
    data = s.recv(256)
    if data[0] == HDR_TX and data[1] == CMD_BOOT:
        plen = data[2]
        payload = data[3:3+plen].decode()
        print(f"开机包: {payload}")
        ok("开机包")
    else:
        no("开机包")

    # GPIO tests
    tests = [
        ("置高", CMD_SET, 0x01, 1),
        ("置低", CMD_SET, 0x00, 0),
    ]
    for label, cmd, d, exp in tests:
        s.sendall(frame(cmd, d))
        r = s.recv(4)
        if r[0] == HDR_TX and r[1] == cmd and r[3] == (r[1] ^ r[2]) and (r[2] & 1) == exp:
            ok(f"GPIO {label}")
        else:
            no(f"GPIO {label}")

    # Read test: first set high, then read
    s.sendall(frame(CMD_SET, 0x01))
    s.recv(4)
    s.sendall(frame(CMD_READ, 0x00))
    r = s.recv(4)
    if r[0] == HDR_TX and r[1] == CMD_READ and (r[2] & 1) == 1:
        ok("GPIO 读取(高)")
    else:
        no("GPIO 读取(高)")

    # Bad checksum
    s.sendall(bytes([HDR_RX, CMD_SET, 0x01, 0xFF]))
    s.settimeout(0.3)
    try: r = s.recv(4); no("错误校验(收到意外响应)")
    except socket.timeout: ok("错误校验(正确丢弃)")

    # Bad header
    s.sendall(bytes([0xBB, CMD_SET, 0x01, 0x00]))
    try: r = s.recv(4); no("错误帧头(收到意外响应)")
    except socket.timeout: ok("错误帧头(正确丢弃)")

    s.close()
    total = _p + _f
    print(f"\n结果: {_p}/{total} 通过" + (f" {RED}{_f} 失败{RST}" if _f else f" {GREEN}全部通过{RST}"))


# ============================================================
#  TCP 客户端 (开机包 + 交互 / 全自动)
# ============================================================
class TCPClient:
    def __init__(self, ip):
        self.ip = ip
        self.sock = None

    def connect(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(5)
        self.sock.connect((self.ip, PORT))

    def recv_boot(self):
        self.sock.settimeout(3)
        data = self.sock.recv(256)
        self.sock.settimeout(2)
        if len(data) < 4 or data[0] != HDR_TX or data[1] != CMD_BOOT:
            no(f"开机包帧头/命令错误")
            return False
        plen = data[2]
        payload = data[3:3+plen].decode('ascii', errors='replace')
        chk_recv = data[3+plen]
        calc = 0
        for b in payload.encode(): calc ^= b
        if chk_recv != calc:
            no(f"开机包校验错: calc=0x{calc:02X} recv=0x{chk_recv:02X}")
            return False
        parts = payload.split(',')
        if len(parts) != 5:
            no(f"开机包字段数错: {len(parts)}")
            return False
        print(f"  固件: {parts[0]}  MAC: {parts[1]}  IP: {parts[2]}  BSSID: {parts[3]}  RSSI: {parts[4]} dBm")
        return True

    def send_frame(self, cmd, data):
        self.sock.sendall(frame(cmd, data))

    def recv_response(self, timeout=2.0):
        self.sock.settimeout(timeout)
        try: return self.sock.recv(FRAME_LEN)
        except socket.timeout: return None

    def check_resp(self, resp, expected_cmd, expected_data=None):
        if resp is None: no("超时"); return False
        if len(resp) != FRAME_LEN: no(f"长度错({len(resp)})"); return False
        h, c, d, chk = resp
        if h != HDR_TX: no(f"帧头错(0x{h:02X})"); return False
        if c != expected_cmd: no(f"CMD错(0x{c:02X})"); return False
        if chk != (c ^ d): no(f"校验错"); return False
        if expected_data is not None and (d & 1) != expected_data: no(f"电平错 期望{expected_data} 实际{d&1}"); return False
        ok(f"GPIO={'HIGH' if d&1 else 'LOW'}")
        return True

    def close(self):
        if self.sock: self.sock.close()


def mode_boot(ip):
    """仅测试开机包"""
    print(f"连接 {ip}:{PORT} ...")
    c = TCPClient(ip)
    c.connect()
    if c.recv_boot(): ok("开机包接收成功")
    else: no("开机包接收失败")
    c.close()

def mode_run(ip):
    """交互式 IO 控制"""
    print(f"连接 {ip}:{PORT} ...")
    c = TCPClient(ip)
    c.connect()
    c.recv_boot()
    print("\n命令: 1=高 0=低 r=读 q=退出\n")
    while True:
        x = input("> ").strip().lower()
        if x == 'q': break
        elif x == '1': c.send_frame(CMD_SET, 0x01); r = c.recv_response(); c.check_resp(r, CMD_SET, 1)
        elif x == '0': c.send_frame(CMD_SET, 0x00); r = c.recv_response(); c.check_resp(r, CMD_SET, 0)
        elif x == 'r': c.send_frame(CMD_READ, 0x00); r = c.recv_response(); c.check_resp(r, CMD_READ)
    c.close()

def mode_full(ip):
    """全自动 12 项测试"""
    global _p, _f
    _p = _f = 0
    print(f"连接 {ip}:{PORT} ...")

    c = TCPClient(ip)
    c.connect()

    # 0: boot packet
    title("[0] 开机包")
    if c.recv_boot(): ok("开机包")
    else: no("开机包")

    def reconnect():
        c.close(); time.sleep(0.3); c.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        c.sock.settimeout(5); c.sock.connect((ip, PORT)); c.recv_boot()

    # 1-3: basic IO
    title("[1] GPIO 置高")
    c.send_frame(CMD_SET, 0x01); c.check_resp(c.recv_response(), CMD_SET, 1)
    title("[2] GPIO 置低")
    c.send_frame(CMD_SET, 0x00); c.check_resp(c.recv_response(), CMD_SET, 0)
    title("[3] 读取 IO")
    c.send_frame(CMD_SET, 0x01); c.recv_response()
    c.send_frame(CMD_READ, 0x00); c.check_resp(c.recv_response(), CMD_READ, 1)

    # 4: bad checksum
    title("[4] 错误校验帧")
    c.sock.sendall(bytes([HDR_RX, CMD_SET, 0x01, 0xFF]))
    r = c.recv_response(1.0)
    ok("正确丢弃") if r is None else no("收到意外响应")

    # 5: bad header
    title("[5] 错误帧头")
    c.sock.sendall(bytes([0xBB, CMD_SET, 0x01, 0x00]))
    r = c.recv_response(1.0)
    ok("正确丢弃") if r is None else no("收到意外响应")

    # 6: unknown CMD
    title("[6] 未知命令")
    c.sock.sendall(bytes([HDR_RX, 0x99, 0x00, 0x99]))
    r = c.recv_response(1.0)
    ok("正确丢弃") if r is None else no("收到意外响应")

    # 7: multi-frame
    title("[7] 一包多帧")
    multi = frame(CMD_SET, 0x01) + frame(CMD_SET, 0x00) + frame(CMD_SET, 0x01)
    c.sock.sendall(multi)
    for i, exp in enumerate([1, 0, 1]):
        print(f"  帧{i+1}:", end=" "); c.check_resp(c.recv_response(), CMD_SET, exp)

    # 8: garbage prefix
    title("[8] 帧前垃圾字节")
    c.sock.sendall(bytes([0x00, 0xFF, 0x55]) + frame(CMD_SET, 0x01))
    c.check_resp(c.recv_response(), CMD_SET, 1)

    # 9: rapid toggle x20
    title("[9] 快速切换 20 次")
    for i in range(20):
        lv = i % 2
        c.send_frame(CMD_SET, lv)
        c.check_resp(c.recv_response(1.0), CMD_SET, lv)

    # 10: batch 100 frames
    title("[10] 批量 100 帧")
    t0 = time.time()
    for i in range(100):
        lv = i % 2
        c.send_frame(CMD_SET, lv)
        r = c.recv_response(0.5)
        if r and len(r)==4 and r[0]==HDR_TX: ok(f"#{i+1}") if i < 3 else None
        else: no(f"#{i+1}")
    fps = 100 / (time.time() - t0)
    info(f"100帧 / {time.time()-t0:.1f}s = {fps:.0f} fps")

    # 11: mixed payload
    title("[11] 混合数据包")
    c.sock.sendall(frame(CMD_SET, 0x01) + bytes([0xFF, 0x00]) + frame(CMD_SET, 0x00))
    for i, exp in enumerate([1, 0]):
        print(f"  帧{i+1}:", end=" "); c.check_resp(c.recv_response(), CMD_SET, exp)

    # 12: reconnect
    title("[12] 断开重连")
    c.close(); time.sleep(0.5)
    c.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    c.sock.settimeout(5); c.sock.connect((ip, PORT)); c.recv_boot()
    c.send_frame(CMD_SET, 0x01); r = c.recv_response()
    if c.check_resp(r, CMD_SET, 1): ok("重连通信正常")
    c.close()

    total = _p + _f
    print(f"\n{CYAN}{'='*50}{RST}")
    print(f"  总计: {total}  通过: {GREEN}{_p}{RST}  失败: {RED}{_f}{RST}" if _f else f"  总计: {total}  通过: {GREEN}{_p}{RST}  失败: 0")
    if _f == 0: print(f"\n  {GREEN}全部通过!{RST}")
    print(f"{CYAN}{'='*50}{RST}")

def mode_stress(ip):
    """循环压测"""
    print(f"连接 {ip}:{PORT} ...")
    c = TCPClient(ip)
    c.connect()
    c.recv_boot()
    i, p, f, t0 = 0, 0, 0, time.time()
    print("持续发送中，Ctrl+C 停止...\n")
    try:
        while True:
            lv = i % 2
            c.send_frame(CMD_SET, lv)
            r = c.recv_response(0.5)
            if r and len(r)==4 and r[0]==HDR_TX: p += 1
            else: f += 1
            i += 1
            if i % 100 == 0:
                fps = i / (time.time() - t0)
                print(f"  [{i:5d}] {fps:.0f} fps | PASS={p} FAIL={f}")
    except KeyboardInterrupt:
        print(f"\n停止。{i}帧/{time.time()-t0:.1f}s = {i/(time.time()-t0):.0f} fps")
    c.close()


# ============================================================
#  入口
# ============================================================
MENU = """
{G}╔══════════════════════════════════╗{X}
{G}║   ESP32-C3 WiFi IO 测试工具     ║{X}
{G}╚══════════════════════════════════╝{X}

  1. sim       本地模拟测试 (无需硬件)
  2. boot      仅测开机包
  3. run       交互式 IO 控制
  4. full      全自动 12 项测试
  5. stress    循环压测

  0. 退出
"""

if __name__ == '__main__':
    if len(sys.argv) >= 2:
        # 命令行模式
        mode, *args = sys.argv[1:]
        if mode == 'sim':     mode_sim()
        elif mode == 'boot' and args:   mode_boot(args[0])
        elif mode == 'run' and args:    mode_run(args[0])
        elif mode == 'full' and args:   mode_full(args[0])
        elif mode == 'stress' and args: mode_stress(args[0])
        else: print(__doc__)
    else:
        # 交互菜单模式
        while True:
            print(MENU.format(G=GREEN, X=RST))
            choice = input("  选择 [1-5/0]: ").strip()
            if choice == '1': mode_sim()
            elif choice == '2':
                ip = input("  ESP32 IP: ").strip()
                if ip: mode_boot(ip)
            elif choice == '3':
                ip = input("  ESP32 IP: ").strip()
                if ip: mode_run(ip)
            elif choice == '4':
                ip = input("  ESP32 IP: ").strip()
                if ip: mode_full(ip)
            elif choice == '5':
                ip = input("  ESP32 IP: ").strip()
                if ip: mode_stress(ip)
            elif choice == '0':
                print("  退出")
                break
            else:
                print(f"  无效选择: {choice}")
            input(f"\n  按回车继续...")
