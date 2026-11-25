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
2. Copy `Core/Inc/wifi_secrets_template.h` to `Core/Inc/wifi_secrets.h` and fill in the
   hotspot name and password, the other board's IP, and the ports for the board you are
   flashing (board A or B, see [Settings](#settings-coreincwifi_secretsh-not-in-git)).
   This file is in `.gitignore` and is never committed.
3. Build and run. Serial output is on the ST-LINK virtual COM port at 115200 8N1.

The project uses STM32Cube FW_L4 **V1.18.2**. When CubeIDE offers to migrate, choose
**Continue** so everyone stays on the same version.

---

## Wi-Fi link

### Quick guide for teammates

Everything Wi-Fi (the module, sockets, packets, sequence numbers, loss counting) is
inside `wifi_link.c`. The rest of the project only deals with
**audio frames**: 160 `int16_t` samples = 20 ms of audio at 8 kHz.

#### 1. Setup

- Both boards join the **same 2.4 GHz WPA2 hotspot** (iPhone: turn on "Maximize Compatibility").
- One board is **A** (`UDP_PORT 5000`, `PEER_PORT 5001`), the other is **B** (`5001` / `5000`).
  Two boards with the same ports cannot hear each other.
- Each board sends to a fixed IP, `PEER_IP`. To set it up: flash both boards, read the
  `Wi-Fi ready, IP ...` line each one prints, put board B's IP in board A's `PEER_IP` and
  board A's IP in board B's, then flash again. The hotspot usually gives a device the same
  IP next time, so this is normally a one-time step; if nothing arrives, check the IPs first.
- The Wi-Fi code does not use any LED; LED2 belongs to the button code.

#### 2. What's in `main.c` and what you plug in

`main.c` already runs the link. One switch at the top (`USER CODE PD`) picks the mode:

```c
#define WIFI_TEST 1   // 1 = Wi-Fi test, 0 = the real walkie-talkie
```

| | `WIFI_TEST 1` (test) | `WIFI_TEST 0` (real) |
|---|---|---|
| What switches RX / TX | a timer, every 5 s | the blue button (`state`) |
| What gets sent | a dummy sound, one frame every 20 ms | `mic_frame`, whenever the mic code says it's ready |
| Serial output | startup, switches, stats every second | startup, switches |

The main loop (`USER CODE 3`) does the same two steps in both modes:

1. If the wanted mode changed, `wifi_link_set_mode()` (and prints `---- TX: talking ----` / `---- RX: listening ----`).
2. In TX, send `mic_frame` when `mic_frame_ready` is set; in RX, `wifi_link_poll()` and pass
   each frame that arrives to `audio_play_frame()`.

**The two hooks for the audio code** (the only things to add for the real walkie-talkie):

```c
// Mic (Brian): fill the frame, then raise the flag. Every 20 ms, e.g. from the DFSDM DMA callbacks.
mic_frame[i] = ...;        // int16_t mic_frame[160], declared in main.c (extern it in other files)
mic_frame_ready = 1;       // the main loop sends it and clears the flag

// Speaker (Nabeeha): write this function (main.c has an empty weak version).
void audio_play_frame(const int16_t *frame)   // called with every frame received, 160 samples
{
  // copy into the DAC buffer
}
```

#### 3. Switching between talking and listening

- The main loop calls `wifi_link_set_mode()` when the wanted mode changes. The button
  interrupt only changes `state` (which is `volatile` so the loop sees the change).
- **To TX** (start talking) takes 3 to 5 ms: the send socket opens while the receive
  socket stays open.
- **To RX** (stop talking) takes 40 to 70 ms: packets the other board sent while you were
  talking are thrown away so you don't play stale audio.
- `wifi_link_send_frame()` does nothing in RX, and `wifi_link_poll()` does nothing in TX.
- If a switch fails (the module refused), the loop prints `switch to TX failed ...` and
  tries again 1 s later.

#### 4. The audio frame (the contract between mic, Wi-Fi and speaker)

| | |
|---|---|
| Samples per frame | `WIFI_LINK_FRAME_SAMPLES` = 160 |
| Format | `int16_t`, signed, silence = 0 |
| Rate | 8 kHz mono, so one frame = 20 ms and 50 frames per second |

- **Mic side (Brian):** DFSDM writes `int32_t` values with the 24-bit result in bits 31..8.
  Shift and clamp them to `int16_t`, put 160 into `mic_frame`, then set `mic_frame_ready = 1`
  (e.g. from the DMA half / complete callbacks). The mic's timing sets the send rate.
- **Speaker side (Nabeeha):** `audio_play_frame()` runs in the main loop, so keep it quick
  (copy the frame into a buffer). Convert each sample for the 12-bit DAC, for example
  `(sample >> 4) + 2048`. Packets arrive in small bursts, so keep 2 or 3 frames in a
  buffer before starting playback, and play silence if the buffer runs dry.

#### 5. Rules and gotchas

- **Never call `wifi_link_*` from an interrupt.** They talk to the module over SPI and
  wait on its interrupts. Interrupts should only set flags or `state`.
- **Calls can block.** A normal send takes a few ms, a failed one up to 100 ms. The mic DMA
  keeps running meanwhile; frames produced during that time are simply not sent.
- **Keep these in `USER CODE 4`**, or the Wi-Fi module stops answering: `SPI3_IRQHandler`
  and the `GPIO_PIN_1` branch of `HAL_GPIO_EXTI_Callback` (it calls `SPI_WIFI_ISR()`).
- **Don't replace `Core/Src/es_wifi.c`**: it has two fixes UDP depends on (see below).
- **Set `WIFI_TEST 0`** for the real walkie-talkie. In test mode the button is ignored
  and a dummy sound is sent instead of the mic.

#### 6. API

| Function | What it does |
|---|---|
| `wifi_link_init()` | Reset the module, join the hotspot, start in RX. Once at startup. |
| `wifi_link_set_mode(mode)` / `wifi_link_get_mode()` | Switch / read RX or TX. |
| `wifi_link_send_frame(samples)` | TX: send one 160-sample frame. Returns 1 if sent. |
| `wifi_link_poll(samples, ms)` | RX: wait up to `ms` for a frame. Returns 1 and fills `samples`. |
| `wifi_link_flush()` | RX: throw away waiting packets (done for you when switching to RX). |
| `wifi_link_get_ip()` / `wifi_link_get_peer_ip()` | This board's IP / `PEER_IP`. |
| `wifi_link_get_stats()` | Counters: sent, send errors, received, lost, bad, restarts, flushed. |
| `wifi_link_last_error()` | The module's reply to the last command, for debugging. |

---

### Hardware and network

- Module: Inventek ISM43362 (on-board), firmware **C3.5.2.7.STM**, driven over SPI3 by
  ST's `es_wifi` driver (`Core/Src/wifi.c`, `es_wifi.c`, `es_wifi_io.c`). 2.4 GHz only.
- Transport: **UDP**. UDP rather than TCP because late audio is useless and
  TCP retransmissions add delay. Loss is detected with sequence numbers instead.

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
| `PEER_IP` | the other board's IP (or the laptop's, for tests), e.g. `{10, 219, 189, 144}` |
| `UDP_PORT` | port this board listens on |
| `PEER_PORT` | port the other side listens on |

The two ports must be **different** (see "Module limits" below), so the boards use opposite pairs:

| | `UDP_PORT` | `PEER_PORT` |
|---|---|---|
| Board A | 5000 | 5001 |
| Board B | 5001 | 5000 |

### Packet format

```c
typedef struct __attribute__((packed)) {
  uint16_t seq;          // packet counter, used to detect lost packets
  uint16_t nsamples;     // samples in this packet (160)
  int16_t  samples[160]; // audio
} AudioPacket;           // 324 bytes, 50 packets/s, about 130 kbit/s
```

### Wi-Fi test (`WIFI_TEST 1`)

The board switches RX / TX every 5 s by itself, sends a dummy sound (a ramp) at the audio
rate of 50 frames/s, and prints one line per second:

```
TX 50/s  sent 1052 (err 0)  rx 1168  lost 0  bad 0  flushed 48
```

rate this second, frames sent (failed sends), frames received, lost (gaps in sequence
numbers), bad (wrong size), and flushed (old packets dropped when switching to RX).
With two boards, flash both with `WIFI_TEST 1`.

Laptop scripts in `tools/` stand in for the second board (run each only once):

```
python tools\udp_listen.py 5001                     # receive from the board (port = board's PEER_PORT)
python tools\udp_send.py <board_ip> [port] [rate]   # send to the board (port = board's UDP_PORT, default 5000)
```

Windows Firewall must allow Python on both Private and Public networks.

### Results (single board + laptop on a phone hotspot)

**Throughput and loss (Nov 20)**

| Test | Direction | Packet size | Rate | Lost | Malformed |
|---|---|---|---|---|---|
| TX | board to laptop | 324 B | 44 pkt/s | 0 | 0 |
| RX | laptop to board | 324 B | 50 pkt/s | 0 (715 received) | 0 |

- RX keeps up with the full audio rate of 50 packets/s.
- TX ran at 44 packets/s because the test loop then waited `HAL_Delay(20)` after each send;
  that was the test's timing, not a link limit. The test now sends one frame every 20 ms,
  like the mic, so it runs at 50.

**Switching between TX and RX** (Wi-Fi test, 5 s per mode)

| Switch | First version | Final |
|---|---|---|
| RX to TX (start talking) | 505 ms | **3 to 5 ms** |
| TX to RX (stop talking) | 3 to 4 ms | 40 to 70 ms |

- The first version closed the receive server before opening the send socket;
  stopping the UDP server alone took ~500 ms in the module. With different send and
  listen ports the module accepts both sockets at once, so the server now stays open
  and switching to TX only opens the send socket.
- Switching to RX takes longer because it throws away the 9 to 14 packets that arrived
  during TX (in this test the laptop never stopped sending). After the flush `lost` stays 0.
  With two boards the other side is quiet while you talk, so there is less to discard.

**Long runs and outages (Nov 24)**

- Clean stretches run at 49 to 51 pkt/s received and 43 to 45 pkt/s sent with 0 loss.
- Losses come in bursts when the hotspot stalls. In one run: 13 of ~1,150 received
  frames lost (1.1%) and 31 of ~1,080 sends failed (2.9%), all within one 3-second window.
- The laptop's lost count matched the board's failed sends exactly: frames that left the
  board arrived. Loss happens at the board's radio, not in the air.
- Some runs had full outages, where the hotspot dropped the board for several seconds.

Still to measure: board to board, round-trip latency, a stress test at 100 pkt/s.

### Known issues

- **Phone hotspots are unreliable.** Keep the phone unlocked, plugged in and close, or
  use a laptop's mobile hotspot (2.4 GHz) for demos.
- **No automatic recovery.** If the hotspot drops a board, sends fail (`err` climbs) and
  nothing arrives. Press the board's reset button to rejoin.
- **Fixed IPs.** If the hotspot gives a board a new IP, update `PEER_IP` on the other board.

### Tried and left out (in the git history)

Two features were built and tested, then removed to keep the code short and easy to explain.
Both can come back if the problems they solve show up during integration.

- **Automatic discovery:** each board broadcast a small HELLO packet with its own IP, so no
  `PEER_IP` was needed. Broadcast is unreliable on Wi-Fi (frames are not acknowledged; 40 to
  50% were lost), so it was only used for these repeated HELLOs, never for audio.
- **Reconnect watchdog:** when sends kept failing (2 s in TX), or a test HELLO failed after
  3 s of silence in RX, it reset the module, rejoined the hotspot and found the other board
  again. It recovered from three outages in testing, but the module's "connected" status
  turned out to be useless for this, since it stays "connected" long after the hotspot is gone.

### Module limits found

- **Two UDP sockets with the same port collide.** With the send socket (to port 5000)
  open, starting the receive server on port 5000 fails with
  `[UDP SVR] Error binding UDP socket to port 5000`. Using different ports
  (`UDP_PORT` / `PEER_PORT`, send socket local port 5002) fixes it.
- **Stopping a UDP server is slow** (~500 ms), so `wifi_link` never stops it during normal use.
- **The "connected" status is unreliable:** it stays "connected" for seconds after the
  hotspot is gone, so failed sends are the only reliable sign that the link is down.

### Fixes to ST's Wi-Fi driver

Two bugs in `Core/Src/es_wifi.c` stopped UDP from working. **Keep these if you
re-copy the driver files.**

1. **UDP client never got a destination.** `ES_WIFI_StartClientConnection` only sent
   the remote port (`P4`) and remote IP (`P3`) for TCP. For UDP the module answered
   `[P6] Invalid IP Address`. Fix: send them for every protocol.
2. **UDP server could not start.** `ES_WIFI_StartServerSingleConn` always sent `P5=11`
   ("multi accept"), which the module only accepts for TCP. Fix: send `P5=1` for UDP.

Added to `wifi.c`: `WIFI_GetLastResponse()` returns the module's reply to the last
command, which is how both bugs were found.
