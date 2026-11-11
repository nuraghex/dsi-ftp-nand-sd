// Wi-Fi: scan, network list (bottom screen), password entry, saved networks.
// Every action works with the buttons and with the touch screen.

#include <stdio.h>
#include <string.h>

#include <nds.h>
#include <dswifi9.h>
#include <arpa/inet.h>

#include "app.h"
#include "ftp.h"
#include "lang.h"
#include "netcfg.h"
#include "ui.h"
#include "wifi.h"

#define MAX_APS          10
#define SCAN_FRAMES      (60 * 3)

// A good association (WPA2 key derivation included) takes about 3 s.
// Nothing for 12 s means the chip is stuck: the library never gives up by
// itself (the ARM7 waits for an event from the chip firmware forever).
#define ASSOC_TIMEOUT    (60 * 12)
// lwIP retries DHCP after 1, 2, 4, 8, 16 s: 30 s cover five requests. The
// library has no DHCP timeout of its own (a TODO in Wifi_AssocStatus()).
#define DHCP_TIMEOUT     (60 * 30)
#define MAX_ATTEMPTS     3
#define RETRY_PAUSE      (60 * 2)

// Network list layout (bottom screen)
#define LIST_Y0     24
#define LIST_ROW_H  13
#define LIST_MSG_Y  156
#define LIST_BTN_Y  168
#define LIST_BTN_H  22

static Wifi_AccessPoint aps[MAX_APS];
static int num_aps;

static char cur_ssid[33];
static int cur_bars;
static int cur_channel;
static const char *cur_sec = "";

