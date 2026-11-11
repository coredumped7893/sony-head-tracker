#!/usr/bin/env python3
"""Relay the bridge's OpenTrack UDP output (127.0.0.1:4242) to X-Plane 12's UDP
dataref interface (127.0.0.1:49000) as pilot head yaw, pitch and roll.

Usage: xplane-relay.py [yaw] [pitch] [roll]   (named axes are inverted)
See docs/LINUX.md#x-plane-12.
"""
import socket
import struct
import sys

DREFS = (("yaw", "sim/graphics/view/pilots_head_psi"),
         ("pitch", "sim/graphics/view/pilots_head_the"),
         ("roll", "sim/graphics/view/pilots_head_phi"))

sign = dict.fromkeys(("yaw", "pitch", "roll"), 1.0)
for axis in sys.argv[1:]:
    if axis not in sign:
        sys.exit(f"unknown axis {axis!r}; use yaw, pitch or roll")
    sign[axis] = -1.0

rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.bind(("127.0.0.1", 4242))
tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
inverted = [axis for axis, s in sign.items() if s < 0] or "none"
print(f"Relaying 127.0.0.1:4242 to X-Plane 127.0.0.1:49000 (inverted: {inverted})", flush=True)

while True:
    data = rx.recv(64)
    if len(data) != 48:  # six little-endian doubles: x, y, z, yaw, pitch, roll
        continue
    values = dict(zip(("yaw", "pitch", "roll"), struct.unpack("<6d", data)[3:]))
    for axis, name in DREFS:
        # X-Plane DREF packet: "DREF\0", float32 value, dataref name padded to 500 bytes.
        packet = b"DREF\0" + struct.pack("<f", values[axis] * sign[axis]) + name.encode().ljust(500, b"\0")
        tx.sendto(packet, ("127.0.0.1", 49000))
