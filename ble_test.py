"""
BLE 蓝牙测试 — 通过 BLE GATT 控制 ESP32 GPIO0

用法:
    python ble_test.py

依赖:
    pip install bleak
"""

import asyncio
import sys
from bleak import BleakScanner, BleakClient

DEVICE_NAME = "XRay_BLE"
RX_CHAR_UUID = "100E0D0C-0B0A-0908-0706-050403020100"
TX_CHAR_UUID = "110E0D0C-0B0A-0908-0706-050403020100"

CHOICES = {
    "1": ("设置高电平", bytes([0xE1, 0x01, 0x01, 0x00])),
    "0": ("设置低电平", bytes([0xE1, 0x01, 0x00, 0x01])),
    "r": ("读取状态",   bytes([0xE1, 0x02, 0x00, 0x02])),
    "x": ("清除 WiFi",  bytes([0xE1, 0x03, 0x00, 0x03])),
}


def notify_handler(sender, data):
    print(f"  \033[92m通知响应:\033[0m {' '.join(f'{b:02X}' for b in data)}")
    if len(data) >= 4:
        level = data[2] & 0x01
        print(f"  \033[92mGPIO0={'高电平' if level else '低电平'}\033[0m")


async def main():
    print(f"搜索 BLE 设备: {DEVICE_NAME} ...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10)

    if not device:
        print(f"\033[91m未找到 {DEVICE_NAME}\033[0m")
        return

    print(f"找到: {device.name} ({device.address})\n")

    async with BleakClient(device) as client:
        print(f"\033[92m已连接\033[0m")
        await client.start_notify(TX_CHAR_UUID, notify_handler)

        print("命令说明:")
        for key, (desc, _) in CHOICES.items():
            print(f"  {key} → {desc}")
        print("  q → 退出")
        print("-" * 40)

        loop = asyncio.get_event_loop()
        while True:
            cmd = await loop.run_in_executor(None, input, "> ")

            if cmd == "q":
                break
            if cmd not in CHOICES:
                print(f"  未知命令: {cmd}")
                continue

            desc, frame = CHOICES[cmd]
            print(f"  发送 {desc}: {' '.join(f'{b:02X}' for b in frame)}")
            await client.write_gatt_char(RX_CHAR_UUID, frame, response=True)
            await asyncio.sleep(0.3)

        await client.stop_notify(TX_CHAR_UUID)

    print("已断开")


if __name__ == "__main__":
    asyncio.run(main())
