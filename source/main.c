// DSi FTP - browse the DSi SD card and NAND from a PC over Wi-Fi.
//
// Top screen: status. Bottom screen: tabs Console / Impostazioni.
// START: quit. SELECT: permissions. Y: choose another network. L/R: tabs.

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <nds.h>
#include <nds/cothread.h>
#include <dswifi9.h>
#include <fat.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "app.h"
#include "ftp.h"
#include "lang.h"
#include "netcfg.h"
#include "ui.h"
#include "wifi.h"

#define SETTINGS_PATH CFG_DIR "/settings.ini"

// NAND writes need the charger or at least this battery level (0..15),
// and are turned off below BATTERY_CUTOFF without charger.
#define BATTERY_MIN_FOR_WRITE 8
#define BATTERY_CUTOFF        4

bool g_sd_ok, g_nand_ok;
u32 g_frames;

static bool g_log; // diagnostic log on the SD (setting, off by default)
static bool lang_chosen; // language picked in the menu (else: DSi system language)

// FTP access (settings): random PIN at every start, or custom user/password.
static bool ftp_custom;
static char ftp_user[33];   // "" = any user name
static char ftp_pass[65];
static int ftp_port = 21;
static char pin_buf[8];     // current random PIN
static u32 adv_ip;          // address announced in PASV replies

#define FTP_PASS_MIN 6

// ---------------------------------------------------------------------------
// Common helpers
// ---------------------------------------------------------------------------

void flog(const char *fmt, ...)
{
    if (!g_sd_ok || !g_log)
        return;
    FILE *f = fopen(LOG_PATH, "a");
    if (!f)
        return;
    fprintf(f, "[%6lu] ", (unsigned long)g_frames);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// Untranslated console line (e.g. the FTP commands): screen and log.txt.
void ui_log(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    flog("%s", buf);
    ui_log_push(buf, COL_WHITE);
}

void ui_msg(str_t id, ...)
{
    static const int cols[] = { [LC_N] = COL_WHITE, [LC_OK] = COL_GREEN, [LC_ER] = COL_RED,
                                [LC_WA] = COL_YELLOW };
    char screen[300], file[300];
    va_list ap, ap2;
    va_start(ap, id);
    va_copy(ap2, ap);
    vsnprintf(screen, sizeof(screen), T(id), ap);
    vsnprintf(file, sizeof(file), T_en(id), ap2);
    va_end(ap2);
    va_end(ap);
    flog("%s", file);
    ui_log_push(screen, cols[lang_color(id)]);
}

void net_yield(void)
{
    cothread_yield();
}

void frame_yield(void)
{
    cothread_yield_irq(IRQ_VBLANK);
}

static void nand_writes_off(void)
{
    ftp_perms_t p = *ftp_get_perms();
    if (!p.nand_write && !p.nand_full)
        return;
    p.nand_write = p.nand_full = false;
    ftp_set_perms(&p);
    nand_WriteProtect(true);
    flog("NAND writing turned off");
}

void app_exit(void)
{
    nand_writes_off();
    ftp_shutdown();
    wifi_shutdown(); // leave the Wi-Fi chip clean for the next program
    flog("exit");
    exit(0);
}

void wait_frame(void)
{
    frame_yield();
    g_frames++;
    wifi_log_poll(); // ARM7 Wi-Fi messages queued since the last frame
    scanKeys();
    if (keysDown() & KEY_START)
        app_exit();
}

static void make_pin(char *pin, size_t sz)
{
    u32 e = g_frames;
    srand((unsigned)time(NULL) ^ (e << 8) ^ REG_VCOUNT ^ TIMER0_DATA);
    for (int i = 0; i < 8; i++)
        e = e * 1103515245u + rand();
    snprintf(pin, sz, "%04u", (unsigned)(e % 10000));
}

// ---------------------------------------------------------------------------
// Settings file: log and the read/write permissions that may persist.
// NAND writes are never saved: they're always off at start.
// ---------------------------------------------------------------------------

static void settings_load(void)
{
    ftp_perms_t p = *ftp_get_perms();
    ftp_lock_cfg_t lock = *ftp_get_lock();
    lang_set(lang_system()); // until the user picks a language
    FILE *f = g_sd_ok ? fopen(SETTINGS_PATH, "r") : NULL;
    if (f)
    {
        char line[128];
        while (fgets(line, sizeof(line), f))
        {
            line[strcspn(line, "\r\n")] = '\0';
            char *v = strchr(line, '=');
            if (!v)
                continue;
            *v++ = '\0';
            bool on = (v[0] == '1');
            if (strcmp(line, "log") == 0)
                g_log = on;
            else if (strcmp(line, "sd_read") == 0)
                p.sd_read = on;
            else if (strcmp(line, "sd_write") == 0)
                p.sd_write = on;
            else if (strcmp(line, "nand_read") == 0)
                p.nand_read = on;
            else if (strcmp(line, "ftp_custom") == 0)
                ftp_custom = on;
            else if (strcmp(line, "ftp_user") == 0)
                snprintf(ftp_user, sizeof(ftp_user), "%s", v);
            else if (strcmp(line, "ftp_pass") == 0)
                snprintf(ftp_pass, sizeof(ftp_pass), "%s", v);
            else if (strcmp(line, "ftp_port") == 0)
                ftp_port = atoi(v);
            else if (strcmp(line, "lock_on") == 0)
                lock.enabled = on;
            else if (strcmp(line, "lock_fails") == 0)
                lock.max_fails = atoi(v);
            else if (strcmp(line, "lock_secs") == 0)
                lock.seconds = atoi(v);
            else if (strcmp(line, "lang") == 0)
            {
                lang_t l;
                if (lang_from_code(v, &l))
                {
                    lang_set(l);
                    lang_chosen = true;
                }
            }
        }
        fclose(f);
    }
    ftp_set_lock(&lock);
    if (ftp_port <= 0 || ftp_port > 65535)
        ftp_port = 21;
    if (strlen(ftp_pass) < FTP_PASS_MIN)
        ftp_custom = false; // a custom login needs a real password
    p.nand_write = p.nand_full = false;
    ftp_set_perms(&p);
}

static void settings_save(void)
{
    if (!g_sd_ok)
        return;
    const ftp_perms_t *p = ftp_get_perms();
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f)
        return;
    fprintf(f, "log=%d\nsd_read=%d\nsd_write=%d\nnand_read=%d\n", g_log, p->sd_read, p->sd_write,
            p->nand_read);
    // The custom FTP password is stored in plain text, like the Wi-Fi ones.
    fprintf(f, "ftp_custom=%d\nftp_user=%s\nftp_pass=%s\nftp_port=%d\n", ftp_custom, ftp_user,
            ftp_pass, ftp_port);
    const ftp_lock_cfg_t *lc = ftp_get_lock();
    fprintf(f, "lock_on=%d\nlock_fails=%d\nlock_secs=%d\n", lc->enabled, lc->max_fails, lc->seconds);
    // Saved only once chosen: otherwise the DSi system language is followed
    if (lang_chosen)
        fprintf(f, "lang=%s\n", lang_code(lang_get()));
    fclose(f);
}

