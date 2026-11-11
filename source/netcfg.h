// Saved Wi-Fi networks: sd:/_nds/dsiftp/networks.ini
//
// One block per network, starting with "ssid=":
//   ssid=MyNetwork
//   pass=secret          (plain text: the file is on the SD card)
//   default=1            (network tried first at startup)
//   ip=192.168.1.50     (optional static IPv4; DHCP if missing)
//   gw=... mask=... dns=...
#ifndef DSIFTP_NETCFG_H
#define DSIFTP_NETCFG_H

#include <stdbool.h>

#define NETCFG_MAX 8

typedef struct {
    char ssid[33];
    char pass[65];
    bool is_default;
    char ip[16], gw[16], mask[16], dns[16];
} saved_net_t;

extern saved_net_t netcfg[NETCFG_MAX];
extern int netcfg_count;

// Loads networks.ini. If it doesn't exist yet, imports the old wifi.cfg
// (as default) and wifi-pi.cfg, then writes networks.ini.
void netcfg_load(void);
bool netcfg_save(void);

saved_net_t *netcfg_find(const char *ssid);
saved_net_t *netcfg_default(void);

// Adds or updates a network (keeps its static IP settings if it exists).
saved_net_t *netcfg_upsert(const char *ssid, const char *pass);
void netcfg_set_default(const char *ssid);
void netcfg_remove(const char *ssid);

#endif
