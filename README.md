# Walkie-Talkie (ECSE 444)

Two-board, half-duplex walkie-talkie on the STM32 B-L4S5I-IOT01A Discovery kit.
Voice is sampled with the on-board DFSDM microphone, sent between boards as UDP
packets over a local Wi-Fi network, and played back through the DAC.

| Part | Owner |
|---|---|
| DAC playback, audio buffering, volume, integration testing | Nabeeha |
| Wi-Fi connection, packet TX/RX, communication testing | Alison |
| DFSDM microphone, audio formatting, push-button / LED | Brian |
| I2C accelerometer / gyroscope, Kalman filter, tilt | Muntasir |

## Getting started

1. Clone the repo and import it into STM32CubeIDE 1.19
   (File > Import > General > Existing Projects into Workspace).
2. Copy `Core/Inc/wifi_secrets_template.h` to `Core/Inc/wifi_secrets.h` and fill in
   your hotspot name, password and target IP. This file is in `.gitignore` and is
   never committed.
3. Build and run. Serial output is on the ST-LINK virtual COM port at 115200 8N1.

The project uses STM32Cube FW_L4 **V1.18.2**. When CubeIDE offers to migrate, choose
**Continue** so everyone stays on the same version.

---

## Wi-Fi link

### Setup

- Module: Inventek ISM43362 (on-board), firmware **C3.5.2.7.STM**, driven over SPI3 by
  ST's `es_wifi` driver (`Core/Src/wifi.c`, `es_wifi.c`, `es_wifi_io.c`).
- Network: a phone hotspot, **2.4 GHz, WPA2** (iPhone: turn on "Maximize Compatibility").
  Both boards (or the board and a laptop) must be on the same hotspot.
- Transport: **UDP**. UDP rather than TCP because late audio is useless and
  TCP retransmissions add delay.

| Pin | Signal |
|---|---|
| PC10 / PC11 / PC12 | SPI3 SCK / MISO / MOSI |
| PE0 | Wi-Fi chip select (output, starts high) |
| PE1 | Wi-Fi data ready (EXTI1, rising edge) |
| PE8 | Wi-Fi reset |
| PB12 | Wi-Fi BOOT0 (output, low) |
| PB13 | Wi-Fi wakeup |
| PD13 | Bluetooth chip select, held high so it stays off the shared SPI3 bus |

### Settings (`Core/Inc/wifi_secrets.h`, not in git)

| Setting | Meaning |
|---|---|
| `WIFI_SSID`, `WIFI_PASSWORD` | the hotspot |
| `PEER_IP` | the other board's IP (or the laptop's, for tests) |
| `UDP_PORT` | port this board listens on |
| `PEER_PORT` | port the other side listens on |

The two ports must be **different** (see "Module limits" below), so the boards use opposite pairs:

| | `UDP_PORT` | `PEER_PORT` |
|---|---|---|
| Board A | 5000 | 5001 |
| Board B | 5001 | 5000 |

Each board prints its IP at startup. The hotspot can hand out new IPs each session,
so check `PEER_IP` before testing.

### Using `wifi_link` (`Core/Inc/wifi_link.h`)

Everything Wi-Fi-specific (packets, sequence numbers, sockets, loss counting) is
inside `wifi_link.c`. The rest of the project only handles frames of
`WIFI_LINK_FRAME_SAMPLES` (160) `int16_t` samples, i.e. 20 ms of audio at 8 kHz.

```c
wifi_link_init();                          // join hotspot, start in RX mode
wifi_link_set_mode(WIFI_LINK_MODE_TX);     // on button press (talk)
wifi_link_send_frame(mic_frame);           // TX mode: one 160-sample frame per call
wifi_link_set_mode(WIFI_LINK_MODE_RX);     // on button release (listen)
if (wifi_link_poll(frame, 10)) { ... }     // RX mode: 1 = a frame was received into frame
```

The link is **half-duplex**, like the walkie-talkie: it is either sending or listening.

- Do not call `wifi_link_*` from an interrupt (they talk to the module over SPI and block for a few ms).
- Keep the `wifi_link_poll` timeout small (5 to 10 ms) so the main loop stays responsive.
- `wifi_link_get_stats()` gives sent / errors / received / lost / bad / flushed counters.

