"""
Local simulation test - verify protocol logic without ESP32-C3 hardware

Usage:
    python simulate_test.py

Description:
    Two threads simulate ESP32-C3 TCP Server and PC Client,
    communicating via local loopback (127.0.0.1).
"""

import socket
import threading
import time

# ========== Protocol constants ==========
PROTO_HEADER_RX = 0xF1   # receive frame header (PC→ESP32)
PROTO_HEADER_TX = 0xF2   # transmit frame header (ESP32→PC)
CMD_SET_GPIO  = 0x01
CMD_READ_GPIO = 0x02
CMD_BOOT_INFO = 0x10
FRAME_LEN = 4


# ========== Simulated ESP32-C3 ==========
class FakeESP32:
    """Simulate ESP32-C3: TCP Server + protocol parsing + virtual IO state"""

    def __init__(self, port=8080):
        self.port = port
        self.io_level = 0          # simulated GPIO0 level (0/1)
        self.server_sock = None

    def calc_checksum(self, cmd, data):
        return cmd ^ data

    def protocol_process(self, frame):
        """Same logic as ESP32-C3 protocol.c"""
        if len(frame) < FRAME_LEN:
            return None
        if frame[0] != PROTO_HEADER_RX:
            return None

        cmd, data, chk = frame[1], frame[2], frame[3]
        if chk != self.calc_checksum(cmd, data):
            print(f"  [ESP32] Checksum error! calc=0x{cmd ^ data:02X} recv=0x{chk:02X}")
            return None

        if cmd == CMD_SET_GPIO:
            self.io_level = data & 0x01   # bit0 controls IO
            print(f"  [ESP32] Set GPIO0 = {self.io_level} (DATA=0x{data:02X})")
        elif cmd == CMD_READ_GPIO:
            print(f"  [ESP32] Read GPIO0 = {self.io_level}")
        else:
            print(f"  [ESP32] Unknown CMD: 0x{cmd:02X}")
            return None

        resp_data = self.io_level
        return bytes([PROTO_HEADER_TX, cmd, resp_data,
                      self.calc_checksum(cmd, resp_data)])

    def build_boot_packet(self):
        """Build CMD_BOOT_INFO frame: F2 10 <LEN> <PAYLOAD> <CHK>"""
        payload = "1.0.0,AA:BB:CC:DD:EE:FF,192.168.0.200,A4:2B:B0:DC:12:34,-65"
        plen = len(payload)
        chk = 0
        for b in payload.encode():
            chk ^= b
        frame = bytes([PROTO_HEADER_TX, CMD_BOOT_INFO, plen]) + \
                payload.encode() + bytes([chk])
        return frame

    def start(self):
        """Start TCP Server"""
        self.server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server_sock.bind(('127.0.0.1', self.port))
        self.server_sock.listen(1)
        print(f"[ESP32] TCP Server started: 127.0.0.1:{self.port}")

        while True:
            client, addr = self.server_sock.accept()
            print(f"[ESP32] Client connected: {addr}")

            # Send boot packet on connect
            boot = self.build_boot_packet()
            client.sendall(boot)
            print(f"[ESP32] Boot packet sent: {boot.hex(' ').upper()}")

            while True:
                data = client.recv(256)
                if not data:
                    print(f"[ESP32] Client disconnected")
                    break

                print(f"[ESP32] Received {len(data)} bytes: "
                      f"{' '.join(f'{b:02X}' for b in data)}")

                # Parse frames with sliding window
                for i in range(len(data) - FRAME_LEN + 1):
                    resp = self.protocol_process(data[i:i + FRAME_LEN])
                    if resp:
                        client.sendall(resp)
                        print(f"[ESP32] Response: "
                              f"{' '.join(f'{b:02X}' for b in resp)}")

            client.close()