const char *wifi_ssid(void) { return cur_ssid; }
int wifi_bars(void) { return cur_bars; }
int wifi_channel(void) { return cur_channel; }
const char *wifi_security(void) { return cur_sec; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void show_state(const char *state)
{
    u32 b = getBatteryLevel();
    const ftp_perms_t *p = ftp_get_perms();
    ui_status_t st = {
        .sd_ok = g_sd_ok, .nand_ok = g_nand_ok,
        .sd_read = p->sd_read, .sd_write = p->sd_write, .nand_read = p->nand_read,
        .nand_mode = NAND_RO, // NAND writes are always off while (re)connecting
        .wifi_state = state, .ssid = NULL, .bars = 0, .ip = NULL, .pin = NULL,
        .battery = (int)(b & BATTERY_LEVEL_MASK), .charging = (b & BATTERY_CHARGER_CONNECTED) != 0,
    };
    ui_draw_status(&st);
}

// Bottom screen shows the console while scanning/connecting.
static void show_console(const char *hint)
{
    ui_panel_set(true, TAB_CONSOLE);
    ui_draw_tabs(TAB_CONSOLE, NAND_RO, false);
    ui_draw_console();
    ui_draw_footer(hint);
}

static int rssi_bars(int rssi)
{
    if (rssi > 0) // DS mode: 0..255
        return rssi >= 192 ? 4 : rssi >= 128 ? 3 : rssi >= 64 ? 2 : 1;
    // DSi mode: dBm
    return rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
}

// WPA2 networks without PSK authentication are WPA3-only (SAE): unsupported.
static bool is_wpa3_only(const Wifi_AccessPoint *ap)
{
    return ap->security_type == AP_SECURITY_WPA2 && ap->auth_type != AP_AUTH_PSK;
}

static const char *sec_name(const Wifi_AccessPoint *ap)
{
    switch (ap->security_type)
    {
        case AP_SECURITY_OPEN: return T(S_SEC_OPEN);
        case AP_SECURITY_WEP:  return "WEP";
        case AP_SECURITY_WPA:  return "WPA";
        case AP_SECURITY_WPA2: return is_wpa3_only(ap) ? "WPA3" : "WPA2";
        default:               return "?";
    }
}

static void log_ap(const Wifi_AccessPoint *ap)
{
    flog("AP '%s' ch=%d rssi=%d sec=%s group=%s pair=%s auth=%s%s", ap->ssid, ap->channel, ap->rssi,
         Wifi_ApSecurityTypeString(ap->security_type), Wifi_ApCryptString(ap->group_crypt_type),
         Wifi_ApCryptString(ap->pair_crypt_type), Wifi_ApAuthTypeString(ap->auth_type),
         (ap->flags & WFLAG_APDATA_CONFIG_IN_WFC) ? " [DSi settings]" : "");
}

// Association state in the interface language.
static const char *assoc_name(int st)
{
    switch (st)
    {
        case ASSOCSTATUS_SEARCHING:      return T(S_AS_SEARCHING);
        case ASSOCSTATUS_AUTHENTICATING: return T(S_AS_AUTHENTICATING);
        case ASSOCSTATUS_ASSOCIATING:    return T(S_AS_ASSOCIATING);
        case ASSOCSTATUS_ACQUIRINGDHCP:  return T(S_AS_ACQUIRINGDHCP);
        case ASSOCSTATUS_ASSOCIATED:     return T(S_AS_ASSOCIATED);
        case ASSOCSTATUS_CANNOTCONNECT:  return T(S_AS_CANNOTCONNECT);
        default:                         return T(S_AS_DISCONNECTED);
    }
}

// State change: translated on screen, library name in log.txt.
static void log_state(int st, int f)
{
    char buf[UI_LINE_BYTES];
    snprintf(buf, sizeof(buf), T(S_LOG_STATE), assoc_name(st), f / 60, (f % 60) / 6);
    ui_log_push(buf, COL_WHITE);
    flog(" %s (%d.%d s)", ASSOCSTATUS_STRINGS[st], f / 60, (f % 60) / 6);
}

// ---------------------------------------------------------------------------
// ARM7 Wi-Fi driver log. The debug ARM7 core prints its messages ("T: ...")
// to a buffer shared with the ARM9 (consoleArm7Setup()). They are handed over
// in interrupt context, so here they are only queued; wifi_log_poll() writes
// them to log.txt from the main loop, prefixed by "W7".
// ---------------------------------------------------------------------------

#define W7_RING      4096
#define W7_MAX_LINES 1500 // per session: enough for several connections

static PrintConsole w7_console;
static volatile char w7_ring[W7_RING];
static volatile u32 w7_head; // written in interrupt context only
static volatile u32 w7_tail; // written by wifi_log_poll() only
static bool w7_on;
static int w7_lines;

static bool w7_putc(void *con, char c)
{
    (void)con;
    u32 next = (w7_head + 1) % W7_RING;
    if (next != w7_tail) // full: drop the character
    {
        w7_ring[w7_head] = c;
        w7_head = next;
    }
    return true; // never draw anything
}

void wifi_log_start(void)
{
    if (w7_on)
        return;
    w7_console.PrintChar = w7_putc;
    w7_on = consoleArm7Setup(&w7_console, 2048) == 0;
}

static void w7_flush_line(char *line, int *len)
{
    line[*len] = '\0';
    if (*len > 0 && w7_lines < W7_MAX_LINES)
    {
        flog("W7 %s", line);
        if (++w7_lines == W7_MAX_LINES)
            flog("W7 (stopped: too many messages in this session)");
    }
    *len = 0;
}

void wifi_log_poll(void)
{
    static char line[100];
    static int len;
    if (!w7_on)
        return;
    while (w7_tail != w7_head)
    {
        char c = w7_ring[w7_tail];
        w7_tail = (w7_tail + 1) % W7_RING;
        if (c == '\n')
            w7_flush_line(line, &len);
        else if (c != '\r')
        {
            if (len == (int)sizeof(line) - 1)
                w7_flush_line(line, &len);
            line[len++] = c;
        }
    }
}

bool wifi_init(void)
{
    return Wifi_InitDefault(INIT_ONLY | WIFI_ATTEMPT_DSI_MODE);
}

// NEVER call Wifi_Deinit(): with lwIP enabled, DSWiFi (BlocksDS 1.24/1.25)
// always crashes there ("DSWifi can't be disabled if lwIP is active", a
// libndsCrash() marked TODO in wifi_lwip_deinit()). Only switch the radio off.
//
// The radio is switched off only when quitting, NEVER to "reset" it: in DSi
// mode Wifi_DisableWifi() runs wifi_card_deinit() on the ARM7 and
// Wifi_EnableWifi() then repeats the whole chip bring-up (firmware upload),
// whose busy-wait loops never end on a chip that has not been power cycled.
// The ARM7 hangs, and SD access with it (seen freezing the app in testing).
void wifi_shutdown(void)
{
    Wifi_DisconnectAP();
    Wifi_DisableWifi();
    for (int i = 0; i < 10; i++)
        frame_yield();
}

static int empty_scans;

static void scan_networks(void)
{
    show_state(T(S_ST_SCANNING));
    show_console(T(S_FOOT_SCANNING));
    Wifi_ScanMode();
    for (int i = 0; i < SCAN_FRAMES; i++)
        wait_frame();

    num_aps = 0;
    int n = Wifi_GetNumAP();
    flog("--- scan: %d APs ---", n);
    // Not a single access point twice in a row: the chip is most likely
    // stuck, and only a power cycle helps (see wifi_shutdown()).
    if (n == 0 && ++empty_scans == 2)
        ui_msg(S_LOG_NO_NETWORKS);
    if (n > 0)
        empty_scans = 0;
    for (int i = 0; i < n; i++)
    {
        Wifi_AccessPoint ap;
        if (Wifi_GetAPData(i, &ap) != WIFI_RETURN_OK)
            continue;
        log_ap(&ap);
        if (ap.ssid_len == 0 || (ap.flags & WFLAG_APDATA_NINTENDO_TAG))
            continue;
        // Sorted by signal, strongest first
        int pos = num_aps;
        while (pos > 0 && aps[pos - 1].rssi < ap.rssi)
            pos--;
        if (pos >= MAX_APS)
            continue;
        int last = num_aps < MAX_APS ? num_aps : MAX_APS - 1;
        for (int j = last; j > pos; j--)
            aps[j] = aps[j - 1];
        aps[pos] = ap;
        if (num_aps < MAX_APS)
            num_aps++;
    }
}

// lwIP's public API, exported by libdswifi9 (its headers aren't installed).
// ip4_addr_t is a single u32 in network byte order.
struct netif;
typedef struct { u32 addr; } lwip_ip4_t;
extern struct netif *netif_default;
extern s8 netifapi_netif_set_addr(struct netif *netif, const lwip_ip4_t *ipaddr,
                                  const lwip_ip4_t *netmask, const lwip_ip4_t *gw);

static bool static_ip_used; // a static address was set in this session

static bool ip_valid(u32 ip)
{
    return ip != 0 && ip != INADDR_NONE;
}

// Static IP from the saved network if configured, DHCP otherwise.
// Wifi_SetIP() wants network byte order (what inet_addr() returns).
// Returns true when DHCP is used.
static bool apply_ip(const saved_net_t *n)
{
    if (n && n->ip[0])
    {
        u32 dns = inet_addr(n->dns[0] ? n->dns : n->gw);
        Wifi_SetIP(inet_addr(n->ip), inet_addr(n->gw),
                   inet_addr(n->mask[0] ? n->mask : "255.255.255.0"), dns, dns);
        static_ip_used = true;
        flog("static IP %s (gw %s)", n->ip, n->gw);
        return false;
    }
    Wifi_SetIP(0, 0, 0, 0, 0);
    // Wifi_SetIP(0...) only turns DHCP on: a static address set earlier in
    // this session would stay on the interface until the router answers, and
    // DSWiFi would report it as the new address straight away.
    if (static_ip_used && netif_default)
    {
        const lwip_ip4_t any = { 0 };
        netifapi_netif_set_addr(netif_default, &any, &any, &any);
        static_ip_used = false;
        flog("previous static address cleared");
    }
    flog("IP via DHCP");
    return true;
}

typedef enum { CONN_OK, CONN_FAIL, CONN_STUCK, CONN_NO_DHCP, CONN_CANCEL } conn_result_t;

// One connection attempt. pass == NULL: DSi's own settings (WFC) or no
// password for open networks. B cancels.
static conn_result_t connect_once(Wifi_AccessPoint *ap, const char *pass, const saved_net_t *n,
                                  int attempt)
{
    bool dhcp = apply_ip(n);

    int r;
    const char *how;
    if (pass && pass[0])
    {
        r = Wifi_ConnectSecureAP(ap, pass, strlen(pass));
        how = "password";
    }
    else if (ap->security_type == AP_SECURITY_OPEN)
    {
        r = Wifi_ConnectSecureAP(ap, NULL, 0);
        how = "open network";
    }
    else if (ap->flags & WFLAG_APDATA_CONFIG_IN_WFC)
    {
        r = Wifi_ConnectWfcAP(ap);
        how = "DSi settings";
    }
    else
    {
        return CONN_FAIL;
    }
    show_console(T(S_FOOT_CONNECTING));
    if (attempt == 1)
        ui_msg(S_LOG_CONNECTING, ap->ssid);
    else
        ui_msg(S_LOG_ATTEMPT, attempt, MAX_ATTEMPTS);
    flog("connecting to '%s' (%s, %s), attempt %d, call result=%d", ap->ssid, how,
         dhcp ? "DHCP" : "static IP", attempt, r);
    if (r != 0)
        return CONN_FAIL;

    char state[UI_LINE_BYTES];
    int last = -1, since = 0;
    int dhcp_since = -1; // frame at which the wait for an IPv4 address started
    bool v6_noted = false;
    conn_result_t result = CONN_FAIL;
    for (int f = 0;; f++)
    {
        wait_frame();
        if (keysDown() & KEY_B)
        {
            flog("cancelled with B");
            result = CONN_CANCEL;
            break;
        }
        int st = Wifi_AssocStatus();
        if (st != last)
        {
            last = st;
            since = f;
            snprintf(state, sizeof(state), "%s: %s", ap->ssid, assoc_name(st));
            show_state(state);
            log_state(st, f);
        }
        if (st == ASSOCSTATUS_ACQUIRINGDHCP && dhcp_since < 0)
            dhcp_since = f;
        if (st == ASSOCSTATUS_ASSOCIATED)
        {
            // With DHCP, DSWiFi also reports "associated" as soon as an IPv6
            // address is ready (routers with IPv6 often answer first). The
            // FTP server needs the IPv4 one: keep waiting for it.
            u32 ip = Wifi_GetIP();
            if (!dhcp || ip_valid(ip))
            {
                if (dhcp)
                {
                    struct in_addr a = { ip };
                    ui_msg(S_LOG_DHCP_OK, inet_ntoa(a), f / 60, (f % 60) / 6);
                }
                snprintf(cur_ssid, sizeof(cur_ssid), "%s", ap->ssid);
                cur_bars = rssi_bars(ap->rssi);
                cur_channel = ap->channel;
                cur_sec = sec_name(ap);
                return CONN_OK;
            }
            if (dhcp_since < 0)
                dhcp_since = f;
            if (!v6_noted)
            {
                v6_noted = true;
                ui_msg(S_LOG_IPV6_WAIT);
            }
        }
        if (dhcp_since >= 0 && f - dhcp_since > DHCP_TIMEOUT)
        {
            result = CONN_NO_DHCP;
            break;
        }
        if (st == ASSOCSTATUS_CANNOTCONNECT)
            break;
        bool before_ip = st == ASSOCSTATUS_DISCONNECTED || st == ASSOCSTATUS_SEARCHING ||
                         st == ASSOCSTATUS_AUTHENTICATING || st == ASSOCSTATUS_ASSOCIATING;
        if (before_ip && f - since > ASSOC_TIMEOUT)
        {
            result = CONN_STUCK;
            break;
        }
    }
    static const char *const why[] = {
        [CONN_FAIL] = "refused", [CONN_STUCK] = "stuck", [CONN_NO_DHCP] = "no DHCP answer",
        [CONN_CANCEL] = "cancelled",
    };
    flog("connection failed, last state '%s' (%s)", last >= 0 ? ASSOCSTATUS_STRINGS[last] : "-",
         why[result]);
    if (result == CONN_STUCK)
        ui_msg(S_LOG_STUCK);
    else if (result == CONN_NO_DHCP)
        ui_msg(S_LOG_NO_DHCP);
    else if (result == CONN_FAIL)
        ui_msg(S_LOG_REFUSED);
    // Back to idle: the ARM7 sends a disconnect command to the chip, which
    // also clears a stuck association (the next attempt starts from a scan).
    Wifi_DisconnectAP();
    return result;
}

// Pause between attempts, with the ARM7 idle. false: cancelled with B.
static bool retry_pause(void)
{
    for (int f = 0; f < RETRY_PAUSE; f++)
    {
        wait_frame();
        if (keysDown() & KEY_B)
        {
            flog("cancelled with B");
            return false;
        }
    }
    return true;
}

// Connects, retrying up to MAX_ATTEMPTS times when the association gets
// stuck or DHCP doesn't answer (a refused connection, usually a wrong
// password, is retried once). Returns true when connected.
static bool try_connect(Wifi_AccessPoint *ap, const char *pass, const saved_net_t *n)
{
    conn_result_t r = CONN_FAIL;
    for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++)
    {
        r = connect_once(ap, pass, n, attempt);
        if (r == CONN_OK)
        {
            if (attempt > 1)
                flog("connected at attempt %d", attempt);
            return true;
        }
        if (r == CONN_CANCEL || (r == CONN_FAIL && attempt >= 2))
            break;
        if (attempt < MAX_ATTEMPTS && !retry_pause())
            return false;
    }
    if (r == CONN_STUCK)
        ui_msg(S_LOG_STUCK_FINAL);
    else if (r == CONN_NO_DHCP)
        ui_msg(S_LOG_NO_DHCP_FINAL);
    return false;
}

