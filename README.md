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
- Transport: **UDP**, port 5000. UDP rather than TCP because late audio is useless and
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

### Packet format

One packet carries 20 ms of audio (160 samples at 8 kHz):

```c
typedef struct __attribute__((packed)) {
  uint16_t seq;          // packet counter, used to detect lost packets
  uint16_t nsamples;     // samples in this packet (160)
  int16_t  samples[160]; // audio
} AudioPacket;           // 324 bytes, 50 packets/s, about 130 kbit/s
```

### Test mode

`main.c` has a switch in the `USER CODE PD` section:

```c
#define WIFI_TEST_MODE WIFI_TEST_RX   // or WIFI_TEST_TX
```

- **TX:** the board sends packets to `PEER_IP` (set in `wifi_secrets.h`).
- **RX:** the board listens on `UDP_PORT` and prints rate, total, lost and bad packets once per second.

Laptop scripts in `tools/` stand in for the second board:

```
python tools\udp_listen.py                    # receive from the board (TX test)
python tools\udp_send.py <board_ip> [port] [rate]   # send to the board (RX test)
```

Windows Firewall must allow Python on both Private and Public networks.

### Results (Nov 20, single board + laptop on a phone hotspot)

| Test | Direction | Packet size | Rate | Lost | Malformed |
|---|---|---|---|---|---|
| TX | board to laptop | 324 B | 44 pkt/s | 0 | 0 |
| RX | laptop to board | 324 B | 50 pkt/s | 0 (715 received) | 0 |

- RX keeps up with the full audio rate of 50 packets/s.
- TX ran at 44 packets/s because the test loop waits `HAL_Delay(20)` after each send;
  that is the test's timing, not a link limit. Once the microphone drives the timing
  the rate will be 50.

Still to measure: stress test at 100 pkt/s, a 5 to 10 minute run, and round-trip latency.

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
