/*
 * wifi_secrets_template.h
 *
 * Copy this file to wifi_secrets.h (same folder) and fill in your hotspot and target IP.
 * wifi_secrets.h is listed in .gitignore, so your password is never pushed.
 * Use a 2.4 GHz WPA2 hotspot.
 */
#ifndef WIFI_SECRETS_H
#define WIFI_SECRETS_H

#define WIFI_SSID      "YourHotspotName"
#define WIFI_PASSWORD  "YourHotspotPassword"

/* The other board's IP (or the laptop's for tests). Each board prints its own IP at startup:
   flash both, read the IPs from the serial terminal, put each one in the OTHER board's file.
   The hotspot usually gives a device the same IP again, but check if nothing arrives. */
#define PEER_IP {192, 168, 1, 100}
#define UDP_PORT 5000   // this board listens here
#define PEER_PORT 5001   // the other side listens here
/* Use different ports on the two boards:
     board A: UDP_PORT 5000, PEER_PORT 5001
     board B: UDP_PORT 5001, PEER_PORT 5000 */

#endif /* WIFI_SECRETS_H */