// ---------------------------------------------------------------------------
// Network list (bottom screen)
// ---------------------------------------------------------------------------

enum { BTN_CONNECT, BTN_DEFAULT, BTN_FORGET, BTN_SCAN, BTN_COUNT };
static const str_t btn_labels[BTN_COUNT] = { S_BTN_CONNECT, S_BTN_DEFAULT, S_BTN_FORGET, S_BTN_SCAN };

// Text clipped to max columns, then drawn.
static void text_clip(int x, int y, const char *s, int max, int col)
{
    char buf[UI_LINE_BYTES];
    ui_clip(buf, sizeof(buf), s, max);
    ui_text(SCR_BOT, x, y, buf, col, 1);
}

static void draw_button(int i, bool enabled)
{
    int bw = 256 / BTN_COUNT;
    int x = i * bw + 2;
    char buf[UI_LINE_BYTES];
    ui_clip(buf, sizeof(buf), T(btn_labels[i]), 7);
    ui_fill(SCR_BOT, x, LIST_BTN_Y, bw - 4, LIST_BTN_H, enabled ? COL_HEADER : COL_TABOFF);
    ui_text(SCR_BOT, x + (bw - 4 - ui_text_w(buf, 1)) / 2, LIST_BTN_Y + 7, buf, enabled ? COL_WHITE : COL_DIM, 1);
}

