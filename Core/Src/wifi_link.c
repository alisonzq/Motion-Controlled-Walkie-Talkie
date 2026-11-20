/*
 * wifi_link.c
 *
 * Audio link between the two walkie-talkie boards over Wi-Fi (UDP).
 * See wifi_link.h for how to use it.
 *
 * Sockets on the Wi-Fi module (only one is open at a time, see wifi_link_set_mode):
 *   TX mode, socket 0 : UDP client, sends to PEER_IP:UDP_PORT (from local port WIFI_LINK_TX_LOCAL_PORT)
 *   RX mode, socket 1 : UDP server, receives on UDP_PORT
 *
 * Note: es_wifi.c has two local fixes that UDP depends on (remote IP/port for UDP
 * clients, P5=1 for UDP servers). Keep them if the driver files are replaced.
 */
#include "wifi_link.h"
#include "wifi.h"
#include <string.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h" // WIFI_SSID, WIFI_PASSWORD, PEER_IP, UDP_PORT - not in git
#else
#error "Missing Core/Inc/wifi_secrets.h: copy wifi_secrets_template.h to wifi_secrets.h and fill it in."
#endif

#ifndef PEER_IP
#error "PEER_IP missing: add  #define PEER_IP {a, b, c, d}  to Core/Inc/wifi_secrets.h"
#endif
#ifndef UDP_PORT
#define UDP_PORT 5000
#endif

#define WIFI_LINK_TX_SOCKET      0
#define WIFI_LINK_RX_SOCKET      1
#define WIFI_LINK_TX_LOCAL_PORT  5001 // the module rejects local port 0 for UDP
#define WIFI_LINK_SEND_TIMEOUT   100 // ms
#define WIFI_LINK_RESTART_GAP    1000 // a sequence jump this big means the sender restarted
#define WIFI_LINK_FLUSH_MAX      64 // max packets thrown away per flush

// What goes over the air: 4-byte header + audio, 324 bytes in total.
typedef struct __attribute__((packed)) {
  uint16_t seq; // packet counter
  uint16_t nsamples; // samples in this packet
  int16_t  samples[WIFI_LINK_FRAME_SAMPLES]; // audio
} AudioPacket;

static AudioPacket   tx_pkt;
static AudioPacket   rx_pkt;
static WifiLinkStats stats;
static uint16_t      tx_seq;
static int32_t       rx_expected = -1; // next sequence number expected, -1 = none yet
static uint8_t       my_ip[4];
static WifiLinkMode  mode = WIFI_LINK_MODE_RX;
static int           socket_open = 0; // is the socket for the current mode open?

static WifiLinkStatus open_socket(WifiLinkMode m)
{
  if (m == WIFI_LINK_MODE_TX) {
    uint8_t peer_ip[4] = PEER_IP;
    if (WIFI_OpenClientConnection(WIFI_LINK_TX_SOCKET, WIFI_UDP_PROTOCOL, "wl_tx",
                                  peer_ip, UDP_PORT, WIFI_LINK_TX_LOCAL_PORT) != WIFI_STATUS_OK) {
      return WIFI_LINK_ERR_TX_OPEN;
    }
  } else {
    if (WIFI_StartServer(WIFI_LINK_RX_SOCKET, WIFI_UDP_PROTOCOL, 1, "wl_rx", UDP_PORT) != WIFI_STATUS_OK) {
      return WIFI_LINK_ERR_RX_OPEN;
    }
    rx_expected = -1; // new listening session: don't count the gap as loss
  }
  return WIFI_LINK_OK;
}

static WifiLinkStatus close_socket(WifiLinkMode m)
{
  WIFI_Status_t r = (m == WIFI_LINK_MODE_TX) ? WIFI_CloseClientConnection(WIFI_LINK_TX_SOCKET)
                                             : WIFI_StopServer(WIFI_LINK_RX_SOCKET);
  return (r == WIFI_STATUS_OK) ? WIFI_LINK_OK : WIFI_LINK_ERR_CLOSE;
}

