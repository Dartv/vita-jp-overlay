#!/usr/bin/env python3
"""Prints log lines sent by Vita JP Overlay (config.ini: log_host = <this
computer's IP>). Usage: tools/udp_log_listener.py [--port 9999] [--out FILE]"""
import argparse, datetime, socket

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=9999)
ap.add_argument("--out", help="also append to this file")
args = ap.parse_args()

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", args.port))
print(f"listening on UDP :{args.port}")
out = open(args.out, "a", encoding="utf-8") if args.out else None
while True:
    data, (ip, _) = s.recvfrom(4096)
    stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
    for line in data.decode("utf-8", "replace").splitlines():
        msg = f"{stamp} {ip} {line}"
        print(msg, flush=True)
        if out:
            out.write(msg + "\n")
            out.flush()