static void draw_list(int sel, const char *msg)
{
    char buf[UI_LINE_BYTES];
    ui_panel_set(false, TAB_CONSOLE);
    ui_fill(SCR_BOT, 0, 0, 256, 192, COL_BG);
    ui_fill(SCR_BOT, 0, 0, 256, 20, COL_HEADER);
    text_clip(6, 6, T(S_NETLIST_TITLE), 15, COL_WHITE);
    snprintf(buf, sizeof(buf), T(S_NETLIST_FOUND), num_aps);
    ui_text(SCR_BOT, 250 - ui_text_w(buf, 1), 6, buf, COL_GRAY, 1);

    if (num_aps == 0)
    {
        text_clip(10, 60, T(S_NETLIST_NONE), 30, COL_YELLOW);
        text_clip(10, 74, T(S_NETLIST_24GHZ), 30, COL_GRAY);
    }
    for (int i = 0; i < num_aps; i++)
    {
        const Wifi_AccessPoint *ap = &aps[i];
        const saved_net_t *n = netcfg_find(ap->ssid);
        int y = LIST_Y0 + i * LIST_ROW_H;
        bool unusable = is_wpa3_only(ap) || ap->security_type == AP_SECURITY_WPA;
        if (i == sel)
        {
            ui_fill(SCR_BOT, 0, y - 2, 256, LIST_ROW_H, COL_SEL);
            ui_fill(SCR_BOT, 0, y - 2, 3, LIST_ROW_H, COL_ACCENT);
        }
        if (n)
            ui_text(SCR_BOT, 6, y, n->is_default ? "*" : "+", n->is_default ? COL_YELLOW : COL_GREEN, 1);
        ui_clip(buf, sizeof(buf), ap->ssid, 18);
        ui_text(SCR_BOT, 18, y, buf, unusable ? COL_DIM : COL_WHITE, 1);
        text_clip(170, y, sec_name(ap), 6, unusable ? COL_RED : COL_GRAY);
        ui_bars(SCR_BOT, 222, y - 1, rssi_bars(ap->rssi));
    }

    text_clip(6, LIST_MSG_Y, msg ? msg : T(S_NETLIST_LEGEND), 31, msg ? COL_GREEN : COL_GRAY);

    bool saved = num_aps > 0 && netcfg_find(aps[sel].ssid) != NULL;
    draw_button(BTN_CONNECT, num_aps > 0);
    draw_button(BTN_DEFAULT, saved);
    draw_button(BTN_FORGET, saved);
    draw_button(BTN_SCAN, true);
}

