// Wi-Fi: scan, network list, password entry, saved networks, connection.
#ifndef DSIFTP_WIFI_H
#define DSIFTP_WIFI_H

#include <stdbool.h>

bool wifi_init(void);

// Disconnects and switches the radio off before quitting. It does NOT call
// Wifi_Deinit(): with lwIP enabled DSWiFi always crashes there. The radio is
// never switched back on (impossible in DSi mode, see wifi.c).
void wifi_shutdown(void);

// Messages of the ARM7 Wi-Fi driver (debug ARM7 core only) to log.txt.
// wifi_log_start() can be called at any time; wifi_log_poll() every frame.
void wifi_log_start(void);
void wifi_log_poll(void);

// Returns once connected. Unless force_list is set it first tries `prefer`
// (if given and visible), the default saved network, the other saved ones,
// then the DSi's own settings.
void wifi_connect_ui(bool force_list, const char *prefer);

const char *wifi_ssid(void);     // connected network
int wifi_bars(void);             // signal 0..4 of the connected network
int wifi_channel(void);          // channel of the connected network
const char *wifi_security(void); // "WPA2", "open"... of the connected network

#endif