// Pushes the current login settings to the FTP server.
static void apply_login(void)
{
    if (ftp_custom && strlen(ftp_pass) >= FTP_PASS_MIN)
        ftp_set_login(ftp_user, ftp_pass, false);
    else
        ftp_set_login("", pin_buf, true);
}

// ---------------------------------------------------------------------------
// Self-test (diagnostic, only with the log on): TCP to the gateway.
// ---------------------------------------------------------------------------

static void selftest_gateway(struct in_addr gw)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return;
    int on = 1;
    ioctl(fd, FIONBIO, &on);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(80);
    sa.sin_addr = gw;

    int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    int err = (r == 0) ? 0 : errno;
    bool finished = (r == 0) || (err != EINPROGRESS && err != EWOULDBLOCK && err != EALREADY);
    for (int f = 0; !finished && f < 60 * 4; f++)
    {
        wait_frame();
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { 0, 0 };
        if (select(fd + 1, NULL, &wfds, NULL, &tv) > 0)
        {
            int so_err = 0;
            socklen_t len = sizeof(so_err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len);
            err = so_err;
            finished = true;
        }
    }
    bool answered = finished && (err == 0 || err == ECONNREFUSED || err == ECONNRESET ||
                                 err == ENOTCONN || err == EISCONN);
    flog("self-test gateway %s:80 -> %s (errno=%d)", inet_ntoa(gw),
         answered ? "answers" : "no answer", err);
    closesocket(fd);
}

// ---------------------------------------------------------------------------
// Battery, status screen
// ---------------------------------------------------------------------------

static int battery_level = -1;
static bool battery_charging;
static const char *shown_ip;

static void read_battery(void)
{
    u32 b = getBatteryLevel();
    battery_level = (int)(b & BATTERY_LEVEL_MASK);
    battery_charging = (b & BATTERY_CHARGER_CONNECTED) != 0;
}

// Effective NAND access (the permission flags combined with their rules).
static ui_nand_mode_t nand_mode(void)
{
    return ftp_nand_full_active() ? NAND_FULL : ftp_nand_writable() ? NAND_WRITE : NAND_RO;
}

static void draw_server_status(void)
{
    const ftp_perms_t *p = ftp_get_perms();
    ui_status_t st = {
        .sd_ok = g_sd_ok, .nand_ok = g_nand_ok,
        .sd_read = p->sd_read, .sd_write = p->sd_write, .nand_read = p->nand_read,
        .nand_mode = nand_mode(),
        .wifi_state = T(S_ST_CONNECTED), .ssid = wifi_ssid(), .bars = wifi_bars(),
        .ip = shown_ip, .pin = pin_buf, .battery = battery_level, .charging = battery_charging,
        .custom_login = ftp_custom, .user = ftp_user, .port = ftp_port,
    };
    ui_draw_status(&st);
}

static void wait_ab(void)
{
    while (1)
    {
        wait_frame();
        if (keysDown() & (KEY_A | KEY_B))
            return;
    }
}

// ---------------------------------------------------------------------------
// NAND write confirmations (always on the DSi itself)
// ---------------------------------------------------------------------------

// SD for the backups and enough battery: required by both write modes.
static bool write_prerequisites(void)
{
    if (!g_sd_ok)
    {
        ui_draw_dialog_text(T(S_NAND_W_UNAVAIL), COL_RED, T(S_NAND_W_NEED_SD));
        wait_ab();
        return false;
    }
    read_battery();
    if (!battery_charging && battery_level < BATTERY_MIN_FOR_WRITE)
    {
        ui_draw_dialog_text(T(S_NAND_W_UNAVAIL), COL_RED, T(S_NAND_W_LOW_BATT));
        wait_ab();
        return false;
    }
    return true;
}

// Protected write: hold L+R and press A.
static bool confirm_protected_write(void)
{
    if (!write_prerequisites())
        return false;
    ui_draw_dialog_text(T(S_CONFIRM_W_TITLE), COL_RED, T(S_CONFIRM_W_TEXT));
    while (1)
    {
        wait_frame();
        u32 down = keysDown(), held = keysHeld();
        if (down & KEY_B)
            return false;
        if ((down & KEY_A) && (held & KEY_L) && (held & KEY_R))
            return true;
    }
}

// EXPERIMENTAL full write (never tested): hold L+R and press X, then a
// 5 second countdown that B cancels.
static bool confirm_full_write(void)
{
    if (!write_prerequisites())
        return false;
    ui_draw_dialog_text(T(S_CONFIRM_FULL_TITLE), COL_RED, T(S_CONFIRM_FULL_TEXT));
    while (1)
    {
        wait_frame();
        u32 down = keysDown(), held = keysHeld();
        if (down & KEY_B)
            return false;
        if ((down & KEY_X) && (held & KEY_L) && (held & KEY_R))
            break;
    }
    for (int s = 5; s > 0; s--)
    {
        char t[UI_LINE_BYTES];
        snprintf(t, sizeof(t), T(S_COUNTDOWN), s);
        const char *c[] = { "", t, "", T(S_B_CANCEL) };
        ui_draw_dialog(T(S_CONFIRM_FULL_TITLE), COL_RED, c, 4);
        for (int f = 0; f < 60; f++)
        {
            wait_frame();
            if (keysDown() & KEY_B)
                return false;
        }
    }
    return true;
}

// Applies new permissions, with the driver-level NAND protection in the
// safe order, and saves the persistent part.
static void apply_perms(const ftp_perms_t *np)
{
    // Unprotect the driver before the server allows writes; protect it again
    // right after the server stops allowing them.
    bool will_write = g_nand_ok && np->nand_read && np->nand_write;
    if (will_write)
        nand_WriteProtect(false);
    ftp_set_perms(np);
    if (!ftp_nand_writable())
        nand_WriteProtect(true);
    settings_save();
    flog("permissions: SD %s%s, NAND %s%s%s", np->sd_read ? "R" : "-", np->sd_write ? "W" : "-",
         np->nand_read ? "R" : "-", np->nand_write ? "W" : "-", np->nand_full ? " FULL" : "");
}

