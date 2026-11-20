/*
 * wifi_link.c
 *
 * Audio link between the two walkie-talkie boards over Wi-Fi (UDP).
 * See wifi_link.h for how to use it.
 *
 * Sockets on the Wi-Fi module:
 *   socket 0 : UDP client, sends to the peer (PEER_PORT), from local port WIFI_LINK_TX_LOCAL_PORT
 *   socket 1 : UDP server, receives on UDP_PORT
 *
 * Finding the other board (PEER_DISCOVERY): both boards broadcast a small HELLO packet
 * that contains their own IP. Whoever hears a HELLO learns the peer's IP and from then
 * on sends to it directly. Broadcast is only used for these few HELLOs, because
 * broadcast packets are not acknowledged by Wi-Fi and about half of them get lost,
 * which is fine for a HELLO that is repeated but not for audio.
 *
 * Stopping the UDP server takes ~500 ms in the module, which would delay every
 * switch to TX. So when switching to TX we first try to open the client while the
 * server stays open (packets that arrive meanwhile are flushed on the way back to RX).
 * If the module refuses that, we fall back to stopping the server first.
 *
 * Watchdog: phone hotspots sometimes stall or drop a client (screen lock, power saving).
 * The module keeps saying "connected" for a long time after the hotspot is gone, so the
 * only reliable sign is that sends fail. In TX that shows on the audio itself; in RX the
 * watchdog sends a HELLO after a few seconds of silence to test the link. When sends keep
 * failing it resets the module, rejoins, finds the peer again and restores the mode.
 *
 * Note: es_wifi.c has two local fixes that UDP depends on (remote IP/port for UDP
 * clients, P5=1 for UDP servers). Keep them if the driver files are replaced.
 */
#include "wifi_link.h"
#include "wifi.h"
#include "main.h" // HAL_GetTick
#include <string.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h" // WIFI_SSID, WIFI_PASSWORD, PEER_DISCOVERY / PEER_IP, UDP_PORT, PEER_PORT - not in git
#else
#error "Missing Core/Inc/wifi_secrets.h: copy wifi_secrets_template.h to wifi_secrets.h and fill it in."
#endif

#if !defined(PEER_IP) && !defined(PEER_DISCOVERY)
#error "Add  #define PEER_DISCOVERY 1  (or  #define PEER_IP {a, b, c, d})  to Core/Inc/wifi_secrets.h"
#endif
#ifndef UDP_PORT
#define UDP_PORT 5000 // port this board listens on
#endif
#ifndef PEER_PORT
#define PEER_PORT UDP_PORT // port the other side listens on
#endif

#define WIFI_LINK_TX_SOCKET      0
#define WIFI_LINK_RX_SOCKET      1
#define WIFI_LINK_TX_LOCAL_PORT  5002 // the module rejects local port 0 for UDP; keep it different from UDP_PORT/PEER_PORT
#define WIFI_LINK_SEND_TIMEOUT   100 // ms
#define WIFI_LINK_RESTART_GAP    1000 // a sequence jump this big means the sender restarted
#define WIFI_LINK_FLUSH_MAX_MS   150 // a flush stops after this long even if packets keep coming
#define WIFI_LINK_HELLO_PERIOD_MS 200 // how often HELLO is sent while looking for the peer
#define WIFI_LINK_HELLO_EXTRA    5   // HELLOs sent directly to the peer after finding it, so it finds us too
#define WIFI_LINK_HELLO_MAGIC    0x4F4C4548u // "HELO"
#define WIFI_LINK_TX_DEAD_MS     2000 // TX sends failing this long: rejoin the hotspot (short hiccups recover by themselves)
#define WIFI_LINK_RX_CHECK_MS    3000 // RX with no packets this long: test the link by sending a HELLO
#define WIFI_LINK_RX_RECHECK_MS  1000 // after a failed test, test again this soon
#define WIFI_LINK_RX_CHECK_FAILS 2   // failed tests in a row before rejoining
#define WIFI_LINK_RETRY_MS       3000 // time between rejoin attempts while the hotspot is gone
#define WIFI_LINK_REFIND_MS      2000 // how long to look for the peer after rejoining

// What goes over the air: 4-byte header + audio, 324 bytes in total.
typedef struct __attribute__((packed)) {
  uint16_t seq; // packet counter
  uint16_t nsamples; // samples in this packet
  int16_t  samples[WIFI_LINK_FRAME_SAMPLES]; // audio
} AudioPacket;

