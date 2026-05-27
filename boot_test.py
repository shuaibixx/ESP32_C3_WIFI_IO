"""
Boot packet test - Connect to ESP32-C3 and receive boot notification

Usage: python boot_test.py <ESP32_IP>
Example: python boot_test.py 192.168.0.200
"""

import sys
import socket

PROTO_HEADER_TX = 0xF2
CMD_BOOT_INFO  = 0x10

def parse_boot(data):
    """Parse CMD_BOOT_INFO frame"""
    if len(data) < 4:
        return None, "frame too short"
    if data[0] != PROTO_HEADER_TX or data[1] != CMD_BOOT_INFO:
        return None, f"bad header/cmd: {data[0]:02X} {data[1]:02X}"
    plen = data[2]
    if len(data) < 3 + plen + 1:
        return None, f"incomplete payload"
    payload = data[3:3 + plen].decode('ascii', errors='replace')
    chk = data[3 + plen]
    calc = 0
    for b in payload.encode():
        calc ^= b
    if chk != calc:
        return None, f"checksum fail: calc=0x{calc:02X} recv=0x{chk:02X}"
    parts = payload.split(',')
    if len(parts) != 5:
        return None, f"expected 5 fields, got {len(parts)}: {payload}"
    return {
        'version': parts[0], 'mac': parts[1], 'ip': parts[2],
        'bssid': parts[3], 'rssi': parts[4]
    }, None

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python boot_test.py <ESP32_IP>")
        sys.exit(1)

    ip = sys.argv[1]
    print(f"Connecting to {ip}:8080 ...")

    s = socket.socket()
    s.settimeout(5)
    try:
        s.connect((ip, 8080))
    except Exception as e:
        print(f"[FAIL] Connection failed: {e}")
        sys.exit(1)

    print("Connected! Waiting for boot packet...\n")

    try:
        data = s.recv(256)
    except socket.timeout:
        print("[FAIL] No boot packet received (timeout)")
        s.close()
        sys.exit(1)

    print(f"Raw ({len(data)} bytes): {data.hex(' ').upper()}\n")

    boot, err = parse_boot(data)
    if err:
        print(f"[FAIL] Parse error: {err}")
    else:
        print("=" * 50)
        print("  ESP32-C3 Boot Packet")
        print("=" * 50)
        print(f"  Firmware Version : {boot['version']}")
        print(f"  Device MAC       : {boot['mac']}")
        print(f"  Device IP        : {boot['ip']}")
        print(f"  AP BSSID         : {boot['bssid']}")
        print(f"  AP RSSI          : {boot['rssi']} dBm")
        print("=" * 50)
        print("\n[PASS] Boot packet received successfully!")

    s.close()