### Packet format

One packet carries 20 ms of audio (160 samples at 8 kHz):

```c
typedef struct __attribute__((packed)) {
  uint16_t seq;          // packet counter, used to detect lost packets
  uint16_t nsamples;     // samples in this packet (160)
  int16_t  samples[160]; // audio
} AudioPacket;           // 324 bytes, 50 packets/s, about 130 kbit/s
```

### Test modes

`main.c` has a switch in the `USER CODE PD` section:

```c
#define WIFI_TEST_MODE WIFI_TEST_SWITCH   // or WIFI_TEST_TX / WIFI_TEST_RX
```

- **TX:** the board sends dummy frames to `PEER_IP:PEER_PORT`.
- **RX:** the board listens on `UDP_PORT`.
- **SWITCH:** the board alternates RX and TX every 5 s and prints how long each switch took.

All modes print one line per second: rate, sent, send errors, received, lost, bad, flushed.

Laptop scripts in `tools/` stand in for the second board (run each only once):

```
python tools\udp_listen.py 5001                     # receive from the board (port = board's PEER_PORT)
python tools\udp_send.py <board_ip> [port] [rate]   # send to the board (port = board's UDP_PORT, default 5000)
```

Windows Firewall must allow Python on both Private and Public networks.

### Results (Nov 20, single board + laptop on a phone hotspot)

**Throughput and loss**

| Test | Direction | Packet size | Rate | Lost | Malformed |
|---|---|---|---|---|---|
| TX | board to laptop | 324 B | 44 pkt/s | 0 | 0 |
| RX | laptop to board | 324 B | 50 pkt/s | 0 (715 received) | 0 |

- RX keeps up with the full audio rate of 50 packets/s.
- TX ran at 44 packets/s because the test loop waits `HAL_Delay(20)` after each send;
  that is the test's timing, not a link limit. Once the microphone drives the timing
  the rate will be 50.

**Switching between TX and RX** (SWITCH test, 5 s per mode)

| Switch | First version | Final |
|---|---|---|
| RX to TX (press to talk) | 505 ms | **3 to 4 ms** |
| TX to RX (release) | 3 to 4 ms | 40 to 55 ms |

- The first version closed the receive server before opening the send socket;
  stopping the UDP server alone took ~500 ms in the module. With different send and
  listen ports the module accepts both sockets at once, so the server now stays open
  and switching to TX only opens the send socket.
- Switching to RX takes longer because it discards packets that arrived during TX
  (in this test the laptop never stopped sending). With two boards the other side is
  quiet while you talk, so there is little to discard.
- Send errors: 3 in about 2,200 packets; losses only around mode switches.

Still to measure: the full-drain flush (`flushed` counter, `lost` should stay 0 after
a switch), a stress test at 100 pkt/s, a 5 to 10 minute run, round-trip latency, and
board to board.

### Module limits found

- **Two UDP sockets with the same port collide.** With the send socket (to port 5000)
  open, starting the receive server on port 5000 fails with
  `[UDP SVR] Error binding UDP socket to port 5000`. Using different ports
  (`UDP_PORT` / `PEER_PORT`, send socket local port 5002) fixes it.
- **Stopping a UDP server is slow** (~500 ms), so `wifi_link` never stops it during normal use.

### Fixes to ST's Wi-Fi driver

Two bugs in `Core/Src/es_wifi.c` stopped UDP from working. **Keep these if you
re-copy the driver files.**

1. **UDP client never got a destination.** `ES_WIFI_StartClientConnection` only sent
   the remote port (`P4`) and remote IP (`P3`) for TCP. For UDP the module answered
   `[P6] Invalid IP Address`. Fix: send them for every protocol.
2. **UDP server could not start.** `ES_WIFI_StartServerSingleConn` always sent `P5=11`
   ("multi accept"), which the module only accepts for TCP. Fix: send `P5=1` for UDP.

Added for debugging: `WIFI_GetLastResponse()` in `wifi.c` returns the module's reply to
the last command, which is how both bugs were found.