// Returns the chosen index, or -1 to rescan.
static int choose_network(void)
{
    int sel = 0;
    draw_list(sel, NULL);
    while (1)
    {
        wait_frame();
        u32 k = keysDown();
        int action = -1;

        if ((k & KEY_DOWN) && sel < num_aps - 1)
            draw_list(++sel, NULL);
        if ((k & KEY_UP) && sel > 0)
            draw_list(--sel, NULL);
        if (k & KEY_A)
            action = BTN_CONNECT;
        if (k & KEY_X)
            action = BTN_DEFAULT;
        if (k & KEY_SELECT)
            action = BTN_FORGET;
        if (k & KEY_Y)
            action = BTN_SCAN;

        if (k & KEY_TOUCH)
        {
            touchPosition t;
            touchRead(&t);
            if (t.py >= LIST_BTN_Y)
            {
                action = t.px / (256 / BTN_COUNT);
            }
            else if (t.py >= LIST_Y0 - 2)
            {
                int row = (t.py - (LIST_Y0 - 2)) / LIST_ROW_H;
                if (row < num_aps)
                {
                    if (row == sel)
                        action = BTN_CONNECT; // second tap on the same network
                    else
                        draw_list(sel = row, NULL);
                }
            }
        }

        if (action == BTN_SCAN)
            return -1;
        if (action < 0 || num_aps == 0)
            continue;
        if (action == BTN_CONNECT)
            return sel;

        saved_net_t *n = netcfg_find(aps[sel].ssid);
        const char *msg;
        if (!n)
            msg = T(S_NET_SAVE_FIRST);
        else if (action == BTN_DEFAULT)
        {
            netcfg_set_default(n->ssid);
            msg = netcfg_save() ? T(S_NET_DEFAULT_SET) : T(S_SAVE_FAILED);
        }
        else
        {
            netcfg_remove(aps[sel].ssid);
            msg = netcfg_save() ? T(S_NET_FORGOTTEN) : T(S_SAVE_FAILED);
        }
        draw_list(sel, msg);
    }
}

