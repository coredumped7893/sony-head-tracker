#!/usr/bin/env python3
"""Relay the bridge's OpenTrack UDP output (127.0.0.1:4242) to X-Plane 12's UDP
dataref interface (127.0.0.1:49000) as pilot head yaw, pitch and roll.

Usage: xplane-relay.py [--gain G] [yaw] [pitch] [roll]   (named axes are inverted)
See docs/LINUX.md#x-plane-12.
"""
import argparse
import socket
import struct

DREFS = (("yaw", "sim/graphics/view/pilots_head_psi"),
         ("pitch", "sim/graphics/view/pilots_head_the"),
         ("roll", "sim/graphics/view/pilots_head_phi"))

parser = argparse.ArgumentParser(description="Relay sony-head-tracker to X-Plane 12.")
parser.add_argument("--gain", type=float, default=1.0,
                    help="multiplies every angle; below 1 turns the view less than your head (default 1.0)")
parser.add_argument("invert", nargs="*", choices=("yaw", "pitch", "roll"), help="axes to invert")
args = parser.parse_args()
scale = {axis: -args.gain if axis in args.invert else args.gain for axis in ("yaw", "pitch", "roll")}

rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.bind(("127.0.0.1", 4242))
tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
print(f"Relaying 127.0.0.1:4242 to X-Plane 127.0.0.1:49000 "
      f"(gain: {args.gain}, inverted: {args.invert or 'none'})", flush=True)

while True:
    data = rx.recv(64)
    if len(data) != 48:  # six little-endian doubles: x, y, z, yaw, pitch, roll
        continue
    values = dict(zip(("yaw", "pitch", "roll"), struct.unpack("<6d", data)[3:]))
    for axis, name in DREFS:
        # X-Plane DREF packet: "DREF\0", float32 value, dataref name padded to 500 bytes.
        packet = b"DREF\0" + struct.pack("<f", values[axis] * scale[axis]) + name.encode().ljust(500, b"\0")
        tx.sendto(packet, ("127.0.0.1", 49000))