# ========== Simulated PC Client ==========
class FakePC:
    """Simulate PC Client: send protocol frames and verify responses"""

    def __init__(self, port=8080):
        self.port = port
        self.sock = None
        self.pass_count = 0
        self.fail_count = 0

    def connect(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect(('127.0.0.1', self.port))
        print(f"[PC] Connected to ESP32-C3\n")

    def disconnect(self):
        if self.sock:
            self.sock.close()

    def send_frame(self, cmd, data):
        """Send one frame and verify response"""
        chk = cmd ^ data
        frame = bytes([PROTO_HEADER_RX, cmd, data, chk])
        print(f"[PC] Send: {' '.join(f'{b:02X}' for b in frame)}")

        self.sock.sendall(frame)
        resp = self.sock.recv(FRAME_LEN)

        if len(resp) != FRAME_LEN:
            print(f"  [FAIL] Response length error: {len(resp)}")
            self.fail_count += 1
            return

        print(f"[PC] Recv: {' '.join(f'{b:02X}' for b in resp)}")

        # Verify response
        r_header, r_cmd, r_data, r_chk = resp
        errors = []

        if r_header != PROTO_HEADER_TX:
            errors.append(f"Header: expected 0xF2 got 0x{r_header:02X}")
        if r_cmd != cmd:
            errors.append(f"CMD: expected 0x{cmd:02X} got 0x{r_cmd:02X}")
        if r_chk != (r_cmd ^ r_data):
            errors.append(f"Checksum: calc 0x{r_cmd ^ r_data:02X} recv 0x{r_chk:02X}")
        if r_data != data:
            errors.append(f"IO state: expected 0x{data:02X} got 0x{r_data:02X}")

        if errors:
            for e in errors:
                print(f"  [FAIL] {e}")
            self.fail_count += 1
        else:
            level = r_data & 0x01
            print(f"  [PASS] GPIO0 = {'HIGH' if level else 'LOW'}")
            self.pass_count += 1

    def parse_boot_packet(self, data):
        """Parse CMD_BOOT_INFO frame, return dict or None on error"""
        if len(data) < 4:
            return None
        if data[0] != PROTO_HEADER_TX or data[1] != CMD_BOOT_INFO:
            return None
        plen = data[2]
        if len(data) < 3 + plen + 1:
            return None
        payload = data[3:3 + plen].decode('ascii')
        chk = data[3 + plen]
        calc = 0
        for b in payload.encode():
            calc ^= b
        if chk != calc:
            return None
        parts = payload.split(',')
        if len(parts) != 5:
            return None
        return {'version': parts[0], 'mac': parts[1], 'ip': parts[2],
                'bssid': parts[3], 'rssi': int(parts[4])}

    def run_all_tests(self):
        """Run all test cases"""
        print("=" * 50)
        print("  Protocol Test Suite")
        print("=" * 50)

        # Test 0: Boot packet
        print("\n[Test 0] Verify boot packet")
        boot_raw = self.sock.recv(256)
        print(f"[PC] Recv: {boot_raw.hex(' ').upper()}")
        boot = self.parse_boot_packet(boot_raw)
        if boot is None:
            print(f"  [FAIL] Boot packet parse failed")
            self.fail_count += 1
        else:
            ok = all([
                boot['version'] == '1.0.0',
                boot['mac'] == 'AA:BB:CC:DD:EE:FF',
                boot['ip'] == '192.168.0.200',
                boot['bssid'] == 'A4:2B:B0:DC:12:34',
                boot['rssi'] == -65,
            ])
            if ok:
                print(f"  [PASS] Boot={boot}")
                self.pass_count += 1
            else:
                print(f"  [FAIL] Boot content mismatch: {boot}")
                self.fail_count += 1

        # Test 1: Set GPIO HIGH
        print("\n[Test 1] Set GPIO0 HIGH")
        self.send_frame(CMD_SET_GPIO, 0x01)

        # Test 2: Verify IO stays HIGH
        print("\n[Test 2] Verify GPIO0 stays HIGH")
        self.send_frame(CMD_SET_GPIO, 0x01)

        # Test 3: Set GPIO LOW
        print("\n[Test 3] Set GPIO0 LOW")
        self.send_frame(CMD_SET_GPIO, 0x00)

        # Test 4: Bad checksum
        print("\n[Test 4] Frame with bad checksum (should be rejected)")
        bad_frame = bytes([PROTO_HEADER_RX, CMD_SET_GPIO, 0x01, 0xFF])
        print(f"[PC] Send: {' '.join(f'{b:02X}' for b in bad_frame)}")
        self.sock.sendall(bad_frame)
        self.sock.settimeout(0.5)
        try:
            resp = self.sock.recv(FRAME_LEN)
            print(f"  [FAIL] Unexpected response: {' '.join(f'{b:02X}' for b in resp)}")
            self.fail_count += 1
        except socket.timeout:
            print(f"  [PASS] Bad frame correctly ignored, no response")
            self.pass_count += 1
        self.sock.settimeout(None)

        # Test 5: Bad header
        print("\n[Test 5] Frame with bad header (should be rejected)")
        bad_frame = bytes([0xBB, CMD_SET_GPIO, 0x01, 0x00])
        print(f"[PC] Send: {' '.join(f'{b:02X}' for b in bad_frame)}")
        self.sock.sendall(bad_frame)
        self.sock.settimeout(0.5)
        try:
            resp = self.sock.recv(FRAME_LEN)
            print(f"  [FAIL] Unexpected response: {' '.join(f'{b:02X}' for b in resp)}")
            self.fail_count += 1
        except socket.timeout:
            print(f"  [PASS] Bad frame correctly ignored, no response")
            self.pass_count += 1
        self.sock.settimeout(None)

        # Summary
        total = self.pass_count + self.fail_count
        print(f"\n{'=' * 50}")
        print(f"  Results: {self.pass_count}/{total} passed")
        if self.fail_count > 0:
            print(f"  [FAIL] {self.fail_count} test(s) failed!")
        else:
            print(f"  [PASS] All tests passed!")
        print(f"{'=' * 50}")


# ========== Entry point ==========
if __name__ == '__main__':
    # Start simulated ESP32-C3 server (background thread)
    esp32 = FakeESP32()
    server_thread = threading.Thread(target=esp32.start, daemon=True)
    server_thread.start()
    time.sleep(0.2)  # wait for server to be ready

    # Start PC client tests
    pc = FakePC()
    pc.connect()
    pc.run_all_tests()
    pc.disconnect()

    # Verify final IO state
    print(f"\nFinal GPIO0 state: {'HIGH' if esp32.io_level else 'LOW'}")
    assert esp32.io_level == 0, "Final IO state should be LOW"
    print("Final IO state check: [PASS] LOW")
