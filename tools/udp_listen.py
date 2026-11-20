"""
Receive packets from the board over UDP and print rate and loss once per second.

Usage:  python udp_listen.py [port]
"""
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 5000

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", port))
print(f"Listening on UDP {port}...")

expected = None
got = lost = 0
t0 = time.time()

while True:
    data, addr = s.recvfrom(2048)
    got += 1
    seq = int.from_bytes(data[0:2], "little")       # first 2 bytes = sequence number
    if expected is not None and seq != expected:
        gap = (seq - expected) & 0xFFFF
        if gap > 1000:                              # big jump = board restarted, not real loss
            print(f"board restarted (seq {seq}), resetting count")
            lost = 0
        else:
            lost += gap
    expected = (seq + 1) & 0xFFFF

    if time.time() - t0 >= 1.0:
        print(f"{addr[0]}  {got} pkt/s  {len(data)} bytes each  lost so far: {lost}")
        got = 0
        t0 = time.time()