// Discovery packet: "I am here, at this IP, listening on this port". 12 bytes.
typedef struct __attribute__((packed)) {
  uint32_t magic; // WIFI_LINK_HELLO_MAGIC
  uint8_t  ip[4]; // sender's IP
  uint16_t listen_port; // sender's UDP_PORT
  uint8_t  knows_peer; // 1 = the sender has already found us
  uint8_t  reserved;
} HelloPacket;

static AudioPacket   tx_pkt;
static AudioPacket   rx_pkt;
static WifiLinkStats stats;
static uint16_t      tx_seq;
static int32_t       rx_expected = -1; // next sequence number expected, -1 = none yet
static uint8_t       my_ip[4];
static uint8_t       peer_ip[4]; // where TX mode sends
static int           peer_known = 0; // peer_ip is valid
static uint8_t       bcast_ip[4]; // broadcast address of the hotspot network
static WifiLinkMode  mode = WIFI_LINK_MODE_RX;
static int           rx_open = 0; // UDP server (socket 1) is running
static int           tx_open = 0; // UDP client (socket 0) is open
static uint32_t      switch_close_ms, switch_open_ms; // how long the last mode switch took, per step
static int           link_up = 0; // joined to the hotspot
static int           tx_failing = 0; // the last send(s) failed
static uint32_t      tx_fail_start; // when the current run of failed sends started
static uint32_t      rx_last_seen; // last packet received in RX (or last link test)
static int           rx_check_fails = 0; // link tests in a row that failed
static uint32_t      last_attempt; // last rejoin attempt

static WifiLinkStatus open_tx(void)
{
  if (!peer_known) {
    return WIFI_LINK_ERR_NO_PEER;
  }
  if (WIFI_OpenClientConnection(WIFI_LINK_TX_SOCKET, WIFI_UDP_PROTOCOL, "wl_tx",
                                peer_ip, PEER_PORT, WIFI_LINK_TX_LOCAL_PORT) != WIFI_STATUS_OK) {
    return WIFI_LINK_ERR_TX_OPEN;
  }
  tx_open = 1;
  return WIFI_LINK_OK;
}

static WifiLinkStatus close_tx(void)
{
  if (WIFI_CloseClientConnection(WIFI_LINK_TX_SOCKET) != WIFI_STATUS_OK) {
    return WIFI_LINK_ERR_CLOSE;
  }
  tx_open = 0;
  return WIFI_LINK_OK;
}

static WifiLinkStatus open_rx(void)
{
  if (WIFI_StartServer(WIFI_LINK_RX_SOCKET, WIFI_UDP_PROTOCOL, 1, "wl_rx", UDP_PORT) != WIFI_STATUS_OK) {
    return WIFI_LINK_ERR_RX_OPEN;
  }
  rx_open = 1;
  rx_expected = -1; // new listening session: don't count the gap as loss
  return WIFI_LINK_OK;
}

static WifiLinkStatus close_rx(void)
{
  if (WIFI_StopServer(WIFI_LINK_RX_SOCKET) != WIFI_STATUS_OK) {
    return WIFI_LINK_ERR_CLOSE;
  }
  rx_open = 0;
  return WIFI_LINK_OK;
}

// Send one HELLO to ip:port using the TX socket (opened and closed again; ~5 ms).
// Returns 1 if the module sent it.
static int send_hello(const uint8_t ip[4], uint16_t port)
{
  HelloPacket h;
  uint16_t sent_len = 0;
  int ok = 0;

  if (tx_open) {
    return 0; // the TX socket is busy sending audio
  }
  h.magic = WIFI_LINK_HELLO_MAGIC;
  memcpy(h.ip, my_ip, sizeof h.ip);
  h.listen_port = UDP_PORT;
  h.knows_peer = (uint8_t)peer_known;
  h.reserved = 0;

  if (WIFI_OpenClientConnection(WIFI_LINK_TX_SOCKET, WIFI_UDP_PROTOCOL, "wl_hi",
                                ip, port, WIFI_LINK_TX_LOCAL_PORT) == WIFI_STATUS_OK) {
    ok = (WIFI_SendData(WIFI_LINK_TX_SOCKET, (uint8_t *)&h, sizeof h, &sent_len,
                        WIFI_LINK_SEND_TIMEOUT) == WIFI_STATUS_OK && sent_len == sizeof h);
    WIFI_CloseClientConnection(WIFI_LINK_TX_SOCKET);
  }
  return ok;
}