// ---------------------------------------------------------------------------
// Password entry and "save network" dialog
// ---------------------------------------------------------------------------

static bool read_password(const char *ssid, char *out, size_t sz)
{
    out[0] = '\0';
    return ui_text_input(T(S_WIFI_PASS_TITLE), T(S_WIFI_PASS_LABEL), ssid, out, sz, 8, false);
}

#define DLG_ROW0_Y   52
#define DLG_ROW1_Y   82
#define DLG_ROW_H    24
#define DLG_BTN_Y    118
#define DLG_BTN_X    68
#define DLG_BTN_W    120

static void draw_save_dialog(const char *ssid, bool save, bool def, int row)
{
    char buf[UI_LINE_BYTES];
    ui_fill(SCR_BOT, 0, 0, 256, 192, COL_BG);
    ui_fill(SCR_BOT, 0, 0, 256, 20, COL_HEADER);
    text_clip(6, 6, T(S_SAVE_CONNECTED_TO), 30, COL_WHITE);
    ui_clip(buf, sizeof(buf), ssid, 30);
    ui_text(SCR_BOT, 6, 28, buf, COL_GREEN, 1);

    const char *labels[2] = { T(S_SAVE_THIS), T(S_SAVE_AS_DEFAULT) };
    const bool vals[2] = { save, def };
    const int ys[2] = { DLG_ROW0_Y, DLG_ROW1_Y };
    for (int i = 0; i < 2; i++)
    {
        int y = ys[i];
        ui_fill(SCR_BOT, 4, y, 248, DLG_ROW_H, row == i ? COL_SEL : COL_CARD);
        ui_fill(SCR_BOT, 12, y + 5, 14, 14, COL_GRAY);
        ui_fill(SCR_BOT, 14, y + 7, 10, 10, COL_BG);
        if (vals[i])
            ui_fill(SCR_BOT, 16, y + 9, 6, 6, COL_GREEN);
        text_clip(36, y + 8, labels[i], 26, COL_WHITE);
    }

    ui_fill(SCR_BOT, DLG_BTN_X, DLG_BTN_Y, DLG_BTN_W, 24, row == 2 ? COL_ACCENT : COL_HEADER);
    ui_clip(buf, sizeof(buf), T(S_CONTINUE), DLG_BTN_W / 8 - 1);
    ui_text(SCR_BOT, DLG_BTN_X + (DLG_BTN_W - ui_text_w(buf, 1)) / 2, DLG_BTN_Y + 8, buf, COL_WHITE, 1);

    static char note[2][UI_LINE_BYTES];
    int n = ui_wrap(T(S_SAVE_PLAIN_NOTE), 31, note, 2);
    for (int i = 0; i < n; i++)
        ui_text(SCR_BOT, 6, 152 + i * 10, note[i], COL_GRAY, 1);
    ui_draw_footer(T(S_FOOT_SAVE));
}

