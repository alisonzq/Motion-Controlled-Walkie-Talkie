"""
Receive packets from the board over UDP and print rate and loss once per second.
Also answers the board's HELLO packets, so a board using PEER_DISCOVERY finds
this laptop as its "other board".

Usage:  python udp_listen.py [port]      (port = the board's PEER_PORT, e.g. 5001)
"""
import socket
import struct
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 5000

HELLO_MAGIC = b"HELO"
HELLO_FMT = "<4s4sHBB"          # magic, ip[4], listen_port, knows_peer, reserved  (12 bytes)

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", port))
print(f"Listening on UDP {port}...")


def my_ip_towards(ip):
    """The laptop's own IP on the network that reaches `ip`."""
    t = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        t.connect((ip, 9))
        return t.getsockname()[0]
    finally:
        t.close()


expected = None
got = lost = 0
t0 = time.time()

while True:
    data, addr = s.recvfrom(2048)

    # HELLO from a board looking for its peer: answer it directly
    if len(data) == struct.calcsize(HELLO_FMT) and data[:4] == HELLO_MAGIC:
        _, ip, board_port, knows, _ = struct.unpack(HELLO_FMT, data)
        board_ip = socket.inet_ntoa(ip)
        reply = struct.pack(HELLO_FMT, HELLO_MAGIC, socket.inet_aton(my_ip_towards(board_ip)), port, 1, 0)
        s.sendto(reply, (board_ip, board_port))
        if not knows:
            print(f"HELLO from board {board_ip} (listens on {board_port}) -> answered")
        continue

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