// If the received packet is a HELLO, learn the peer from it. Returns 1 if it was a HELLO.
static int handle_hello(uint16_t rx_len, int *peer_knows_us)
{
  const HelloPacket *h = (const HelloPacket *)&rx_pkt;

  if (rx_len != sizeof(HelloPacket) || h->magic != WIFI_LINK_HELLO_MAGIC) {
    return 0;
  }
  if (!peer_known || memcmp(peer_ip, h->ip, sizeof peer_ip) != 0) {
    memcpy(peer_ip, h->ip, sizeof peer_ip); // new peer, or the peer got a new IP
    peer_known = 1;
    rx_expected = -1;
  }
  if (peer_knows_us != NULL) {
    *peer_knows_us = h->knows_peer;
  }
  if (!h->knows_peer) {
    send_hello(peer_ip, PEER_PORT); // it is still looking for us: answer directly
  }
  return 1;
}

// Reset the module, join the hotspot and work out our IP and the broadcast address.
// Used at startup and by the watchdog. Every socket is closed afterwards.
static WifiLinkStatus join_network(void)
{
  link_up = 0;
  rx_open = 0; // the module reset below closes every socket
  tx_open = 0;
  tx_failing = 0;
  rx_check_fails = 0;

  if (WIFI_Init() != WIFI_STATUS_OK) { // also pulses the module's reset pin
    return WIFI_LINK_ERR_INIT;
  }
  if (WIFI_Connect(WIFI_SSID, WIFI_PASSWORD, WIFI_ECN_WPA2_PSK) != WIFI_STATUS_OK) {
    return WIFI_LINK_ERR_CONNECT;
  }
  WIFI_GetIP_Address(my_ip, sizeof my_ip);

  // Broadcast address = our IP with all host bits set, e.g. 10.219.189.22 / 255.255.255.0 -> 10.219.189.255
  uint8_t mask[4] = {255, 255, 255, 0}; // fallback if the module did not report a mask
  if (WIFI_GetNetmask(mask, sizeof mask) != WIFI_STATUS_OK || (mask[0] | mask[1] | mask[2] | mask[3]) == 0) {
    mask[0] = 255; mask[1] = 255; mask[2] = 255; mask[3] = 0;
  }
  for (int i = 0; i < 4; i++) {
    bcast_ip[i] = my_ip[i] | (uint8_t)~mask[i];
  }
  link_up = 1;
  rx_last_seen = HAL_GetTick();
  return WIFI_LINK_OK;
}

WifiLinkStatus wifi_link_init(void)
{
  WifiLinkStatus s = join_network();
  if (s != WIFI_LINK_OK) {
    return s;
  }

#ifdef PEER_DISCOVERY
  peer_known = 0; // found later by wifi_link_find_peer()
#else
  const uint8_t fixed_peer[4] = PEER_IP;
  memcpy(peer_ip, fixed_peer, sizeof peer_ip);
  peer_known = 1;
#endif

  wifi_link_reset_stats();
  mode = WIFI_LINK_MODE_RX;
  return open_rx();
}

WifiLinkStatus wifi_link_set_mode(WifiLinkMode new_mode)
{
  WifiLinkStatus s = WIFI_LINK_OK;
  uint32_t t;

  switch_close_ms = 0;
  switch_open_ms = 0;

  if (new_mode == WIFI_LINK_MODE_TX) {
    if (!tx_open) {
      t = HAL_GetTick();
      s = open_tx();                         // fast path: keep the server open
      switch_open_ms = HAL_GetTick() - t;
      if (s != WIFI_LINK_OK && rx_open) {    // module refused both at once: stop the server first
        t = HAL_GetTick();
        s = close_rx();
        switch_close_ms = HAL_GetTick() - t;
        if (s != WIFI_LINK_OK) {
          return s;
        }
        t = HAL_GetTick();
        s = open_tx();
        switch_open_ms += HAL_GetTick() - t;
      }
    }
  } else {
    if (tx_open) {
      t = HAL_GetTick();
      s = close_tx();
      switch_close_ms = HAL_GetTick() - t;
      if (s != WIFI_LINK_OK) {
        return s;
      }
    }
    t = HAL_GetTick();
    if (rx_open) {
      mode = WIFI_LINK_MODE_RX;
      wifi_link_flush();                     // drop what piled up while we were sending
      rx_expected = -1;
    } else {
      s = open_rx();
    }
    switch_open_ms = HAL_GetTick() - t;
  }

  if (s == WIFI_LINK_OK) {
    mode = new_mode;
  }
  tx_failing = 0; // the watchdog starts counting again in the new mode
  rx_check_fails = 0;
  rx_last_seen = HAL_GetTick();
  return s;
}