// ---------------------------------------------------------------------------
// Permissions menu (bottom screen). The FTP server keeps running meanwhile.
// ---------------------------------------------------------------------------

enum { P_SD_READ, P_SD_WRITE, P_NAND_READ, P_NAND_WRITE, P_NAND_FULL, P_COUNT };
#define PERM_Y0  (UI_TAB_H + 4)
#define PERM_H   29

static void draw_perms(int sel)
{
    const ftp_perms_t *p = ftp_get_perms();
    ui_nand_mode_t m = nand_mode();
    ui_draw_title_bar(m == NAND_FULL ? T(S_PERMS_TITLE_FULL) : T(S_PERMS_TITLE),
                      m == NAND_RO ? COL_HEADER : COL_RED, T(S_BACK));
    // Each flag is shown as set; "inactive" when another permission blocks it.
    const char *sd_w = !p->sd_write ? T(S_V_NO_READ_ONLY) : p->sd_read ? T(S_YES) : T(S_V_YES_INACTIVE);
    const char *nd_w = !p->nand_write ? T(S_V_W_OFF) : ftp_nand_writable() ? T(S_V_W_ON) : T(S_V_W_INACTIVE);
    const char *nd_f = !p->nand_full ? T(S_V_FULL_OFF)
                     : ftp_nand_full_active() ? T(S_V_FULL_ON) : T(S_V_FULL_INACTIVE);
    const char *allow_deny_sd_r = p->sd_read ? T(S_DENY) : T(S_ALLOW);
    const char *allow_deny_sd_w = p->sd_write ? T(S_DENY) : T(S_ALLOW);
    const char *allow_deny_nd_r = p->nand_read ? T(S_DENY) : T(S_ALLOW);
    ui_setting_t rows[P_COUNT] = {
        [P_SD_READ] = { T(S_P_SD_READ), p->sd_read ? T(S_YES) : T(S_V_NO_HIDDEN),
                        p->sd_read ? COL_GREEN : COL_GRAY, allow_deny_sd_r, COL_HEADER },
        [P_SD_WRITE] = { T(S_P_SD_WRITE), sd_w,
                         !p->sd_write ? COL_GRAY : p->sd_read ? COL_GREEN : COL_YELLOW,
                         allow_deny_sd_w, COL_HEADER },
        [P_NAND_READ] = { T(S_P_NAND_READ),
                          !g_nand_ok ? T(S_V_NAND_UNAVAIL) : p->nand_read ? T(S_YES) : T(S_V_NO_HIDDEN),
                          p->nand_read ? COL_GREEN : COL_GRAY, g_nand_ok ? allow_deny_nd_r : NULL, COL_HEADER },
        [P_NAND_WRITE] = { T(S_P_NAND_WRITE), nd_w,
                           !p->nand_write ? COL_GRAY : ftp_nand_writable() ? COL_RED : COL_YELLOW,
                           g_nand_ok ? (p->nand_write ? T(S_TURN_OFF) : T(S_TURN_ON)) : NULL,
                           p->nand_write ? COL_HEADER : COL_RED },
        [P_NAND_FULL] = { T(S_P_NAND_FULL), nd_f,
                          !p->nand_full ? COL_GRAY : COL_YELLOW,
                          g_nand_ok ? (p->nand_full ? T(S_TURN_OFF) : T(S_TURN_ON)) : NULL,
                          p->nand_full ? COL_HEADER : COL_RED },
    };
    ui_draw_rows(rows, P_COUNT, sel, PERM_Y0, PERM_H);
    ui_draw_footer(T(S_FOOT_MENU));
}

static void toggle_perm(int row)
{
    ftp_perms_t p = *ftp_get_perms();
    switch (row)
    {
        // Every button changes only its own permission.
        case P_SD_READ:
            p.sd_read = !p.sd_read;
            break;
        case P_SD_WRITE:
            p.sd_write = !p.sd_write;
            break;
        case P_NAND_READ:
            p.nand_read = !p.nand_read;
            break;
        case P_NAND_WRITE:
            if (p.nand_write)
                p.nand_write = false;
            else if (confirm_protected_write())
                p.nand_write = true;
            break;
        case P_NAND_FULL:
            if (p.nand_full)
                p.nand_full = false;
            else if (confirm_full_write())
                p.nand_full = true;
            break;
    }
    apply_perms(&p);
    draw_server_status();
}

static void permissions_menu(void)
{
    int sel = 0;
    ui_panel_set(false, TAB_SERVER);
    draw_perms(sel);
    while (1)
    {
        wait_frame();
        ftp_poll();
        u32 down = keysDown();
        int act = -1;
        if (down & KEY_B)
            return;
        if ((down & KEY_DOWN) && sel < P_COUNT - 1)
            draw_perms(++sel);
        if ((down & KEY_UP) && sel > 0)
            draw_perms(--sel);
        if (down & KEY_A)
            act = sel;
        if (down & KEY_TOUCH)
        {
            touchPosition t;
            touchRead(&t);
            if (ui_back_hit(t.px, t.py))
                return;
            int r = ui_rows_hit(t.py, P_COUNT, PERM_Y0, PERM_H);
            if (r >= 0)
                act = sel = r;
        }
        // SD rows always work; NAND rows only with a mounted NAND.
        if (act >= 0 && (act <= P_SD_WRITE || g_nand_ok))
        {
            toggle_perm(act);
            draw_perms(sel);
        }
        if (Wifi_AssocStatus() != ASSOCSTATUS_ASSOCIATED)
            return; // the main loop handles the reconnection
    }
}

// ---------------------------------------------------------------------------
// Bottom panel: Console / Impostazioni
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// FTP access menu: PIN or custom user/password, port.
// ---------------------------------------------------------------------------

enum { F_MODE, F_PIN, F_USER, F_PASS, F_PORT, F_COUNT };
#define FTPM_Y0  (UI_TAB_H + 4)
#define FTPM_H   29