// After connecting with a new password: [x] save, [x] default.
static void save_dialog(const char *ssid, const char *pass)
{
    bool save = true, def = (netcfg_default() == NULL);
    int row = 0;
    ui_panel_set(false, TAB_CONSOLE);
    draw_save_dialog(ssid, save, def, row);
    while (1)
    {
        wait_frame();
        u32 k = keysDown();
        int activate = -1;
        if ((k & KEY_DOWN) && row < 2)
            draw_save_dialog(ssid, save, def, ++row);
        if ((k & KEY_UP) && row > 0)
            draw_save_dialog(ssid, save, def, --row);
        if (k & KEY_A)
            activate = row;
        if (k & KEY_TOUCH)
        {
            touchPosition t;
            touchRead(&t);
            if (t.py >= DLG_ROW0_Y && t.py < DLG_ROW0_Y + DLG_ROW_H)
                activate = 0;
            else if (t.py >= DLG_ROW1_Y && t.py < DLG_ROW1_Y + DLG_ROW_H)
                activate = 1;
            else if (t.py >= DLG_BTN_Y && t.py < DLG_BTN_Y + 24 && t.px >= DLG_BTN_X &&
                     t.px < DLG_BTN_X + DLG_BTN_W)
                activate = 2;
            if (activate >= 0)
                row = activate;
        }
        if (activate == 0)
            save = !save;
        else if (activate == 1)
            def = !def;
        else if (activate == 2)
        {
            if (def)
                save = true; // a default network must be saved
            if (save)
            {
                netcfg_upsert(ssid, pass);
                if (def)
                    netcfg_set_default(ssid);
                ui_msg(netcfg_save() ? S_NET_SAVED : S_SAVE_FAILED);
            }
            return;
        }
        if (activate >= 0)
            draw_save_dialog(ssid, save, def, row);
    }
}

