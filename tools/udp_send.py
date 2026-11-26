"""
Send fake audio packets to the board over UDP (laptop plays the "other board").

Packet format matches AudioPacket in main.c:
    uint16 seq | uint16 nsamples | int16 samples[160]   (324 bytes, little-endian)

Usage:  python udp_send.py <board_ip> [port] [packets_per_second]
        python udp_send.py 10.219.189.22
"""
import socket
import struct
import sys
import time

board_ip = sys.argv[1] if len(sys.argv) > 1 else "10.219.189.22"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 5000
rate = float(sys.argv[3]) if len(sys.argv) > 3 else 50.0   # 50 pkt/s = 20 ms of audio each

SAMPLES = 160
samples = struct.pack("<160h", *[(i * 100) for i in range(SAMPLES)])   # dummy ramp

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
period = 1.0 / rate
seq = 0
next_t = time.perf_counter()
last_print = time.time()
print(f"Sending {rate:.0f} pkt/s to {board_ip}:{port}  (Ctrl+C to stop)")

while True:
    s.sendto(struct.pack("<HH", seq, SAMPLES) + samples, (board_ip, port))
    seq = (seq + 1) & 0xFFFF

    if time.time() - last_print >= 5:
        print(f"sent {seq} packets")
        last_print = time.time()

    next_t += period                       # fixed schedule, so the rate doesn't drift
    delay = next_t - time.perf_counter()
    if delay > 0:
        time.sleep(delay)