static void draw_ftp_menu(int sel)
{
    char port[8], passinfo[UI_LINE_BYTES];
    snprintf(port, sizeof(port), "%d", ftp_port);
    if (ftp_pass[0])
        snprintf(passinfo, sizeof(passinfo), T(S_PASS_SET), (unsigned)strlen(ftp_pass));
    else
        snprintf(passinfo, sizeof(passinfo), "%s", T(S_PASS_NOT_SET));
    ui_draw_title_bar(T(S_FTP_ACCESS), COL_HEADER, T(S_BACK));
    ui_setting_t rows[F_COUNT] = {
        [F_MODE] = { T(S_ACCESS_MODE), ftp_custom ? T(S_V_USER_PASS) : T(S_V_RANDOM_PIN), COL_WHITE,
                     T(S_CHANGE), COL_HEADER },
        [F_PIN] = { T(S_PIN_LABEL), ftp_custom ? T(S_V_NOT_USED) : pin_buf,
                    ftp_custom ? COL_GRAY : COL_ACCENT, ftp_custom ? NULL : T(S_NEW), COL_HEADER },
        [F_USER] = { T(S_USERNAME), ftp_user[0] ? ftp_user : T(S_ANY), COL_WHITE, T(S_EDIT), COL_HEADER },
        [F_PASS] = { T(S_PASSWORD), passinfo, ftp_pass[0] ? COL_GREEN : COL_GRAY, T(S_EDIT), COL_HEADER },
        [F_PORT] = { T(S_PORT), port, COL_WHITE, T(S_EDIT), COL_HEADER },
    };
    ui_draw_rows(rows, F_COUNT, sel, FTPM_Y0, FTPM_H);
    ui_draw_footer(ftp_custom ? T(S_FOOT_PLAIN_PASS) : T(S_FOOT_MENU));
}

static bool edit_ftp_password(void)
{
    char buf[65] = "";
    if (!ui_text_input(T(S_FTP_PASS_TITLE), T(S_FTP_PASS_LABEL), T(S_SAVED_PLAIN), buf, sizeof(buf),
                       FTP_PASS_MIN, false))
        return false;
    snprintf(ftp_pass, sizeof(ftp_pass), "%s", buf);
    memset(buf, 0, sizeof(buf));
    return true;
}

// Restarts the listening socket after a port change.
static void restart_server(void)
{
    ftp_shutdown();
    if (!ftp_init(adv_ip, ftp_port, g_nand_ok))
    {
        ui_msg(S_LOG_PORT_FALLBACK, ftp_port);
        ftp_port = 21;
        ftp_init(adv_ip, ftp_port, g_nand_ok);
    }
}

static void ftp_menu(void)
{
    int sel = 0;
    ui_panel_set(false, TAB_SERVER);
    draw_ftp_menu(sel);
    while (1)
    {
        wait_frame();
        ftp_poll();
        u32 down = keysDown();
        int act = -1;
        if (down & KEY_B)
            return;
        if ((down & KEY_DOWN) && sel < F_COUNT - 1)
            draw_ftp_menu(++sel);
        if ((down & KEY_UP) && sel > 0)
            draw_ftp_menu(--sel);
        if (down & KEY_A)
            act = sel;
        if (down & KEY_TOUCH)
        {
            touchPosition t;
            touchRead(&t);
            if (ui_back_hit(t.px, t.py))
                return;
            int r = ui_rows_hit(t.py, F_COUNT, FTPM_Y0, FTPM_H);
            if (r >= 0)
                act = sel = r;
        }
        if (act < 0)
        {
            if (Wifi_AssocStatus() != ASSOCSTATUS_ASSOCIATED)
                return; // the main loop handles the reconnection
            continue;
        }

        switch (act)
        {
            case F_MODE:
                if (ftp_custom)
                    ftp_custom = false;
                else if (strlen(ftp_pass) >= FTP_PASS_MIN || edit_ftp_password())
                    ftp_custom = true; // a custom login needs a password first
                ui_msg(ftp_custom ? S_LOG_LOGIN_USER : S_LOG_LOGIN_PIN);
                break;
            case F_PIN:
                if (!ftp_custom)
                {
                    make_pin(pin_buf, sizeof(pin_buf));
                    ui_msg(S_LOG_NEW_PIN);
                }
                break;
            case F_USER:
            {
                char buf[33];
                snprintf(buf, sizeof(buf), "%s", ftp_user);
                if (ui_text_input(T(S_FTP_USER_TITLE), T(S_FTP_USER_LABEL), NULL, buf, sizeof(buf), 0, false))
                    snprintf(ftp_user, sizeof(ftp_user), "%s", buf);
                break;
            }
            case F_PASS:
                edit_ftp_password();
                break;
            case F_PORT:
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", ftp_port);
                if (ui_text_input(T(S_FTP_PORT_TITLE), T(S_FTP_PORT_LABEL), T(S_FTP_PORT_NOTE), buf,
                                  sizeof(buf), 1, true))
                {
                    int pt = atoi(buf);
                    if (pt < 1 || pt > 65535 || (pt >= 50000 && pt < 51000))
                        ui_msg(S_LOG_PORT_INVALID);
                    else if (pt != ftp_port)
                    {
                        ftp_port = pt;
                        restart_server();
                        ui_msg(S_LOG_PORT_SET, ftp_port);
                    }
                }
                break;
            }
        }
        settings_save();
        apply_login();
        draw_server_status();
        draw_ftp_menu(sel);
    }
}

// ---------------------------------------------------------------------------
// IP address menu (Rete tab): DHCP, fix the current address, or type one.
// Returns true if the setting changed (the caller reconnects).
// ---------------------------------------------------------------------------

// "a.b.c.d" with four numbers 0..255 -> network order; false if malformed.
static bool parse_ipv4(const char *s, u32 *out)
{
    unsigned v[4];
    char extra;
    if (sscanf(s, "%u.%u.%u.%u%c", &v[0], &v[1], &v[2], &v[3], &extra) != 4)
        return false;
    for (int i = 0; i < 4; i++)
        if (v[i] > 255)
            return false;
    *out = htonl((v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3]);
    return true;
}

enum { IP_DHCP, IP_FIX, IP_TYPE, IP_COUNT };
#define IPM_Y0  (UI_TAB_H + 4)
#define IPM_H   29

