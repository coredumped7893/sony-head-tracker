#!/usr/bin/env python3
"""Add a joystick hat to the bridge's head tracking, so the hat pans the view in
games that ignore their own view keys while head tracking is active (DCS).

Reads OpenTrack UDP from the bridge (default 127.0.0.1:4240), adds the hat's
yaw/pitch offset and forwards it (default 127.0.0.1:4242). See docs/LINUX.md#hat-panning.

  hat-relay.py --device /dev/input/by-id/...-event-joystick --learn
  hat-relay.py --device ... --up N --down N --left N --right N [--reset N]
"""
import argparse
import os
import select
import socket
import struct
import time

EVENT = struct.Struct("qqHHi")  # struct input_event on 64-bit Linux
EV_KEY, EV_ABS = 1, 3
ABS_HAT0X, ABS_HAT3Y = 0x10, 0x17
HAT_CODE = 1000  # hat axes become button codes 1000+ (two per axis: negative, positive)


def presses(fd):
    """Yield (code, value) for the pending button events, with hat axes as buttons."""
    try:
        data = os.read(fd, EVENT.size * 64)
    except BlockingIOError:
        return
    for i in range(0, len(data) - EVENT.size + 1, EVENT.size):
        _, _, kind, code, value = EVENT.unpack_from(data, i)
        if kind == EV_KEY:
            yield code, value
        elif kind == EV_ABS and ABS_HAT0X <= code <= ABS_HAT3Y:
            negative = HAT_CODE + 2 * (code - ABS_HAT0X)
            yield negative, int(value < 0)
            yield negative + 1, int(value > 0)


def learn(fd):
    codes = {}
    for direction in ("up", "down", "left", "right", "reset"):
        optional = direction == "reset"
        print(f"Press {direction.upper()}" + (" (or wait 5 s to skip)" if optional else ""), flush=True)
        deadline = time.time() + 5
        while direction not in codes and (not optional or time.time() < deadline):
            select.select([fd], [], [], 0.5)
            for code, value in presses(fd):
                if value == 1 and code not in codes.values() and direction not in codes:
                    codes[direction] = code
        time.sleep(0.3)
        list(presses(fd))  # drop the release events
    print("Run with: " + " ".join(f"--{d} {c}" for d, c in codes.items()))


def relay(fd, args):
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", args.listen))
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    held = {args.up: False, args.down: False, args.left: False, args.right: False}
    yaw = pitch = 0.0
    last = time.monotonic()
    print(f"Relaying 127.0.0.1:{args.listen} to 127.0.0.1:{args.send}, hat pans at {args.rate} deg/s", flush=True)
    while True:
        ready = select.select([rx, fd], [], [])[0]
        if fd in ready:
            for code, value in presses(fd):
                if code in held:
                    held[code] = value != 0
                elif code == args.reset and value == 1:
                    yaw = pitch = 0.0
        if rx not in ready:
            continue
        data = rx.recv(64)
        now = time.monotonic()
        step = args.rate * min(now - last, 0.1)
        last = now
        yaw = max(-180.0, min(180.0, yaw + step * (held[args.right] - held[args.left])))
        pitch = max(-90.0, min(90.0, pitch + step * (held[args.up] - held[args.down])))
        if len(data) != 48:  # six little-endian doubles: x, y, z, yaw, pitch, roll
            continue
        pose = list(struct.unpack("<6d", data))
        pose[3] += yaw
        pose[4] += pitch
        tx.sendto(struct.pack("<6d", *pose), ("127.0.0.1", args.send))


parser = argparse.ArgumentParser(description="Pan head tracking with a joystick hat.")
parser.add_argument("--device", required=True, help="joystick event device, e.g. /dev/input/by-id/...-event-joystick")
parser.add_argument("--learn", action="store_true", help="press each hat direction and print the codes")
for name in ("up", "down", "left", "right", "reset"):
    parser.add_argument(f"--{name}", type=int, default=-1, help=f"button code for {name}")
parser.add_argument("--rate", type=float, default=60.0, help="pan speed in degrees per second (default 60)")
parser.add_argument("--listen", type=int, default=4240, help="UDP port the bridge sends to (default 4240)")
parser.add_argument("--send", type=int, default=4242, help="UDP port OpenTrack listens on (default 4242)")
args = parser.parse_args()

device = os.open(args.device, os.O_RDONLY | os.O_NONBLOCK)
if args.learn:
    learn(device)
elif min(args.up, args.down, args.left, args.right) < 0:
    parser.error("give --up, --down, --left and --right, or run --learn first")
else:
    relay(device, args)
