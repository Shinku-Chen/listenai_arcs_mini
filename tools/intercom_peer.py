#!/usr/bin/env python3
"""对讲机主机侧 BLE 对端（不需要手机）。

用 bleak 直接讲 `docs/development/intercom-wire-protocol.md` 里那套协议，用来在没有安卓 App
的情况下验证设备侧：发现 → 连接 → 订阅 → 握手 → 文本上下行 → 轮次 → 状态。

用法：
    pip install bleak
    python tools/intercom_peer.py                 # 全流程
    python tools/intercom_peer.py --scan-only     # 只扫描，确认设备在广播
    python tools/intercom_peer.py --address AA:BB:CC:DD:EE:FF

退出码：0 = 全部断言通过；1 = 有断言失败；2 = 找不到设备。
"""

import argparse
import asyncio
import json
import sys

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # 手机 -> 设备（write）
TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # 设备 -> 手机（notify）

MAGIC0, MAGIC1 = 0xA5, 0x5A
T_PCM, T_TEXT, T_CONTROL, T_EVENT, T_OPUS, T_TTS = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06
F_MORE, F_FIRST, F_LAST = 0x01, 0x02, 0x04

NAME_PREFIX = "LING-"

frames = []      # 解出来的 (type, flags, payload)
texts = []       # (role, text, truncated)
controls = []


def pack(ftype, flags, payload=b""):
    return bytes([MAGIC0, MAGIC1, ftype, flags, len(payload) >> 8, len(payload) & 0xFF]) + payload


class Reassembler:
    """与固件 ic_proto.c 同构的接收端：够用即可（不做插入帧头恢复）。"""

    def __init__(self):
        self.buf = b""
        self.merge = None  # [(role, bytes)]

    def feed(self, data: bytes):
        self.buf += data
        while True:
            i = self.buf.find(bytes([MAGIC0, MAGIC1]))
            if i < 0:
                self.buf = self.buf[-1:]
                return
            if i > 0:
                self.buf = self.buf[i:]
            if len(self.buf) < 6:
                return
            ftype, flags = self.buf[2], self.buf[3]
            plen = (self.buf[4] << 8) | self.buf[5]
            if ftype not in (T_PCM, T_TEXT, T_CONTROL, T_EVENT, T_OPUS, T_TTS) or plen > 2048:
                self.buf = self.buf[1:]  # 假帧头，前进一字节
                continue
            if len(self.buf) < 6 + plen:
                return
            payload = self.buf[6:6 + plen]
            self.buf = self.buf[6 + plen:]
            self._dispatch(ftype, flags, payload)

    def _dispatch(self, ftype, flags, payload):
        frames.append((ftype, flags, payload))
        if ftype == T_EVENT:
            try:
                controls.append(json.loads(payload.decode("utf-8")))
            except Exception:
                pass
        elif ftype == T_TEXT:
            role = chr(payload[0]) if payload else "?"
            body = payload[1:]
            if flags & F_FIRST:
                self.merge = [role, bytearray()]
            if self.merge is None:
                texts.append((role, bytes(body), False))
                return
            self.merge[1] += body
            if flags & F_LAST:
                texts.append((self.merge[0], bytes(self.merge[1]), False))
                self.merge = None


