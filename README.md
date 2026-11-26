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

