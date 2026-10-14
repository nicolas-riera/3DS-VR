#!/usr/bin/env python3
"""Pretends to be a 3DS running 3DS VR, to test the PC side without a console.

It does what the console does on the network: broadcasts HELLO, shows the
pairing code, sends TRACKING at 100 Hz (slowly turning the head), receives and
reassembles VIDEO, counts AUDIO, and saves what it received:
    <out>/stream.h264      H.264 elementary stream (play it with ffplay)
    <out>/frame_NNN.jpg    a few JPEG frames (Old 3DS models)

Standard library only. Example:
    python3 tools/fake_console.py --model new3dsxl --seconds 20
"""
import argparse
import math
import os
import random
import socket
import struct
import time

MAGIC = 0x52564433
VERSION = 1
PORT_STREAMER = 39630
PORT_CONSOLE = 39631

HELLO, PAIR_STATUS, CONFIG, TRACKING, SETTINGS, VIDEO, AUDIO, KEYFRAME_REQ, BYE = range(1, 10)
MODELS = {"3ds": 0, "3dsxl": 1, "new3ds": 2, "2ds": 3, "new3dsxl": 4, "new2dsxl": 5}
CAP_HW_H264, CAP_STEREO = 1, 2

HEADER = struct.Struct("<IBBH")
SETTINGS_S = struct.Struct("<BBHffHH")  # stereo, fps, kbps, tanX, tanY, heightCm, reserved
HELLO_S = struct.Struct("<QQBB3sB32s")
PAIR_S = struct.Struct("<BBH32s")
CONFIG_S = struct.Struct("<IHBBHHBBHff32s")
TRACKING_S = struct.Struct("<Iffff" + "HBBhhhh" + "BBBBIQIII")
VIDEO_S = struct.Struct("<IHHHBBIQffff")
AUDIO_S = struct.Struct("<IHhhBB")


def header(t):
    return HEADER.pack(MAGIC, VERSION, t, 0)