WifiLinkStatus wifi_link_find_peer(uint32_t timeout_ms)
{
#ifndef PEER_DISCOVERY
  (void)timeout_ms;
  return WIFI_LINK_OK; // fixed PEER_IP, nothing to find
#else
  uint32_t start = HAL_GetTick();
  uint32_t last_hello = start - WIFI_LINK_HELLO_PERIOD_MS;
  int peer_knows_us = 0;
  int direct_hellos = 0;
  uint16_t rx_len;

  if (!rx_open || mode != WIFI_LINK_MODE_RX) {
    return WIFI_LINK_ERR_RX_OPEN;
  }
  peer_known = 0;

  while (HAL_GetTick() - start < timeout_ms) {
    // Done when we know the peer and it knows us (or we told it directly a few times)
    if (peer_known && (peer_knows_us || direct_hellos >= WIFI_LINK_HELLO_EXTRA)) {
      break;
    }
    if (HAL_GetTick() - last_hello >= WIFI_LINK_HELLO_PERIOD_MS) {
      if (peer_known) {
        send_hello(peer_ip, PEER_PORT); // reliable, direct
        direct_hellos++;
      } else {
        send_hello(bcast_ip, PEER_PORT); // "anyone there?"
      }
      last_hello = HAL_GetTick();
    }
    if (WIFI_ReceiveData(WIFI_LINK_RX_SOCKET, (uint8_t *)&rx_pkt, sizeof rx_pkt,
                         &rx_len, 20) == WIFI_STATUS_OK && rx_len > 0) {
      int knows = 0;
      if (handle_hello(rx_len, &knows) && knows) {
        peer_knows_us = 1;
      }
      // anything else (e.g. audio) is ignored while searching
    }
  }

  rx_expected = -1;
  return peer_known ? WIFI_LINK_OK : WIFI_LINK_ERR_NO_PEER;
#endif
}

int wifi_link_peer_known(void)
{
  return peer_known;
}

WifiLinkMode wifi_link_get_mode(void)
{
  return mode;
}

void wifi_link_get_switch_timing(uint32_t *close_ms, uint32_t *open_ms)
{
  *close_ms = switch_close_ms;
  *open_ms = switch_open_ms;
}

int wifi_link_send_frame(const int16_t *samples)
{
  uint16_t sent_len = 0;

  if (mode != WIFI_LINK_MODE_TX || !tx_open) {
    return 0;
  }

  tx_pkt.seq      = tx_seq++;
  tx_pkt.nsamples = WIFI_LINK_FRAME_SAMPLES;
  memcpy(tx_pkt.samples, samples, sizeof tx_pkt.samples);

  if (WIFI_SendData(WIFI_LINK_TX_SOCKET, (uint8_t *)&tx_pkt, sizeof tx_pkt,
                    &sent_len, WIFI_LINK_SEND_TIMEOUT) != WIFI_STATUS_OK
      || sent_len != sizeof tx_pkt) {
    stats.send_errors++;
    if (!tx_failing) {
      tx_failing = 1; // start of a run of failures (the watchdog looks at how long it lasts)
      tx_fail_start = HAL_GetTick();
    }
    return 0;
  }
  stats.sent++;
  tx_failing = 0;
  return 1;
}