static void draw_ip_menu(int sel, const saved_net_t *n, const char *cur_ip)
{
    ui_draw_title_bar(T(S_IP_ADDRESS), COL_HEADER, T(S_BACK));
    bool dhcp = !n->ip[0];
    char fixed[UI_LINE_BYTES];
    if (dhcp)
        snprintf(fixed, sizeof(fixed), "%s", T(S_IP_EXAMPLE));
    else
        snprintf(fixed, sizeof(fixed), T(S_IN_USE_FMT), n->ip);
    ui_setting_t rows[IP_COUNT] = {
        [IP_DHCP] = { T(S_IP_AUTO), dhcp ? T(S_IN_USE) : T(S_FROM_ROUTER), dhcp ? COL_GREEN : COL_GRAY,
                      dhcp ? NULL : T(S_USE), COL_HEADER },
        [IP_FIX] = { T(S_IP_FIX_CURRENT), cur_ip, COL_WHITE, T(S_FIX), COL_HEADER },
        [IP_TYPE] = { T(S_IP_TYPE), fixed, COL_WHITE, T(S_TYPE_BTN), COL_HEADER },
    };
    ui_draw_rows(rows, IP_COUNT, sel, IPM_Y0, IPM_H);
    ui_draw_footer(T(S_FOOT_RECONNECT_AFTER));
}

static bool ip_menu(void)
{
    saved_net_t *n = netcfg_find(wifi_ssid());
    if (!n)
    {
        ui_msg(S_LOG_SAVE_NET_FIRST);
        return false;
    }
    struct in_addr gw, mask, dns1, dns2;
    struct in_addr ip = Wifi_GetIPInfo(&gw, &mask, &dns1, &dns2);
    char cur_ip[16], cur_gw[16], cur_mask[16], cur_dns[16];
    snprintf(cur_ip, sizeof(cur_ip), "%s", inet_ntoa(ip));
    snprintf(cur_gw, sizeof(cur_gw), "%s", inet_ntoa(gw));
    snprintf(cur_mask, sizeof(cur_mask), "%s", inet_ntoa(mask));
    snprintf(cur_dns, sizeof(cur_dns), "%s", dns1.s_addr ? inet_ntoa(dns1) : cur_gw);

    int sel = 0;
    ui_panel_set(false, TAB_NET);
    draw_ip_menu(sel, n, cur_ip);
    while (1)
    {
        wait_frame();
        ftp_poll();
        u32 down = keysDown();
        int act = -1;
        if (down & KEY_B)
            return false;
        if ((down & KEY_DOWN) && sel < IP_COUNT - 1)
            draw_ip_menu(++sel, n, cur_ip);
        if ((down & KEY_UP) && sel > 0)
            draw_ip_menu(--sel, n, cur_ip);
        if (down & KEY_A)
            act = sel;
        if (down & KEY_TOUCH)
        {
            touchPosition t;
            touchRead(&t);
            if (ui_back_hit(t.px, t.py))
                return false;
            int r = ui_rows_hit(t.py, IP_COUNT, IPM_Y0, IPM_H);
            if (r >= 0)
                act = sel = r;
        }
        if (act < 0)
            continue;

        if (act == IP_DHCP)
        {
            if (!n->ip[0])
                continue; // already DHCP
            n->ip[0] = n->gw[0] = n->mask[0] = n->dns[0] = '\0';
        }
        else if (act == IP_FIX)
        {
            snprintf(n->ip, sizeof(n->ip), "%s", cur_ip);
            snprintf(n->gw, sizeof(n->gw), "%s", cur_gw);
            snprintf(n->mask, sizeof(n->mask), "%s", cur_mask);
            snprintf(n->dns, sizeof(n->dns), "%s", cur_dns);
        }
        else
        {
            char buf[16];
            snprintf(buf, sizeof(buf), "%s", n->ip[0] ? n->ip : cur_ip);
            if (!ui_text_input(T(S_IP_ADDRESS), T(S_IP_INPUT_LABEL), T(S_IP_INPUT_NOTE), buf, sizeof(buf), 7,
                               false))
            {
                draw_server_status();
                draw_ip_menu(sel, n, cur_ip);
                continue;
            }
            u32 a;
            bool same_net = parse_ipv4(buf, &a) && (a & mask.s_addr) == (gw.s_addr & mask.s_addr) &&
                            a != gw.s_addr && (a & ~mask.s_addr) != 0 &&
                            (a & ~mask.s_addr) != ~mask.s_addr;
            if (!same_net)
            {
                ui_msg(S_LOG_IP_INVALID, cur_gw);
                draw_server_status();
                draw_ip_menu(sel, n, cur_ip);
                continue;
            }
            snprintf(n->ip, sizeof(n->ip), "%s", buf);
            snprintf(n->gw, sizeof(n->gw), "%s", cur_gw);
            snprintf(n->mask, sizeof(n->mask), "%s", cur_mask);
            snprintf(n->dns, sizeof(n->dns), "%s", cur_dns);
        }
        bool saved = netcfg_save();
        ui_msg(!saved ? S_SAVE_FAILED : n->ip[0] ? S_LOG_IP_FIXED : S_LOG_IP_DHCP);
        return saved;
    }
}

// ---------------------------------------------------------------------------
// Bottom panel: tabs Console / Server / Rete / Accessi
// ---------------------------------------------------------------------------

enum {
    A_NONE,
    A_PERMS, A_FTP, A_LOG, A_LANG, A_EXIT,                       // Server
    A_NET_CHANGE, A_NET_IP, A_NET_DEFAULT, A_NET_RECONNECT,      // Rete
    A_LOCK_TOGGLE, A_LOCK_FAILS, A_LOCK_TIME, A_KICK, A_UNBLOCK, // Accessi
};
#define ROWS_MAX 6
#define LOCK_LIST_MAX 8
#define TAB_Y0   (UI_TAB_H + 2)
#define TAB_ROW  25

static const int tab_actions[TAB_COUNT][ROWS_MAX] = {
    [TAB_SERVER] = { A_PERMS, A_FTP, A_LOG, A_LANG, A_EXIT },
    [TAB_NET] = { A_NET_CHANGE, A_NET_IP, A_NONE, A_NONE, A_NET_DEFAULT, A_NET_RECONNECT },
    [TAB_ACCESS] = { A_LOCK_TOGGLE, A_LOCK_FAILS, A_LOCK_TIME, A_KICK, A_UNBLOCK, A_NONE },
};
static const int tab_rows[TAB_COUNT] = { 0, 5, 6, 6 };

static ui_tab_t tab = TAB_CONSOLE;
static int tab_sel[TAB_COUNT];

static int blocked_count(void)
{
    ftp_blocked_t b[1];
    return ftp_get_blocked(b, 1);
}