// ---------------------------------------------------------------------------
// Main flow
// ---------------------------------------------------------------------------

void wifi_connect_ui(bool force_list, const char *prefer)
{
    bool auto_try = !force_list;
    char want[33] = "";
    if (prefer)
        snprintf(want, sizeof(want), "%s", prefer); // prefer may point to cur_ssid
    cur_ssid[0] = '\0';

    while (1)
    {
        scan_networks();

        if (auto_try)
        {
            auto_try = false;
            saved_net_t *d = netcfg_default();
            // 0) preferred network, 1) default network, 2) other saved
            // networks, 3) networks configured in the DSi settings
            for (int round = 0; round < 4; round++)
            {
                for (int i = 0; i < num_aps; i++)
                {
                    saved_net_t *n = netcfg_find(aps[i].ssid);
                    bool match = round == 0 ? (n && want[0] && strcmp(n->ssid, want) == 0)
                               : round == 1 ? (n && n == d)
                               : round == 2 ? (n && n != d)
                               : (!n && (aps[i].flags & WFLAG_APDATA_CONFIG_IN_WFC));
                    if (match && try_connect(&aps[i], n ? n->pass : NULL, n))
                        return;
                }
            }
            scan_networks();
        }

        show_state(T(S_ST_CHOOSE));
        int sel = choose_network();
        if (sel < 0)
            continue;
        Wifi_AccessPoint ap = aps[sel];
        saved_net_t *n = netcfg_find(ap.ssid);

        if (ap.security_type == AP_SECURITY_WPA)
        {
            ui_msg(S_LOG_WPA1);
            continue;
        }
        if (is_wpa3_only(&ap))
        {
            ui_msg(S_LOG_WPA3);
            continue;
        }

        // Saved, open or configured in the DSi settings: no password needed
        if (n || ap.security_type == AP_SECURITY_OPEN || (ap.flags & WFLAG_APDATA_CONFIG_IN_WFC))
        {
            if (try_connect(&ap, n ? n->pass : NULL, n))
            {
                if (!n && ap.security_type == AP_SECURITY_OPEN)
                    save_dialog(ap.ssid, "");
                return;
            }
            if (ap.security_type == AP_SECURITY_OPEN)
                continue;
            ui_msg(S_LOG_REENTER_PASS);
        }

        char pass[65];
        if (!read_password(ap.ssid, pass, sizeof(pass)))
            continue;
        if (try_connect(&ap, pass, n))
        {
            save_dialog(ap.ssid, pass);
            memset(pass, 0, sizeof(pass));
            return;
        }
        memset(pass, 0, sizeof(pass));
    }
}
