#!/usr/bin/env python3
"""Generate golden depayloader fixtures from a live HEVC RTP stream on udp/5600.

  tests/files/hevc_capture.bin : repeated [u32 len][len bytes raw RTP packet]
  tests/files/hevc_golden.bin  : repeated [u32 len][len bytes Annex-B access unit]

Run with fpvd/pixelpilot stopped (udp/5600 free) while the air unit streams:
  ./tools/gen_hevc_golden.py [--frames 400] [--golden-only]
The golden step needs python3-gi + gst1.0 plugins (good/bad).
"""
import argparse, os, socket, struct, sys

CAP = "tests/files/hevc_capture.bin"
GOLD = "tests/files/hevc_golden.bin"

def capture(frames):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 5600))
    s.settimeout(5.0)
    pkts, markers = [], 0
    while markers < frames:
        try:
            d = s.recv(65535)
        except socket.timeout:
            print("timeout on udp/5600 (air unit streaming? fpvd stopped?)", file=sys.stderr)
            break
        if len(d) < 12 or (d[0] >> 6) != 2:
            continue
        pkts.append(d)
        if d[1] & 0x80:
            markers += 1
    os.makedirs(os.path.dirname(CAP), exist_ok=True)
    with open(CAP, "wb") as f:
        for p in pkts:
            f.write(struct.pack("<I", len(p))); f.write(p)
    print(f"wrote {CAP}: {len(pkts)} packets, {markers} frames")

def build_golden():
    import gi
    gi.require_version("Gst", "1.0")
    from gi.repository import Gst
    Gst.init(None)
    pipe = Gst.parse_launch(
        "appsrc name=src is-live=false format=time ! "
        "application/x-rtp,media=video,encoding-name=H265,clock-rate=90000,payload=97 ! "
        "rtph265depay ! h265parse config-interval=-1 ! "
        "video/x-h265,stream-format=byte-stream,alignment=au ! "
        "appsink name=sink sync=false")
    src = pipe.get_by_name("src"); sink = pipe.get_by_name("sink")
    pipe.set_state(Gst.State.PLAYING)
    with open(CAP, "rb") as f:
        data = f.read()
    off, ts = 0, 0
    while off + 4 <= len(data):
        (n,) = struct.unpack_from("<I", data, off); off += 4
        pkt = data[off:off+n]; off += n
        buf = Gst.Buffer.new_allocate(None, len(pkt), None)
        buf.fill(0, pkt); buf.pts = ts
        if len(pkt) >= 2 and (pkt[1] & 0x80):
            ts += Gst.SECOND // 60
        src.emit("push-buffer", buf)
    src.emit("end-of-stream")
    aus = []
    while True:
        sample = sink.try_pull_sample(Gst.SECOND)
        if sample is None:
            break
        b = sample.get_buffer()
        ok, info = b.map(Gst.MapFlags.READ)
        if ok:
            aus.append(bytes(info.data)); b.unmap(info)
    pipe.set_state(Gst.State.NULL)
    with open(GOLD, "wb") as f:
        for au in aus:
            f.write(struct.pack("<I", len(au))); f.write(au)
    print(f"wrote {GOLD}: {len(aus)} access units")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=400)
    ap.add_argument("--golden-only", action="store_true")
    a = ap.parse_args()
    if not a.golden_only:
        capture(a.frames)
    build_golden()