def quat_yaw(angle):
    return (math.cos(angle / 2), 0.0, math.sin(angle / 2), 0.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pc", default="255.255.255.255", help="PC address (default: broadcast)")
    ap.add_argument("--model", default="new3dsxl", choices=MODELS)
    ap.add_argument("--stereo", type=int, default=1)
    ap.add_argument("--fps", type=int, default=0, help="default: 60 (New 3DS) or 30")
    ap.add_argument("--kbps", type=int, default=3500)
    ap.add_argument("--fov", type=float, default=80.0)
    ap.add_argument("--height", type=int, default=175, help="player height in cm")
    ap.add_argument("--seconds", type=float, default=20.0, help="streaming time before leaving")
    ap.add_argument("--out", default="fake_console_out")
    ap.add_argument("--name", default="FakeConsole")
    args = ap.parse_args()

    model = MODELS[args.model]
    is_new = model in (2, 4, 5)
    has_3d = model not in (3, 5)
    caps = (CAP_HW_H264 if is_new else 0) | (CAP_STEREO if has_3d else 0)
    fps = args.fps or (60 if is_new else 30)
    os.makedirs(args.out, exist_ok=True)

    ident = os.path.join(args.out, "identity.txt")
    if os.path.exists(ident):
        cid, token = (int(x, 16) for x in open(ident).read().split())
    else:
        cid, token = random.getrandbits(64), random.getrandbits(64) | 1
        open(ident, "w").write("%016x %016x\n" % (cid, token))

    tan_x = math.tan(math.radians(args.fov) / 2)
    settings = SETTINGS_S.pack(1 if (args.stereo and has_3d) else 0, fps, args.kbps, tan_x, tan_x * 0.6, args.height, 0)
    hello = header(HELLO) + HELLO_S.pack(cid, token, model, caps, bytes([1, 0, 0]), 0,
                                         args.name.encode()[:31].ljust(32, b"\0")) + settings

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    sock.bind(("0.0.0.0", PORT_CONSOLE))
    sock.settimeout(0.005)

    pc = None
    config = None
    frames = {}  # frame id -> [received fragments, data, size, key]
    stats = dict(frames=0, keys=0, bytes=0, audio=0, lost=0, gaps=0)
    last_id = None
    stream = open(os.path.join(args.out, "stream.h264"), "wb")
    jpegs = 0
    t0 = time.time()
    stream_start = None
    last_hello = last_track = last_settings = 0.0
    seq = 0
    print("Searching for the PC...")

    while True:
        now = time.time()
        if stream_start and now - stream_start > args.seconds:
            break
        if not config and now - t0 > 120:
            print("No PC answered within 2 minutes")
            break

        if not config and now - last_hello > 0.5:
            last_hello = now
            sock.sendto(hello, (args.pc, PORT_STREAMER))
        if config and now - last_track > 0.01:
            last_track = now
            q = quat_yaw(0.4 * math.sin((now - stream_start) * 0.5))
            pad = (0, 0, 0, 0, 0, 0, 0)
            pkt = header(TRACKING) + TRACKING_S.pack(seq, *q, *pad, 255, 80, 0, 3, 0, 0, 0, stats["frames"], 0)
            seq += 1
            sock.sendto(pkt, pc)
        if config and now - last_settings > 1.0:
            last_settings = now
            sock.sendto(header(SETTINGS) + struct.pack("<I", seq) + settings, pc)

        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            continue
        except ConnectionResetError:
            continue
        if len(data) < 8:
            continue
        magic, ver, ptype, _ = HEADER.unpack_from(data)
        if magic != MAGIC or ver != VERSION:
            continue
        body = data[8:]

        if ptype == PAIR_STATUS:
            state, _, code, name = PAIR_S.unpack_from(body)
            if state == 1:
                print("Pairing code %04u: click Pair in the 3DS VR app on %s" % (code, name.rstrip(b"\0").decode()))
            else:
                print("Pairing refused")
        elif ptype == CONFIG:
            c = CONFIG_S.unpack_from(body)
            cfg = dict(session=c[0], id=c[1], codec=c[2], stereo=c[3], w=c[4], h=c[5], fps=c[6], kbps=c[8],
                       tx=c[9], ty=c[10], pc=c[11].rstrip(b"\0").decode())
            if not config or cfg["id"] != config["id"]:
                print("CONFIG %s" % cfg)
                frames.clear()
                last_id = None
            if not config:
                stream_start = time.time()
                sock.sendto(header(KEYFRAME_REQ) + struct.pack("<H", cfg["id"]), addr)
            config, pc = cfg, (addr[0], PORT_STREAMER)
        elif ptype == VIDEO and config:
            v = VIDEO_S.unpack_from(body)
            fid, cfg_id, idx, count, flags, _, size = v[:7]
            if cfg_id != config["id"]:
                continue
            payload = body[VIDEO_S.size:]
            f = frames.setdefault(fid, [set(), bytearray(size), size, flags & 1])
            if idx not in f[0]:
                off = idx * (1400 - 8 - VIDEO_S.size)
                f[1][off:off + len(payload)] = payload
                f[0].add(idx)
            if len(f[0]) == count:
                del frames[fid]
                for old in [k for k in frames if k < fid]:
                    del frames[old]
                    stats["lost"] += 1
                if last_id is not None and fid != last_id + 1:
                    stats["gaps"] += 1
                last_id = fid
                stats["frames"] += 1
                stats["keys"] += f[3]
                stats["bytes"] += size
                if config["codec"] == 1:
                    stream.write(f[1])
                elif jpegs < 5:
                    open(os.path.join(args.out, "frame_%03d.jpg" % jpegs), "wb").write(f[1])
                    jpegs += 1
        elif ptype == AUDIO:
            stats["audio"] += 1
        elif ptype == BYE:
            print("The PC ended the session")
            break

    if pc:
        sock.sendto(header(BYE), pc)
    stream.close()
    dur = max(time.time() - (stream_start or time.time()), 1e-3)
    print("Received %d frames (%d key) in %.1f s: %.1f fps, %.2f Mbps; %d incomplete, %d gaps; %d audio packets"
          % (stats["frames"], stats["keys"], dur, stats["frames"] / dur, stats["bytes"] * 8 / dur / 1e6, stats["lost"],
             stats["gaps"], stats["audio"]))
    print("Output in", os.path.abspath(args.out))


if __name__ == "__main__":
    main()