static void draw_server_tab(void)
{
    const ftp_perms_t *p = ftp_get_perms();
    char perms[UI_LINE_BYTES * 2], login[UI_LINE_BYTES * 2]; // clipped on screen by ui_draw_rows()
    snprintf(perms, sizeof(perms), T(S_PERMS_SUMMARY),
             !p->sd_read ? T(S_SHORT_HIDDEN) : p->sd_write ? T(S_SHORT_RW) : T(S_SHORT_R),
             !g_nand_ok || !p->nand_read ? T(S_SHORT_HIDDEN)
             : ftp_nand_full_active() ? T(S_SHORT_RW_FULL) : ftp_nand_writable() ? T(S_SHORT_RW) : T(S_SHORT_R));
    if (ftp_custom)
        snprintf(login, sizeof(login), T(S_LOGIN_USER_FMT), ftp_user[0] ? ftp_user : T(S_ANY), ftp_port);
    else
        snprintf(login, sizeof(login), T(S_LOGIN_PIN_FMT), pin_buf, ftp_port);
    ui_setting_t items[] = {
        { T(S_ROW_PERMS), perms, nand_mode() == NAND_RO ? COL_WHITE : COL_RED, T(S_OPEN), COL_HEADER },
        { T(S_FTP_ACCESS), login, COL_ACCENT, T(S_OPEN), COL_HEADER },
        { T(S_ROW_LOG), g_log ? T(S_V_LOG_ON) : T(S_V_LOG_OFF), g_log ? COL_GREEN : COL_GRAY,
          g_log ? T(S_TURN_OFF) : T(S_TURN_ON), COL_HEADER },
        { T(S_ROW_LANG), lang_name(lang_get()), COL_WHITE, T(S_CHANGE), COL_HEADER },
        { T(S_ROW_EXIT), T(S_V_EXIT), COL_GRAY, T(S_EXIT_BTN), COL_RED },
    };
    ui_draw_rows(items, 5, tab_sel[TAB_SERVER], TAB_Y0, TAB_ROW);
}

static void draw_net_tab(void)
{
    struct in_addr gw, mask, dns1, dns2;
    struct in_addr ip = Wifi_GetIPInfo(&gw, &mask, &dns1, &dns2);
    const saved_net_t *n = netcfg_find(wifi_ssid());
    const saved_net_t *d = netcfg_default();
    char net[48], addr[UI_LINE_BYTES], gwdns[48], hw[48];
    snprintf(net, sizeof(net), "%s", wifi_ssid());
    snprintf(addr, sizeof(addr), "%s (%s)", inet_ntoa(ip), (n && n->ip[0]) ? T(S_V_STATIC) : "DHCP");
    snprintf(gwdns, sizeof(gwdns), "gw %s", inet_ntoa(gw));
    snprintf(gwdns + strlen(gwdns), sizeof(gwdns) - strlen(gwdns), "  dns %s", inet_ntoa(dns1));
    u8 mac[6] = { 0 };
    Wifi_GetData(WIFIGETDATA_MACADDRESS, 6, mac);
    snprintf(hw, sizeof(hw), "ch %d %s  %02X%02X%02X%02X%02X%02X", wifi_channel(), wifi_security(), mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5]);
    ui_setting_t items[] = {
        { T(S_ROW_NETWORK), net, COL_WHITE, T(S_CHANGE), COL_HEADER },
        { T(S_IP_ADDRESS), addr, COL_ACCENT, n ? T(S_EDIT) : NULL, COL_HEADER },
        { T(S_ROW_ROUTER_DNS), gwdns, COL_WHITE, NULL, 0 },
        { T(S_ROW_CHANNEL), hw, COL_WHITE, NULL, 0 },
        { T(S_ROW_DEFAULT_NET), d ? d->ssid : T(S_NONE_NET), d ? COL_WHITE : COL_GRAY,
          (n && !n->is_default) ? T(S_USE_THIS) : NULL, COL_HEADER },
        { T(S_ROW_RECONNECT), T(S_V_RECONNECT), COL_GRAY, T(S_RECONNECT_BTN), COL_HEADER },
    };
    ui_draw_rows(items, 6, tab_sel[TAB_NET], TAB_Y0, TAB_ROW);
}

static void draw_access_tab(void)
{
    const ftp_lock_cfg_t *lc = ftp_get_lock();
    char lock[UI_LINE_BYTES], fails[UI_LINE_BYTES], secs[24], clients[UI_LINE_BYTES],
        blocked[UI_LINE_BYTES], last[UI_LINE_BYTES];
    snprintf(lock, sizeof(lock), "%s", lc->enabled ? T(S_V_LOCK_ON) : T(S_V_LOCK_OFF));
    snprintf(fails, sizeof(fails), T(S_N_FAILS), lc->max_fails);
    if (lc->seconds >= 60)
        snprintf(secs, sizeof(secs), "%d min", lc->seconds / 60);
    else
        snprintf(secs, sizeof(secs), "%d s", lc->seconds);

    ftp_client_t cl[4];
    int nc = ftp_get_clients(cl, 4);
    if (nc == 0)
        snprintf(clients, sizeof(clients), "%s", T(S_NONE));
    else
    {
        struct in_addr a = { .s_addr = cl[0].ip };
        snprintf(clients, sizeof(clients), "%s%s", inet_ntoa(a), cl[0].authed ? "" : " (login)");
        if (nc > 1)
            snprintf(clients + strlen(clients), sizeof(clients) - strlen(clients), " +%d", nc - 1);
    }

    ftp_blocked_t bl[LOCK_LIST_MAX];
    int nb = ftp_get_blocked(bl, LOCK_LIST_MAX);
    if (nb == 0)
        snprintf(blocked, sizeof(blocked), "%s", T(S_NONE));
    else
    {
        struct in_addr a = { .s_addr = bl[0].ip };
        snprintf(blocked, sizeof(blocked), "%s (%d s)", inet_ntoa(a), bl[0].remaining);
        if (nb > 1)
            snprintf(blocked + strlen(blocked), sizeof(blocked) - strlen(blocked), " +%d", nb - 1);
    }

    const ftp_attempt_t *la = ftp_get_last_attempt();
    if (!la->valid)
        snprintf(last, sizeof(last), "%s", T(S_NONE));
    else
    {
        struct in_addr a = { .s_addr = la->ip };
        char ip_s[16];
        snprintf(ip_s, sizeof(ip_s), "%s", inet_ntoa(a));
        snprintf(last, sizeof(last), T(S_LAST_FMT), ip_s, la->ok ? "OK" : T(S_WRONG),
                 (long)(time(NULL) - la->when));
    }

    ui_setting_t items[] = {
        { T(S_ROW_LOCK), lock, lc->enabled ? COL_GREEN : COL_YELLOW, lc->enabled ? T(S_TURN_OFF) : T(S_TURN_ON),
          lc->enabled ? COL_HEADER : COL_RED },
        { T(S_ROW_LOCK_AFTER), fails, lc->enabled ? COL_WHITE : COL_GRAY, T(S_CHANGE), COL_HEADER },
        { T(S_ROW_LOCK_TIME), secs, lc->enabled ? COL_WHITE : COL_GRAY, T(S_CHANGE), COL_HEADER },
        { T(S_ROW_CLIENTS), clients, nc ? COL_ACCENT : COL_GRAY, nc ? T(S_DISCONNECT) : NULL, COL_HEADER },
        { T(S_ROW_BLOCKED), blocked, nb ? COL_RED : COL_GRAY, nb ? T(S_UNBLOCK) : NULL, COL_HEADER },
        { T(S_ROW_LAST), last, (la->valid && !la->ok) ? COL_YELLOW : COL_WHITE, NULL, 0 },
    };
    ui_draw_rows(items, 6, tab_sel[TAB_ACCESS], TAB_Y0, TAB_ROW);
}

