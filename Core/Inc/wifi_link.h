/*
 * wifi_link.h
 *
 * Audio link between the two walkie-talkie boards over Wi-Fi (UDP).
 *
 * The rest of the project only deals with frames of WIFI_LINK_FRAME_SAMPLES
 * 16-bit samples. Packets, sequence numbers, loss counting and the Wi-Fi
 * module itself stay inside wifi_link.c.
 *
 * Settings (hotspot, password, PEER_IP, UDP_PORT) come from wifi_secrets.h.
 *
 * The link is half-duplex like the walkie-talkie: it is either in RX mode
 * (listening on UDP_PORT) or in TX mode (sending to PEER_IP:UDP_PORT).
 * The Wi-Fi module refuses to have the send and receive sockets open at the
 * same time ("Error binding UDP socket to port 5000"), so switching mode closes
 * one and opens the other. Call wifi_link_set_mode() when the button changes state.
 *
 * Do NOT call these functions from an interrupt: they talk to the Wi-Fi
 * module over SPI and can block for several milliseconds.
 */
#ifndef WIFI_LINK_H
#define WIFI_LINK_H

#include <stdint.h>

#define WIFI_LINK_FRAME_SAMPLES 160 // 20 ms of audio at 8 kHz

typedef enum {
  WIFI_LINK_OK = 0,
  WIFI_LINK_ERR_INIT, // module did not respond (SPI / pins / reset)
  WIFI_LINK_ERR_CONNECT, // could not join the hotspot (name, password, 2.4 GHz?)
  WIFI_LINK_ERR_TX_OPEN, // could not open the sending socket
  WIFI_LINK_ERR_RX_OPEN, // could not start the receiving socket
  WIFI_LINK_ERR_CLOSE // could not close the previous socket when switching mode
} WifiLinkStatus;

typedef enum {
  WIFI_LINK_MODE_RX = 0, // listening for the other board
  WIFI_LINK_MODE_TX // sending to the other board
} WifiLinkMode;

typedef struct {
  uint32_t sent; // frames sent
  uint32_t send_errors; // sends that failed or were incomplete
  uint32_t received; // valid frames received
  uint32_t lost; // frames missing, from gaps in the sequence numbers
  uint32_t bad; // packets of the wrong size or format
  uint32_t restarts; // times the other board restarted (sequence jumped back)
  uint32_t flushed; // old packets thrown away when switching to RX
} WifiLinkStats;

// Start the module, join the hotspot and start in RX mode. Call once at startup.
WifiLinkStatus wifi_link_init(void);

/* Switch between RX and TX (takes a few ms; call on the button press, not every loop).
   Returns WIFI_LINK_OK, or WIFI_LINK_ERR_TX_OPEN / WIFI_LINK_ERR_RX_OPEN. */
WifiLinkStatus wifi_link_set_mode(WifiLinkMode mode);
WifiLinkMode wifi_link_get_mode(void);

// How long the last wifi_link_set_mode() took: closing the old socket, opening the new one (ms).
void wifi_link_get_switch_timing(uint32_t *close_ms, uint32_t *open_ms);

/* TX mode only: send one frame of WIFI_LINK_FRAME_SAMPLES samples.
   Returns 1 on success, 0 on failure (or if not in TX mode). */
int wifi_link_send_frame(const int16_t *samples);

/* RX mode only: wait up to timeout_ms for one frame. Returns 1 and fills samples
   (WIFI_LINK_FRAME_SAMPLES values) if a frame arrived, 0 otherwise.
   Keep timeout_ms small (5-10 ms) so the main loop stays responsive. */
int wifi_link_poll(int16_t *samples, uint32_t timeout_ms);

// RX mode only: throw away packets waiting in the module.
void wifi_link_flush(void);

// Counters for testing and the report.
void wifi_link_get_stats(WifiLinkStats *out);
void wifi_link_reset_stats(void);

// This board's IP address (valid after wifi_link_init).
void wifi_link_get_ip(uint8_t ip[4]);

// The Wi-Fi module's reply to the last command, useful after an error.
const char *wifi_link_last_error(void);

#endif // WIFI_LINK_H