WifiLinkStatus wifi_link_init(void)
{
  if (WIFI_Init() != WIFI_STATUS_OK) {
    return WIFI_LINK_ERR_INIT;
  }
  if (WIFI_Connect(WIFI_SSID, WIFI_PASSWORD, WIFI_ECN_WPA2_PSK) != WIFI_STATUS_OK) {
    return WIFI_LINK_ERR_CONNECT;
  }
  WIFI_GetIP_Address(my_ip, sizeof my_ip);

  wifi_link_reset_stats();
  mode = WIFI_LINK_MODE_RX;
  WifiLinkStatus s = open_socket(mode);
  socket_open = (s == WIFI_LINK_OK);
  return s;
}

WifiLinkStatus wifi_link_set_mode(WifiLinkMode new_mode)
{
  if (new_mode == mode && socket_open) {
    return WIFI_LINK_OK; // already there
  }
  if (socket_open) {
    WifiLinkStatus s = close_socket(mode);
    if (s != WIFI_LINK_OK) {
      return s;
    }
    socket_open = 0;
  }
  mode = new_mode;
  WifiLinkStatus s = open_socket(mode);
  socket_open = (s == WIFI_LINK_OK);
  return s;
}

WifiLinkMode wifi_link_get_mode(void)
{
  return mode;
}

int wifi_link_send_frame(const int16_t *samples)
{
  uint16_t sent_len = 0;

  if (mode != WIFI_LINK_MODE_TX || !socket_open) {
    return 0;
  }

  tx_pkt.seq      = tx_seq++;
  tx_pkt.nsamples = WIFI_LINK_FRAME_SAMPLES;
  memcpy(tx_pkt.samples, samples, sizeof tx_pkt.samples);

  if (WIFI_SendData(WIFI_LINK_TX_SOCKET, (uint8_t *)&tx_pkt, sizeof tx_pkt,
                    &sent_len, WIFI_LINK_SEND_TIMEOUT) != WIFI_STATUS_OK
      || sent_len != sizeof tx_pkt) {
    stats.send_errors++;
    return 0;
  }
  stats.sent++;
  return 1;
}

int wifi_link_poll(int16_t *samples, uint32_t timeout_ms)
{
  uint16_t rx_len = 0;

  if (mode != WIFI_LINK_MODE_RX || !socket_open) {
    return 0;
  }
  if (timeout_ms == 0) {
    timeout_ms = 1; // the module needs at least 1 ms
  }

  if (WIFI_ReceiveData(WIFI_LINK_RX_SOCKET, (uint8_t *)&rx_pkt, sizeof rx_pkt,
                       &rx_len, timeout_ms) != WIFI_STATUS_OK || rx_len == 0) {
    return 0; // nothing arrived
  }

  if (rx_len != sizeof rx_pkt || rx_pkt.nsamples != WIFI_LINK_FRAME_SAMPLES) {
    stats.bad++; // not one of our packets
    return 0;
  }

  // Loss detection from the sequence number
  if (rx_expected >= 0 && rx_pkt.seq != (uint16_t)rx_expected) {
    uint16_t gap = (uint16_t)(rx_pkt.seq - (uint16_t)rx_expected);
    if (gap > WIFI_LINK_RESTART_GAP) {
      stats.restarts++; // the other board restarted: not real loss
    } else {
      stats.lost += gap;
    }
  }
  rx_expected = (uint16_t)(rx_pkt.seq + 1);

  stats.received++;
  memcpy(samples, rx_pkt.samples, sizeof rx_pkt.samples);
  return 1;
}

void wifi_link_flush(void)
{
  uint16_t rx_len;

  if (mode != WIFI_LINK_MODE_RX || !socket_open) {
    return;
  }
  for (int i = 0; i < WIFI_LINK_FLUSH_MAX; i++) {
    if (WIFI_ReceiveData(WIFI_LINK_RX_SOCKET, (uint8_t *)&rx_pkt, sizeof rx_pkt,
                         &rx_len, 1) != WIFI_STATUS_OK || rx_len == 0) {
      break; // nothing left
    }
  }
  rx_expected = -1; // don't count the thrown-away packets as lost
}

void wifi_link_get_stats(WifiLinkStats *out)
{
  *out = stats;
}

void wifi_link_reset_stats(void)
{
  memset(&stats, 0, sizeof stats);
}

void wifi_link_get_ip(uint8_t ip[4])
{
  memcpy(ip, my_ip, sizeof my_ip);
}

const char *wifi_link_last_error(void)
{
  return WIFI_GetLastResponse();
}