static void panel_show(void)
{
    ui_panel_set(true, tab);
    ui_draw_tabs(tab, nand_mode(), blocked_count() > 0);
    switch (tab)
    {
        case TAB_CONSOLE:
            ui_draw_console();
            ui_draw_footer(T(S_FOOT_CONSOLE));
            return;
        case TAB_SERVER:
            draw_server_tab();
            break;
        case TAB_NET:
            draw_net_tab();
            break;
        case TAB_ACCESS:
            draw_access_tab();
            break;
        default:
            break;
    }
    ui_draw_footer(T(S_FOOT_TABS));
}

// ---------------------------------------------------------------------------
// Language menu: every language written in itself. Applied at once.
// ---------------------------------------------------------------------------

#define LANGM_Y0  (UI_TAB_H + 4)
#define LANGM_H   18

static void draw_lang_menu(int sel)
{
    ui_draw_title_bar(T(S_LANGUAGE), COL_HEADER, T(S_BACK));
    ui_fill(SCR_BOT, 0, UI_TAB_H, 256, UI_FOOTER_Y - UI_TAB_H, COL_BG);
    for (int i = 0; i < LANG_COUNT; i++)
    {
        int y = LANGM_Y0 + i * LANGM_H;
        bool cur = (lang_t)i == lang_get();
        ui_fill(SCR_BOT, 4, y, 248, LANGM_H - 3, i == sel ? COL_SEL : COL_CARD);
        if (i == sel)
            ui_fill(SCR_BOT, 4, y, 3, LANGM_H - 3, COL_ACCENT);
        ui_text(SCR_BOT, 14, y + 4, lang_name((lang_t)i), cur ? COL_GREEN : COL_WHITE, 1);
        if (cur)
            ui_text(SCR_BOT, 232, y + 4, "*", COL_GREEN, 1);
    }
    ui_draw_footer(T(S_FOOT_MENU));
}