async def find_device(address=None, timeout=15.0):
    if address:
        return address, "(given)"
    print(f"扫描 {timeout:.0f}s，找广播名 {NAME_PREFIX}* 或带 NUS 服务的设备…")
    found = {}

    def cb(dev, adv):
        uuids = [str(u).lower() for u in (adv.service_uuids or [])]
        names = " ".join(filter(None, [dev.name, adv.local_name])).upper()
        if SERVICE_UUID in uuids or NAME_PREFIX in names or "ARCS" in names:
            found[dev.address] = (dev.name, adv.local_name, adv.rssi, uuids)

    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    await asyncio.sleep(timeout)
    await scanner.stop()

    if not found:
        return None, None
    for addr, info in found.items():
        print(f"  候选 {addr} rssi={info[2]} name={info[0]!r} local={info[1]!r}")
    addr = list(found)[0]
    return addr, found[addr][0]


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scan-only", action="store_true")
    ap.add_argument("--address", default=None)
    args = ap.parse_args()

    addr, name = await find_device(args.address)
    if addr is None:
        print("FAIL: 没有扫到设备（确认设备已上电、未休眠）")
        return 2
    print(f"命中 {addr} name={name!r}")
    if args.scan_only:
        return 0

    rx = Reassembler()
    ok = True
    hello_evt = asyncio.Event()

    def on_notify(_char, data: bytearray):
        rx.feed(bytes(data))
        for m in controls:
            if m.get("ev") == "hello":
                hello_evt.set()

    async with BleakClient(addr, timeout=20.0) as client:
        print(f"已连接 mtu={getattr(client, 'mtu_size', '?')}")
        await client.start_notify(TX_UUID, on_notify)

        # 1) 设备应主动发 hello
        try:
            await asyncio.wait_for(hello_evt.wait(), timeout=8.0)
        except asyncio.TimeoutError:
            print("FAIL: 8s 内没有收到设备 hello（订阅后应立即发）")
            ok = False

        # 2) 手机侧 hello
        await client.write_gatt_char(RX_UUID, pack(T_CONTROL, 0, json.dumps({
            "cmd": "hello", "proto": 1, "caps": ["text", "pcm"],
            "model": "host-peer", "fw": "0.0.1",
        }).encode()), response=True)
        await asyncio.sleep(1.0)

        # 3) 状态请求
        await client.write_gatt_char(RX_UUID, pack(T_CONTROL, 0, b'{"cmd":"status"}'), response=True)
        await asyncio.sleep(0.8)

        # 4) 网关状态
        await client.write_gatt_char(RX_UUID, pack(T_CONTROL, 0, json.dumps({
            "cmd": "gateway", "state": "ready", "detail": "host peer",
        }).encode()), response=True)

        # 5) 清屏 + 两条单帧文本（用户 / 助手）
        await client.write_gatt_char(RX_UUID, pack(T_CONTROL, 0, b'{"cmd":"clear_display"}'), response=True)
        await client.write_gatt_char(
            RX_UUID, pack(T_TEXT, 0, b"U" + "你好，我是用户".encode()), response=True)
        await client.write_gatt_char(
            RX_UUID, pack(T_TEXT, 0, b"A" + "收到，设备侧文本通道正常".encode()), response=True)

        # 6) 分三帧下发的长文本（FIRST/MORE/LAST）
        parts = ["这是", "一段被拆成三帧的", "助手回复。"]
        for idx, part in enumerate(parts):
            flags = 0
            if idx == 0:
                flags |= F_FIRST
            if idx == len(parts) - 1:
                flags |= F_LAST
            if idx != len(parts) - 1:
                flags |= F_MORE
            await client.write_gatt_char(RX_UUID, pack(T_TEXT, flags, b"A" + part.encode()),
                                         response=True)
            await asyncio.sleep(0.2)

        await asyncio.sleep(1.2)
        await client.stop_notify(TX_UUID)

    # ---------------- 断言 ----------------
    def check(cond, msg):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + msg)
        if not cond:
            ok = False

    evts = [m.get("ev") for m in controls if m.get("ev")]
    print(f"\n收到 {len(frames)} 帧 / {len(controls)} 条事件 / {len(texts)} 条文本")
    print(f"事件: {evts}")

    check("hello" in evts, "设备发过 hello")
    hello = next((m for m in controls if m.get("ev") == "hello"), None)
    if hello:
        print(f"     hello={hello}")
        check(hello.get("proto") == 1, "proto=1")
        check(isinstance(hello.get("caps"), list), "caps 是数组")
        check("tts_opus" not in (hello.get("caps") or []), "caps 如实不含 tts_opus")

    statuses = [m for m in controls if m.get("ev") == "status"]
    check(len(statuses) >= 2, f"至少两条 status（握手 + 主动查询），实际 {len(statuses)}")
    if statuses:
        print(f"     status={statuses[-1]}")
        check(isinstance(statuses[-1].get("battery"), int), "status 带电量")
        check(isinstance(statuses[-1].get("volume"), int), "status 带音量")

    check(len(texts) == 3, f"上屏 3 条文本（单帧 2 + 合并 1），实际 {len(texts)}")
    if len(texts) == 3:
        check(texts[0][0] == "U" and texts[0][1].decode("utf-8") == "你好，我是用户",
              "用户文本原样到达")
        check(texts[1][1].decode("utf-8") == "收到，设备侧文本通道正常", "助手文本原样到达")
        check(texts[2][1].decode("utf-8") == "".join(parts), f"三帧文本合并正确：{texts[2][1].decode('utf-8')}")

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
