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

/* Where to send UDP packets: your laptop now, the other board later.
   Check the laptop's IPv4 with ipconfig (it changes between hotspot sessions). */
#define PEER_IP        {192, 168, 1, 100}
#define UDP_PORT       5000

#endif /* WIFI_SECRETS_H */