static void lang_menu(void)
{
    int sel = lang_get();
    ui_panel_set(false, TAB_SERVER);
    draw_lang_menu(sel);
    while (1)
    {
        wait_frame();
        ftp_poll();
        u32 down = keysDown();
        int act = -1;
        if (down & KEY_B)
            return;
        if ((down & KEY_DOWN) && sel < LANG_COUNT - 1)
            draw_lang_menu(++sel);
        if ((down & KEY_UP) && sel > 0)
            draw_lang_menu(--sel);
        if (down & KEY_A)
            act = sel;
        if (down & KEY_TOUCH)
        {
            touchPosition t;
            touchRead(&t);
            if (ui_back_hit(t.px, t.py))
                return;
            int r = ui_rows_hit(t.py, LANG_COUNT, LANGM_Y0, LANGM_H);
            if (r >= 0)
                act = sel = r;
        }
        if (act >= 0)
        {
            lang_set((lang_t)act);
            lang_chosen = true;
            settings_save();
            flog("language: %s", lang_code(lang_get()));
            draw_server_status();
            return;
        }
        if (Wifi_AssocStatus() != ASSOCSTATUS_ASSOCIATED)
            return; // the main loop handles the reconnection
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    ui_init();
    lang_set(lang_system()); // settings_load() may pick another one

    if (!isDSiMode())
    {
        ui_draw_dialog_text(T(S_DSI_MODE_TITLE), COL_RED, T(S_DSI_MODE_TEXT));
        while (1)
            wait_frame();
    }

    g_sd_ok = fatInitDefault();
    g_nand_ok = nandInit(true); // READ-ONLY. Mounted even without SD.
    if (g_sd_ok)
    {
        mkdir("sd:/_nds", 0777);
        mkdir(CFG_DIR, 0777);
    }
    ftp_preinit(g_nand_ok); // the server must know about the NAND before the permissions
    settings_load();
    flog("===== DSi FTP " APP_VERSION " started: SD %s, NAND %s, language %s =====",
         g_sd_ok ? "ok" : "missing", g_nand_ok ? "ok (read only)" : "not available", lang_code(lang_get()));
    read_battery();

    if (g_log)
        wifi_log_start(); // before wifi_init(): the chip bring-up is logged too
    if (!wifi_init())
    {
        ui_draw_dialog_text(T(S_WIFI_ERR_TITLE), COL_RED, T(S_WIFI_ERR_TEXT));
        while (1)
            wait_frame();
    }
    netcfg_load();

    bool force_list = false;
    char prefer[33] = ""; // reconnect to this network first (after an IP change)
    while (1)
    {
        wifi_connect_ui(force_list, prefer[0] ? prefer : NULL);
        force_list = false;
        prefer[0] = '\0';

        struct in_addr gw, mask, dns1, dns2;
        struct in_addr ip = Wifi_GetIPInfo(&gw, &mask, &dns1, &dns2);
        char ipstr[20];
        snprintf(ipstr, sizeof(ipstr), "%s", inet_ntoa(ip));
        flog("connected to '%s', IP %s, gateway %s", wifi_ssid(), ipstr, inet_ntoa(gw));
        if (g_log)
            selftest_gateway(gw);

        make_pin(pin_buf, sizeof(pin_buf));
        apply_login();

        adv_ip = ip.s_addr; // address announced in PASV replies
        shown_ip = ipstr;
        if (!ftp_init(adv_ip, ftp_port, g_nand_ok))
        {
            ui_msg(S_LOG_PORT_BUSY, ftp_port);
            ftp_port = 21;
            if (!ftp_init(adv_ip, ftp_port, g_nand_ok))
            {
                ui_draw_dialog_text(T(S_FTP_ERR_TITLE), COL_RED, T(S_PORT21_TEXT));
                while (1)
                    wait_frame();
            }
        }
        draw_server_status();
        panel_show();
        ui_msg(S_LOG_SERVER_READY, ipstr);

        int last_blocked = blocked_count();
        while (1)
        {
            wait_frame();
            u32 down = keysDown();
            int action = A_NONE;

            // Tabs: L/R cycle, or touch on the tab bar
            if (down & KEY_L)
            {
                tab = (ui_tab_t)((tab + TAB_COUNT - 1) % TAB_COUNT);
                panel_show();
            }
            if (down & KEY_R)
            {
                tab = (ui_tab_t)((tab + 1) % TAB_COUNT);
                panel_show();
            }
            if (down & KEY_TOUCH)
            {
                touchPosition t;
                touchRead(&t);
                int hit = ui_tabs_hit(t.px, t.py);
                if (hit >= 0)
                {
                    tab = (ui_tab_t)hit;
                    panel_show();
                }
                else if (tab_rows[tab] > 0)
                {
                    int r = ui_rows_hit(t.py, tab_rows[tab], TAB_Y0, TAB_ROW);
                    if (r >= 0)
                    {
                        tab_sel[tab] = r;
                        action = tab_actions[tab][r];
                        panel_show();
                    }
                }
            }
            if (tab_rows[tab] > 0)
            {
                if ((down & KEY_DOWN) && tab_sel[tab] < tab_rows[tab] - 1)
                {
                    tab_sel[tab]++;
                    panel_show();
                }
                if ((down & KEY_UP) && tab_sel[tab] > 0)
                {
                    tab_sel[tab]--;
                    panel_show();
                }
                if (down & KEY_A)
                    action = tab_actions[tab][tab_sel[tab]];
            }

            // Shortcuts, from any tab
            if (down & KEY_SELECT)
                action = A_PERMS;
            if (down & KEY_Y)
                action = A_NET_CHANGE;

            bool redraw = true;
            ftp_lock_cfg_t lc = *ftp_get_lock();
            switch (action)
            {
                case A_PERMS:
                    permissions_menu();
                    draw_server_status();
                    break;
                case A_FTP:
                    ftp_menu();
                    draw_server_status();
                    break;
                case A_LOG:
                    g_log = !g_log;
                    settings_save();
                    if (g_log)
                    {
                        flog("===== log turned on (DSi FTP " APP_VERSION ") =====");
                        wifi_log_start();
                    }
                    ui_msg(g_log ? S_LOG_LOG_ON : S_LOG_LOG_OFF);
                    break;
                case A_LANG:
                    lang_menu();
                    break;
                case A_EXIT:
                    app_exit();
                    break;
                case A_NET_CHANGE:
                    ui_msg(S_LOG_CHANGE_NET);
                    force_list = true;
                    break;
                case A_NET_IP:
                    if (ip_menu())
                        snprintf(prefer, sizeof(prefer), "%s", wifi_ssid()); // reconnect below
                    draw_server_status();
                    break;
                case A_NET_DEFAULT:
                {
                    saved_net_t *n = netcfg_find(wifi_ssid());
                    if (!n)
                        ui_msg(S_LOG_NET_NOT_SAVED);
                    else if (!n->is_default)
                    {
                        netcfg_set_default(n->ssid);
                        ui_msg(netcfg_save() ? S_NET_DEFAULT_SET : S_SAVE_FAILED);
                    }
                    break;
                }
                case A_NET_RECONNECT:
                    ui_msg(S_LOG_RECONNECTING);
                    snprintf(prefer, sizeof(prefer), "%s", wifi_ssid());
                    break;
                case A_LOCK_TOGGLE:
                    lc.enabled = !lc.enabled;
                    ftp_set_lock(&lc);
                    settings_save();
                    ui_msg(lc.enabled ? S_LOG_LOCK_ON : S_LOG_LOCK_OFF);
                    break;
                case A_LOCK_FAILS:
                    lc.max_fails = lc.max_fails == 3 ? 5 : lc.max_fails == 5 ? 10 : 3;
                    ftp_set_lock(&lc);
                    settings_save();
                    break;
                case A_LOCK_TIME:
                    lc.seconds = lc.seconds == 30 ? 120 : lc.seconds == 120 ? 600 : 30;
                    ftp_set_lock(&lc);
                    settings_save();
                    break;
                case A_KICK:
                    ftp_disconnect_all();
                    break;
                case A_UNBLOCK:
                    ftp_unblock_all();
                    break;
                default:
                    redraw = false;
                    break;
            }
            if (force_list || prefer[0])
                break; // leave the server loop: change network / reconnect
            if (redraw)
                panel_show();

            // Every second: blocked addresses (tab label alert, countdown)
            if (g_frames % 60 == 0)
            {
                int nb = blocked_count();
                if (nb != last_blocked || (tab == TAB_ACCESS))
                    panel_show();
                last_blocked = nb;
            }

            // Every 2 seconds: battery (and NAND write safety cut-off)
            if (g_frames % 120 == 0)
            {
                int old_level = battery_level;
                bool old_charging = battery_charging;
                read_battery();
                if (nand_mode() != NAND_RO && !battery_charging && battery_level < BATTERY_CUTOFF)
                {
                    ui_msg(S_LOG_BATT_CUTOFF);
                    nand_writes_off();
                    draw_server_status();
                    panel_show();
                }
                else if (old_level != battery_level || old_charging != battery_charging)
                {
                    draw_server_status();
                }

                // DHCP renewal may change the address (or lose it)
                u32 now_ip = Wifi_GetIP();
                if (now_ip != ip.s_addr)
                {
                    if (now_ip == 0 || now_ip == INADDR_NONE)
                    {
                        ui_msg(S_LOG_IP_LOST);
                        snprintf(prefer, sizeof(prefer), "%s", wifi_ssid());
                        break;
                    }
                    ip.s_addr = now_ip;
                    snprintf(ipstr, sizeof(ipstr), "%s", inet_ntoa(ip));
                    ui_msg(S_LOG_NEW_IP, ipstr);
                    adv_ip = now_ip;
                    restart_server(); // PASV replies must carry the new address
                    draw_server_status();
                    panel_show();
                }
            }

            if (Wifi_AssocStatus() != ASSOCSTATUS_ASSOCIATED)
            {
                ui_msg(S_LOG_WIFI_LOST);
                break;
            }
            ftp_poll();
        }

        nand_writes_off();
        ftp_shutdown();
        Wifi_DisconnectAP();
    }
}