int wifi_link_poll(int16_t *samples, uint32_t timeout_ms)
{
  uint16_t rx_len = 0;

  if (mode != WIFI_LINK_MODE_RX || !rx_open) {
    return 0;
  }
  if (timeout_ms == 0) {
    timeout_ms = 1; // the module needs at least 1 ms
  }

  if (WIFI_ReceiveData(WIFI_LINK_RX_SOCKET, (uint8_t *)&rx_pkt, sizeof rx_pkt,
                       &rx_len, timeout_ms) != WIFI_STATUS_OK || rx_len == 0) {
    return 0; // nothing arrived
  }
  rx_last_seen = HAL_GetTick(); // the link is alive
  rx_check_fails = 0;

  if (handle_hello(rx_len, NULL)) {
    return 0; // the other board (re)announced itself; not audio
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

  if (mode != WIFI_LINK_MODE_RX || !rx_open) {
    return;
  }
  // Read and throw away until the queue is empty (or the time limit is reached,
  // so a sender that never stops cannot keep us here forever).
  uint32_t start = HAL_GetTick();
  while (HAL_GetTick() - start < WIFI_LINK_FLUSH_MAX_MS) {
    if (WIFI_ReceiveData(WIFI_LINK_RX_SOCKET, (uint8_t *)&rx_pkt, sizeof rx_pkt,
                         &rx_len, 1) != WIFI_STATUS_OK || rx_len == 0) {
      break; // queue is empty
    }
    stats.flushed++;
  }
  rx_expected = -1; // don't count the thrown-away packets as lost
}

// Rejoin the hotspot, find the peer again and go back to the mode we were in.
static WifiLinkStatus reconnect(void)
{
  WifiLinkMode was = mode;
  uint8_t old_peer[4];
  int had_peer = peer_known;
  WifiLinkStatus s;

  memcpy(old_peer, peer_ip, sizeof old_peer);
  mode = WIFI_LINK_MODE_RX;

  s = join_network();
  if (s == WIFI_LINK_OK) {
    s = open_rx();
  }
  if (s != WIFI_LINK_OK) {
    link_up = 0; // try again later
    return s;
  }

#ifdef PEER_DISCOVERY
  // Our IP may have changed: announce ourselves. If the peer does not answer in time
  // (e.g. it is busy sending), keep the address we had; it usually stays the same.
  if (wifi_link_find_peer(WIFI_LINK_REFIND_MS) != WIFI_LINK_OK && had_peer) {
    memcpy(peer_ip, old_peer, sizeof peer_ip);
    peer_known = 1;
  }
#else
  (void)had_peer;
#endif

  stats.reconnects++;
  if (was == WIFI_LINK_MODE_TX) {
    wifi_link_set_mode(WIFI_LINK_MODE_TX); // if this fails we stay in RX; the link itself is back
  }
  rx_last_seen = HAL_GetTick();
  return WIFI_LINK_OK;
}

WifiLinkWatchdog wifi_link_watchdog(void)
{
  uint32_t now = HAL_GetTick();
  int lost = 0;

  if (!link_up) {
    if (now - last_attempt < WIFI_LINK_RETRY_MS) {
      return WIFI_LINK_WD_NOTHING; // wait a bit between attempts
    }
    lost = 1;
  } else if (mode == WIFI_LINK_MODE_TX && tx_failing) {
    if (now - tx_fail_start >= WIFI_LINK_TX_DEAD_MS) {
      lost = 1;
    }
  } else if (mode == WIFI_LINK_MODE_RX && now - rx_last_seen >= WIFI_LINK_RX_CHECK_MS) {
    // Silence is normal when nobody talks, so test the link: a HELLO to the peer
    // (or a broadcast one if we don't know it yet). The peer just ignores it.
    int ok = rx_open && (peer_known ? send_hello(peer_ip, PEER_PORT) : send_hello(bcast_ip, PEER_PORT));
    if (ok) {
      rx_check_fails = 0;
      rx_last_seen = now; // next test in RX_CHECK_MS
    } else if (++rx_check_fails >= WIFI_LINK_RX_CHECK_FAILS) {
      lost = 1;
    } else {
      rx_last_seen = now - (WIFI_LINK_RX_CHECK_MS - WIFI_LINK_RX_RECHECK_MS); // test again soon
    }
  }

  if (!lost) {
    return WIFI_LINK_WD_NOTHING;
  }
  link_up = 0;
  WifiLinkStatus s = reconnect();
  last_attempt = HAL_GetTick();
  return (s == WIFI_LINK_OK) ? WIFI_LINK_WD_RECONNECTED : WIFI_LINK_WD_DOWN;
}

int wifi_link_is_up(void)
{
  return link_up;
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

void wifi_link_get_peer_ip(uint8_t ip[4])
{
  memcpy(ip, peer_ip, sizeof peer_ip);
}

const char *wifi_link_last_error(void)
{
  return WIFI_GetLastResponse();
}
