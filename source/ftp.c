// Minimal FTP server for the Nintendo DSi.
//
// Virtual filesystem seen by the client:
//   /sd     -> "sd:/"    (read/write)
//   /nand   -> "nand:/"  (read-only, enforced here AND by nandInit(true))
//   /nand2  -> "nand2:/" (read-only, camera photos partition)
//
// Single-threaded: control connections are polled once per frame with
// non-blocking sockets; a data transfer runs to completion before the next
// poll. Only passive mode (PASV/EPSV) is supported.

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <nds.h>
#include <nds/cothread.h>
#include <dswifi9.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "ftp.h"
#include "lang.h"

#define MAX_SESSIONS     4
#define PASV_BASE        50000
#define PASV_RANGE       1000
#define LINE_LEN         1024
#define VPATH_LEN        768
#define RPATH_LEN        800
#define XFER_BUF_SIZE    (32 * 1024)
#define ACCEPT_TIMEOUT_FRAMES (60 * 15)
#define IDLE_TIMEOUT_FRAMES   (60 * 30)
#define MAX_LOGIN_FAILS  5     // per connection, then it's closed
#define LOCK_SLOTS       8     // client addresses tracked for the login lock

typedef enum { V_ROOT, V_SD, V_NAND, V_NAND2, V_INVALID } vkind_t;

typedef struct {
    int ctrl;                 // control socket, -1 = free slot
    int pasv;                 // passive listening socket, -1 = none
    bool authed;
    bool got_user;
    char user[33];            // name given with USER
    uint32_t peer;            // client IPv4 address (network order)
    int fails;
    char cwd[VPATH_LEN];      // normalized virtual path ("/" or "/sd/x")
    char rnfr[VPATH_LEN];     // pending rename source, "" = none
    char line[LINE_LEN];
    int linelen;
    long long rest;           // REST offset for the next RETR
} session_t;

static int listen_fd = -1;
static session_t sess[MAX_SESSIONS];
static uint32_t my_ip;        // network byte order
static int ctrl_port = 21;
// Login: any user name (login_user == "") and the PIN, or a custom user/password.
static char login_user[33];
static char login_pass[65];
static bool login_is_pin = true;
// Login lock per client address: after lock_cfg.max_fails wrong passwords
// (over any number of connections) that address is refused for a while.
static ftp_lock_cfg_t lock_cfg = { .enabled = true, .max_fails = 5, .seconds = 30 };
static struct { uint32_t ip; int fails; time_t locked_until; time_t last; } lock_tab[LOCK_SLOTS];
static ftp_attempt_t last_attempt;
static bool nand_ok;
static unsigned pasv_counter;
static uint8_t *xbuf;
// Default: SD read/write, NAND read-only. Writes to the NAND are never on at start.
static ftp_perms_t perms = { .sd_read = true, .sd_write = true, .nand_read = true };
static bool ftp_ready;               // sessions initialized by ftp_init()
static char backup_dir[96];          // per-session backup folder on the SD
static unsigned backup_count;

#define BACKUP_ROOT "sd:/_nds/dsiftp/nand_backup"

// ---------------------------------------------------------------------------
// Socket helpers
// ---------------------------------------------------------------------------

static void set_nonblock(int fd)
{
    int on = 1;
    ioctl(fd, FIONBIO, &on);
}

static void sock_close(int *fd)
{
    if (*fd >= 0)
    {
        shutdown(*fd, SHUT_RDWR);
        closesocket(*fd);
        *fd = -1;
    }
}

// Ends a session. Always both sockets: a passive socket left open here is
// lost when the slot is reused, and lwIP only has 8 sockets (four clients
// that asked for PASV and then just quit used to block every transfer).
static void session_close(session_t *s)
{
    sock_close(&s->pasv);
    sock_close(&s->ctrl);
}

static bool would_block(int ret)
{
    return ret < 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
}

// Waits a little: yields to other work and, every few calls, to the next frame.
//
// The sleep lets only one send buffer through per frame (about 700 KB/s with
// DSWiFi's 11680 bytes), but it must stay: sending faster freezes the console.
// Measured on 2026-11-13 with BlocksDS 1.25: 256 quick retries gain nothing,
// 1024 froze a download after 7 MB, never sleeping after 1 MB.
#ifdef DSIFTP_TUNING
// Test builds only (make TUNING=1): the XWAIT command changes the number of
// quick retries, to try a faster send side against a modified DSWiFi.
static int wait_spins = 64;
#else
#define wait_spins 64
#endif

static void wait_a_bit(int *spins, int *frames)
{
    net_yield();
    if (++(*spins) >= wait_spins)
    {
        *spins = 0;
        // Never swiWaitForVBlank() here: it doesn't let the lwIP cothread run.
        cothread_yield_irq(IRQ_VBLANK);
        (*frames)++;
    }
}

// The same for a receive loop, where the sleep is not needed: here it only
// halves the speed (one 5840-byte TCP window per frame, about 350 KB/s), and
// receiving without it was stable in the same tests. So this keeps yielding to
// the lwIP cothreads and sleeps, a frame at a time, only after RX_BUSY_FRAMES
// without data. *line holds the screen line seen by the last call.
#define RX_BUSY_FRAMES 30
static void wait_rx(int *line, int *frames)
{
    if (*frames >= RX_BUSY_FRAMES)
    {
        cothread_yield_irq(IRQ_VBLANK);
        (*frames)++;
        return;
    }
    net_yield();
    int now = REG_VCOUNT;
    if (now < *line) // the line counter wrapped: a new frame has started
        (*frames)++;
    *line = now;
}

static bool send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    int spins = 0, frames = 0;

    while (len > 0)
    {
        int n = send(fd, p, len > 8192 ? 8192 : len, 0);
        if (n > 0)
        {
            p += n;
            len -= n;
            spins = frames = 0;
            continue;
        }
        if (would_block(n) && frames < IDLE_TIMEOUT_FRAMES)
        {
            wait_a_bit(&spins, &frames);
            continue;
        }
        return false;
    }
    return true;
}

static void reply(session_t *s, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 3)
        n = sizeof(buf) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    if (!send_all(s->ctrl, buf, n))
        session_close(s);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

// Combines cwd and arg into a normalized absolute virtual path.
static bool vpath_resolve(const char *cwd, const char *arg, char *out, size_t outsz)
{
    char tmp[VPATH_LEN * 2];
    if (arg == NULL || arg[0] == '\0')
        snprintf(tmp, sizeof(tmp), "%s", cwd);
    else if (arg[0] == '/')
        snprintf(tmp, sizeof(tmp), "%s", arg);
    else
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, arg);

    // Split into components and resolve "." and "..".
    char *parts[64];
    int np = 0;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save))
    {
        if (strcmp(tok, ".") == 0 || tok[0] == '\0')
            continue;
        if (strcmp(tok, "..") == 0)
        {
            if (np > 0)
                np--;
            continue;
        }
        if (np >= 64)
            return false;
        parts[np++] = tok;
    }

    size_t pos = 0;
    if (np == 0)
    {
        if (outsz < 2)
            return false;
        strcpy(out, "/");
        return true;
    }
    for (int i = 0; i < np; i++)
    {
        size_t l = strlen(parts[i]);
        if (pos + 1 + l + 1 > outsz)
            return false;
        out[pos++] = '/';
        memcpy(out + pos, parts[i], l);
        pos += l;
    }
    out[pos] = '\0';
    return true;
}

// Maps a normalized virtual path to a real libc path.
static vkind_t vpath_to_real(const char *vp, char *real, size_t sz)
{
    static const struct { const char *name; const char *drive; vkind_t kind; } map[] = {
        { "sd",    "sd:/",    V_SD },
        { "nand",  "nand:/",  V_NAND },
        { "nand2", "nand2:/", V_NAND2 },
    };

    if (strcmp(vp, "/") == 0)
    {
        real[0] = '\0';
        return V_ROOT;
    }

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    {
        size_t l = strlen(map[i].name);
        if (strncmp(vp + 1, map[i].name, l) == 0 && (vp[1 + l] == '\0' || vp[1 + l] == '/'))
        {
            // A drive without read permission doesn't exist for the client.
            if (map[i].kind == V_SD ? !perms.sd_read : (!nand_ok || !perms.nand_read))
                return V_INVALID;
            const char *rest = vp + 1 + l;
            if (*rest == '/')
                rest++;
            if (snprintf(real, sz, "%s%s", map[i].drive, rest) >= (int)sz)
                return V_INVALID;
            return map[i].kind;
        }
    }
    return V_INVALID;
}

static const char *base_name(const char *vp)
{
    const char *b = strrchr(vp, '/');
    return (b && b[1]) ? b + 1 : vp;
}

static bool is_drive_root(const char *real)
{
    size_t l = strlen(real);
    return l > 0 && real[l - 1] == '/' && strchr(real, '/') == real + l - 1;
}

// Returns 1 = directory, 0 = file, -1 = does not exist.
static int path_type(vkind_t k, const char *real, struct stat *st)
{
    if (k == V_ROOT)
        return 1;
    if (k == V_INVALID)
        return -1;
    if (is_drive_root(real))
    {
        DIR *d = opendir(real);
        if (!d)
            return -1;
        closedir(d);
        if (st)
            memset(st, 0, sizeof(*st));
        return 1;
    }
    struct stat tmp;
    struct stat *p = st ? st : &tmp;
    if (stat(real, p) != 0)
        return -1;
    return S_ISDIR(p->st_mode) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// NAND write mode: protected paths, backups
// ---------------------------------------------------------------------------

// Paths that stay read-only even in write mode: damaging them can brick the
// console (Unlaunch lives in the launcher's title.tmd) and there's no software
// recovery. Compared case-insensitively (FAT), on path component boundaries.
static const char *const protected_paths[] = {
    "/nand/sys",                  // HWINFO, HWID.sgn, cert.sys, font, logs
    "/nand/shared1",              // TWLCFG0/1: system settings
    "/nand/title/00030017",       // launcher (DSi Menu) + Unlaunch
    "/nand/ticket/00030017",
    "/nand/title/0003000f",       // system data: Wi-Fi firmware, whitelist, version
    "/nand/ticket/0003000f",
    "/nand/title/00030004/48465441",        // DSi FTP itself, if installed to the NAND
    "/nand/ticket/00030004/48465441.tik",
};

static bool path_has_prefix(const char *vp, const char *prefix)
{
    size_t l = strlen(prefix);
    return strncasecmp(vp, prefix, l) == 0 && (vp[l] == '\0' || vp[l] == '/');
}

static int path_depth(const char *vp)
{
    int d = 0;
    for (const char *p = vp; *p; p++)
        if (*p == '/')
            d++;
    return strcmp(vp, "/") == 0 ? 0 : d;
}

// True if the path holds a name the filesystem resolves differently from how
// it is written: FatFs drops a trailing dot or space, takes '\' as a
// separator and ':' as a drive prefix, and '~' marks 8.3 aliases. Without
// this check "/nand/sys./HWINFO" would slip past the protected paths.
static bool has_ambiguous_name(const char *vp)
{
    for (const char *p = vp; *p; p++)
    {
        if (*p == '\\' || *p == ':' || *p == '~')
            return true;
        if ((*p == '.' || *p == ' ') && (p[1] == '/' || p[1] == '\0'))
            return true;
    }
    return false;
}

// NULL if a write to vp is allowed, otherwise the reason it is refused.
static const char *write_refusal(vkind_t k, const char *vp, const char *real)
{
    if (k == V_SD)
    {
        if (!perms.sd_write)
            return "SD is read only (permissions on the DSi)";
        return is_drive_root(real) ? "the SD root cannot be changed" : NULL;
    }
    if (k != V_NAND && k != V_NAND2)
        return "path not writable";
    if (!perms.nand_write)
        return "NAND is read only (permissions on the DSi)";
    if (is_drive_root(real))
        return "the NAND root cannot be changed";
    if (ftp_nand_full_active())
        return NULL; // EXPERIMENTAL full write: no protected paths (backups still made)
    if (!perms.nand_loose_names && has_ambiguous_name(vp))
        return "name not allowed on the NAND: trailing dot or space, or \\ : ~ "
               "(nand_strict_names in settings.ini)";
    if (path_depth(vp) <= 2)
        return "the top-level NAND folders are protected";
    for (size_t i = 0; i < sizeof(protected_paths) / sizeof(protected_paths[0]); i++)
    {
        if (path_has_prefix(vp, protected_paths[i]))
            return "protected system path (brick risk)";
    }
    return NULL;
}

// mkdir -p for a path like "sd:/a/b/c" (the last component is created too).
static bool mkdirs(const char *path)
{
    char tmp[RPATH_LEN + 160];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
        return false; // path too long: refuse rather than truncate
    char *start = strchr(tmp, '/'); // skip "sd:"
    if (!start)
        return false;
    for (char *p = start + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
    struct stat st;
    return stat(tmp, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in)
        return false;
    FILE *out = fopen(dst, "wb");
    if (!out)
    {
        fclose(in);
        return false;
    }
    bool ok = true;
    size_t n;
    while ((n = fread(xbuf, 1, XFER_BUF_SIZE, in)) > 0)
    {
        if (fwrite(xbuf, 1, n, out) != n)
        {
            ok = false;
            break;
        }
    }
    if (ferror(in))
        ok = false;
    fclose(in);
    if (fclose(out) != 0)
        ok = false;
    return ok;
}

// Per-session folder for backups and the write journal (created on first use).
static bool ensure_backup_dir(void)
{
    if (backup_dir[0] == '\0')
    {
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
        snprintf(backup_dir, sizeof(backup_dir), BACKUP_ROOT "/%s", stamp);
    }
    return mkdirs(backup_dir);
}

// Journal of every NAND write, next to its backups. Always on, independent
// of the diagnostic log setting.
static void nand_journal(const char *fmt, ...)
{
    if (!ensure_backup_dir())
        return;
    char path[sizeof(backup_dir) + 16];
    snprintf(path, sizeof(path), "%s/journal.txt", backup_dir);
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char stamp[24];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(f, "%s %s", stamp, ftp_nand_full_active() ? "[FULL] " : "");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// Copies an existing NAND file to the SD backup folder, keeping its path.
// Must succeed before the file is overwritten or deleted.
static bool backup_nand_file(const char *vp, const char *real)
{
    if (!ensure_backup_dir())
    {
        ui_msg(S_LOG_BACKUP_DIR_FAIL);
        return false;
    }
    char dst[RPATH_LEN + 128];
    snprintf(dst, sizeof(dst), "%s%s", backup_dir, vp); // vp starts with /nand...
    char dir[sizeof(dst)];
    snprintf(dir, sizeof(dir), "%s", dst);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return false;
    *slash = '\0';
    // Don't overwrite an earlier backup of the same file in this session:
    // the first copy is the original.
    struct stat st;
    if (stat(dst, &st) == 0)
        return true;
    // Copy under a temporary name and give it the final one only when it is
    // complete and as long as the original: an interrupted copy must never
    // pass for a backup on the next attempt.
    char part[sizeof(dst) + 16];
    snprintf(part, sizeof(part), "%s.dsiftp.tmp", dst);
    struct stat src_st, part_st;
    bool ok = mkdirs(dir) && copy_file(real, part) && stat(real, &src_st) == 0 &&
              stat(part, &part_st) == 0 && part_st.st_size == src_st.st_size &&
              rename(part, dst) == 0;
    if (!ok)
    {
        unlink(part);
        ui_msg(S_LOG_BACKUP_FAIL, vp);
        nand_journal("backup FAILED %s (operation cancelled)", vp);
        return false;
    }
    backup_count++;
    ui_msg(S_LOG_BACKUP, vp);
    nand_journal("backup %s", vp);
    return true;
}

// ---------------------------------------------------------------------------
// Login lock per client address, connected clients
// ---------------------------------------------------------------------------

static const char *ip_str(uint32_t ip)
{
    struct in_addr a = { .s_addr = ip };
    return inet_ntoa(a); // static buffer: one call per expression
}

static int lock_find(uint32_t ip, bool create)
{
    int oldest = 0;
    for (int i = 0; i < LOCK_SLOTS; i++)
    {
        if (lock_tab[i].ip == ip)
            return i;
        if (lock_tab[i].last < lock_tab[oldest].last)
            oldest = i;
    }
    if (!create)
        return -1;
    for (int i = 0; i < LOCK_SLOTS; i++)
    {
        if (lock_tab[i].ip == 0)
        {
            oldest = i;
            break;
        }
    }
    memset(&lock_tab[oldest], 0, sizeof(lock_tab[oldest]));
    lock_tab[oldest].ip = ip;
    return oldest;
}

static int lock_remaining(uint32_t ip, time_t now)
{
    if (!lock_cfg.enabled)
        return 0;
    int i = lock_find(ip, false);
    return (i >= 0 && lock_tab[i].locked_until > now) ? (int)(lock_tab[i].locked_until - now) : 0;
}

// Counts a wrong password; returns true if the address has just been locked.
static bool lock_fail(uint32_t ip, time_t now)
{
    if (!lock_cfg.enabled)
        return false;
    int i = lock_find(ip, true);
    lock_tab[i].last = now;
    if (++lock_tab[i].fails < lock_cfg.max_fails)
        return false;
    lock_tab[i].fails = 0;
    lock_tab[i].locked_until = now + lock_cfg.seconds;
    return true;
}

static void lock_clear(uint32_t ip)
{
    int i = lock_find(ip, false);
    if (i >= 0)
    {
        lock_tab[i].fails = 0;
        lock_tab[i].locked_until = 0;
    }
}

void ftp_set_lock(const ftp_lock_cfg_t *c)
{
    lock_cfg = *c;
    if (lock_cfg.max_fails < 1)
        lock_cfg.max_fails = 1;
    if (lock_cfg.seconds < 1)
        lock_cfg.seconds = 1;
}

const ftp_lock_cfg_t *ftp_get_lock(void)
{
    return &lock_cfg;
}

int ftp_get_blocked(ftp_blocked_t *out, int max)
{
    time_t now = time(NULL);
    int n = 0;
    for (int i = 0; i < LOCK_SLOTS && n < max; i++)
    {
        if (lock_cfg.enabled && lock_tab[i].ip && lock_tab[i].locked_until > now)
        {
            out[n].ip = lock_tab[i].ip;
            out[n].remaining = (int)(lock_tab[i].locked_until - now);
            n++;
        }
    }
    return n;
}

void ftp_unblock_all(void)
{
    memset(lock_tab, 0, sizeof(lock_tab));
    ui_msg(S_LOG_UNBLOCKED_ALL);
}

int ftp_get_clients(ftp_client_t *out, int max)
{
    int n = 0;
    for (int i = 0; ftp_ready && i < MAX_SESSIONS && n < max; i++)
    {
        if (sess[i].ctrl >= 0)
        {
            out[n].ip = sess[i].peer;
            out[n].authed = sess[i].authed;
            n++;
        }
    }
    return n;
}

void ftp_disconnect_all(void)
{
    for (int i = 0; ftp_ready && i < MAX_SESSIONS; i++)
    {
        if (sess[i].ctrl >= 0)
        {
            const char *bye = "421 Disconnected by the DSi\r\n";
            send(sess[i].ctrl, bye, strlen(bye), 0);
            session_close(&sess[i]);
        }
    }
    ui_msg(S_LOG_CLIENTS_KICKED);
}

const ftp_attempt_t *ftp_get_last_attempt(void)
{
    return &last_attempt;
}

void ftp_set_login(const char *user, const char *pass, bool is_pin)
{
    snprintf(login_user, sizeof(login_user), "%s", user ? user : "");
    snprintf(login_pass, sizeof(login_pass), "%s", pass ? pass : "");
    login_is_pin = is_pin;
}

void ftp_preinit(bool nand_available)
{
    nand_ok = nand_available;
}

// The flags are independent; what is actually allowed follows these rules:
// a drive without read is hidden (so its write has no effect), and full
// write only acts on top of an active protected write.
bool ftp_nand_writable(void)
{
    return nand_ok && perms.nand_read && perms.nand_write;
}

bool ftp_nand_full_active(void)
{
    return ftp_nand_writable() && perms.nand_full;
}

void ftp_set_perms(const ftp_perms_t *p)
{
    bool old_w = ftp_nand_writable(), old_f = ftp_nand_full_active();
    perms = *p;
    bool w = ftp_nand_writable(), f = ftp_nand_full_active();

    if (w != old_w || f != old_f)
    {
        ui_msg(f ? S_LOG_NAND_FULL_ON : w ? S_LOG_NAND_W_ON : S_LOG_NAND_RO);
        nand_journal("permissions: writing %s%s", f ? "FULL (experimental)" : w ? "protected" : "off",
                     w && !f && perms.nand_loose_names ? " (strict names OFF)" : "");
    }
}

const ftp_perms_t *ftp_get_perms(void)
{
    return &perms;
}

// ---------------------------------------------------------------------------
// Data connection (passive mode)
// ---------------------------------------------------------------------------

static int open_pasv(session_t *s)
{
    sock_close(&s->pasv);

    for (int tries = 0; tries < 20; tries++)
    {
        int port = PASV_BASE + (pasv_counter++ % PASV_RANGE);
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;

        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = INADDR_ANY;

        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(fd, 1) == 0)
        {
            set_nonblock(fd);
            s->pasv = fd;
            return port;
        }
        closesocket(fd);
    }
    return -1;
}

static int accept_data(session_t *s)
{
    if (s->pasv < 0)
        return -1;

    int spins = 0, frames = 0;
    while (frames < ACCEPT_TIMEOUT_FRAMES)
    {
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        int fd = accept(s->pasv, (struct sockaddr *)&sa, &sl);
        if (fd >= 0)
        {
            sock_close(&s->pasv);
            set_nonblock(fd);
            return fd;
        }
        if (!would_block(fd))
            break;
        wait_a_bit(&spins, &frames);
    }
    sock_close(&s->pasv);
    return -1;
}

// ---------------------------------------------------------------------------
// Listings
// ---------------------------------------------------------------------------

static void format_entry(char *out, size_t sz, const char *name, bool dir,
                         long long size, time_t mtime, bool ro, bool names_only)
{
    if (names_only)
    {
        snprintf(out, sz, "%s\r\n", name);
        return;
    }

    char date[32];
    struct tm tm;
    time_t now = time(NULL);
    gmtime_r(&mtime, &tm);
    if (mtime <= now + 3600 && now - mtime < (time_t)180 * 24 * 3600)
        strftime(date, sizeof(date), "%b %e %H:%M", &tm);
    else
        strftime(date, sizeof(date), "%b %e  %Y", &tm);

    const char *perm = dir ? (ro ? "dr-xr-xr-x" : "drwxr-xr-x")
                           : (ro ? "-r--r--r--" : "-rw-r--r--");
    snprintf(out, sz, "%s 1 dsi dsi %lld %s %s\r\n", perm, size, date, name);
}

static bool send_listing(session_t *s, int dfd, vkind_t k, const char *vp,
                         const char *real, bool names_only)
{
    char line[600];
    bool ro = (k == V_SD) ? !perms.sd_write : !perms.nand_write;
    time_t now = time(NULL);

    if (k == V_ROOT)
    {
        // Only the drives with read permission are listed.
        if (perms.sd_read)
        {
            format_entry(line, sizeof(line), "sd", true, 0, now, !perms.sd_write, names_only);
            if (!send_all(dfd, line, strlen(line)))
                return false;
        }
        if (nand_ok && perms.nand_read)
        {
            format_entry(line, sizeof(line), "nand", true, 0, now, !perms.nand_write, names_only);
            if (!send_all(dfd, line, strlen(line)))
                return false;
            format_entry(line, sizeof(line), "nand2", true, 0, now, !perms.nand_write, names_only);
            if (!send_all(dfd, line, strlen(line)))
                return false;
        }
        return true;
    }

    struct stat st;
    int t = path_type(k, real, &st);
    if (t == 0)
    {
        format_entry(line, sizeof(line), base_name(vp), false, (long long)st.st_size,
                     st.st_mtime, ro, names_only);
        return send_all(dfd, line, strlen(line));
    }

    DIR *d = opendir(real);
    if (!d)
        return false;

    char child[RPATH_LEN];
    size_t rl = strlen(real);
    bool slash = rl > 0 && real[rl - 1] == '/';
    struct dirent *de;
    bool ok = true;

    while (ok && (de = readdir(d)) != NULL)
    {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        snprintf(child, sizeof(child), "%s%s%s", real, slash ? "" : "/", de->d_name);
        struct stat cst;
        memset(&cst, 0, sizeof(cst));
        bool isdir;
        if (stat(child, &cst) == 0)
            isdir = S_ISDIR(cst.st_mode);
        else
            isdir = (de->d_type == DT_DIR);
        format_entry(line, sizeof(line), de->d_name, isdir, isdir ? 0 : (long long)cst.st_size,
                     cst.st_mtime, ro, names_only);
        ok = send_all(dfd, line, strlen(line));
    }
    closedir(d);
    return ok;
}

// ---------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------

// Speed test: a transfer from or to this name, in any folder, never touches
// the SD or the NAND, so it measures the Wi-Fi link alone. For example:
//   curl -o NUL ftp://USER:PASS@IP/.dsiftp-speedtest        (download)
//   curl -T bigfile ftp://USER:PASS@IP/.dsiftp-speedtest    (upload)
#define BENCH_NAME  ".dsiftp-speedtest"
#define BENCH_BYTES (32u * 1024 * 1024)

static bool is_bench(const char *vp)
{
    return strcmp(base_name(vp), BENCH_NAME) == 0;
}

static void bench_retr(session_t *s)
{
    s->rest = 0;
    reply(s, "150 Speed test: sending %u bytes", (unsigned)BENCH_BYTES);
    int dfd = accept_data(s);
    if (dfd < 0)
    {
        reply(s, "425 Data connection failed");
        return;
    }
    memset(xbuf, 0, XFER_BUF_SIZE);
    bool ok = true;
    for (size_t left = BENCH_BYTES; ok && left > 0; left -= XFER_BUF_SIZE)
        ok = send_all(dfd, xbuf, XFER_BUF_SIZE);
    sock_close(&dfd);
    reply(s, ok ? "226 Transfer complete" : "426 Transfer aborted");
}

static void bench_stor(session_t *s)
{
    s->rest = 0;
    reply(s, "150 Speed test: the data is discarded");
    int dfd = accept_data(s);
    if (dfd < 0)
    {
        reply(s, "425 Data connection failed");
        return;
    }
    bool ok = true;
    int spins = 0, frames = 0;
    for (;;)
    {
        int n = recv(dfd, xbuf, XFER_BUF_SIZE, 0);
        if (n > 0)
        {
            spins = frames = 0;
            continue;
        }
        if (n == 0)
            break;
        if (would_block(n) && frames < IDLE_TIMEOUT_FRAMES)
        {
            wait_rx(&spins, &frames);
            continue;
        }
        ok = false;
        break;
    }
    sock_close(&dfd);
    reply(s, ok ? "226 Speed test done" : "426 Transfer aborted");
}

static void cmd_retr(session_t *s, const char *arg)
{
    char vp[VPATH_LEN], real[RPATH_LEN];
    if (!vpath_resolve(s->cwd, arg, vp, sizeof(vp)))
    {
        reply(s, "553 Invalid path");
        return;
    }
    if (is_bench(vp))
    {
        bench_retr(s);
        return;
    }
    vkind_t k = vpath_to_real(vp, real, sizeof(real));
    struct stat st;
    if (path_type(k, real, &st) != 0)
    {
        reply(s, "550 File not found");
        return;
    }

    FILE *f = fopen(real, "rb");
    if (!f)
    {
        reply(s, "550 Cannot open the file");
        return;
    }
    if (s->rest > 0 && fseek(f, (long)s->rest, SEEK_SET) != 0)
    {
        fclose(f);
        s->rest = 0;
        reply(s, "554 Invalid REST offset");
        return;
    }
    long long start = s->rest;
    s->rest = 0;

    reply(s, "150 Sending %s (%lld bytes)", base_name(vp), (long long)st.st_size - start);
    int dfd = accept_data(s);
    if (dfd < 0)
    {
        fclose(f);
        reply(s, "425 Data connection failed");
        return;
    }

    bool ok = true; // the command itself is already logged by handle_command()
    size_t n;
    while ((n = fread(xbuf, 1, XFER_BUF_SIZE, f)) > 0)
    {
        if (!send_all(dfd, xbuf, n))
        {
            ok = false;
            break;
        }
    }
    if (ferror(f))
        ok = false;
    fclose(f);
    sock_close(&dfd);

    if (ok)
        reply(s, "226 Transfer complete");
    else
        reply(s, "426 Transfer aborted");
}

static void cmd_stor(session_t *s, const char *arg, bool append)
{
    char vp[VPATH_LEN], real[RPATH_LEN];
    if (!vpath_resolve(s->cwd, arg, vp, sizeof(vp)))
    {
        reply(s, "553 Invalid path");
        return;
    }
    if (is_bench(vp))
    {
        bench_stor(s);
        return;
    }
    // REST is only honoured by RETR. A resumed STOR would replace the file
    // with just its tail, so it is refused; APPE simply appends.
    long long rest = s->rest;
    s->rest = 0;
    if (rest > 0 && !append)
    {
        reply(s, "554 Resuming an upload is not supported: send the whole file");
        return;
    }
    vkind_t k = vpath_to_real(vp, real, sizeof(real));
    const char *why = write_refusal(k, vp, real);
    if (why)
    {
        reply(s, "550 Write refused: %s", why);
        return;
    }
    int type = path_type(k, real, NULL);
    if (type == 1)
    {
        reply(s, "550 It is a folder");
        return;
    }

    // On the NAND: never APPE, back up the original, and write to a temporary
    // file that replaces the original only once the upload is complete.
    bool on_nand = (k != V_SD);
    char tmp[RPATH_LEN + 16];
    const char *target = real;
    if (on_nand)
    {
        if (append)
        {
            reply(s, "550 APPE not allowed on the NAND");
            return;
        }
        if (type == 0 && !backup_nand_file(vp, real))
        {
            reply(s, "550 Backup to the SD failed: operation cancelled");
            return;
        }
        snprintf(tmp, sizeof(tmp), "%s.dsiftp.tmp", real);
        target = tmp;
    }

    reply(s, "150 Receiving %s", base_name(vp));
    int dfd = accept_data(s);
    if (dfd < 0)
    {
        reply(s, "425 Data connection failed");
        return;
    }

    // The file is created (and an existing one truncated) only now that the
    // data connection is up: a blocked passive port must not empty it.
    FILE *f = fopen(target, append ? "ab" : "wb");
    if (!f)
    {
        sock_close(&dfd);
        reply(s, "553 Cannot create the file");
        return;
    }

    if (on_nand)
        ui_msg(S_LOG_NAND_WRITING, vp);
    // recv() hands over a few KB at a time (lwIP's TCP window). Writing each
    // piece on its own means many small, unaligned storage requests, and the
    // Wi-Fi cannot acknowledge anything while one is running (see sdlock.c):
    // collect a full buffer first.
    bool ok = true, wr_ok = true;
    size_t fill = 0;
    int spins = 0, frames = 0;
    for (;;)
    {
        int n = recv(dfd, xbuf + fill, XFER_BUF_SIZE - fill, 0);
        if (n > 0)
        {
            spins = frames = 0;
            fill += n;
            if (fill == XFER_BUF_SIZE)
            {
                fill = 0;
                if (fwrite(xbuf, 1, XFER_BUF_SIZE, f) != XFER_BUF_SIZE)
                {
                    ok = wr_ok = false;
                    break;
                }
            }
            continue;
        }
        if (n == 0)
            break; // client closed: upload finished
        if (would_block(n) && frames < IDLE_TIMEOUT_FRAMES)
        {
            wait_rx(&spins, &frames);
            continue;
        }
        ok = false;
        break;
    }
    // What is left in the buffer is valid data even if the transfer broke off.
    if (wr_ok && fill > 0 && fwrite(xbuf, 1, fill, f) != fill)
        ok = false;
    if (fclose(f) != 0)
        ok = false;
    sock_close(&dfd);

    if (on_nand)
    {
        bool stranded = false; // original left under its .old name
        if (ok)
        {
            // Swap in the new file: original -> .old, temporary -> original
            // name, delete .old. FAT cannot rename over an existing file, so
            // between the two renames the name is briefly missing; the
            // original then survives as .old and in the SD backup.
            if (type == 0)
            {
                char old[RPATH_LEN + 16];
                snprintf(old, sizeof(old), "%s.dsiftp.old", real);
                unlink(old);
                if (rename(real, old) != 0)
                {
                    ok = false;
                }
                else if (rename(tmp, real) != 0)
                {
                    ok = false;
                    if (rename(old, real) != 0)
                    {
                        stranded = true;
                        ui_msg(S_LOG_RESTORE_WARN, vp);
                    }
                }
                else
                {
                    unlink(old);
                }
            }
            else if (rename(tmp, real) != 0)
            {
                ok = false;
            }
        }
        if (!ok)
            unlink(tmp);
        ui_msg(ok ? S_LOG_NAND_WRITTEN : S_LOG_NAND_NOT_WRITTEN, vp);
        nand_journal("write %s %s", vp,
                     ok ? "ok"
                     : stranded ? "FAILED: original NOT in place, it is now <name>.dsiftp.old "
                                  "(also in this backup folder)"
                     : type == 0 ? "FAILED (original intact)" : "FAILED (nothing written)");
    }

    if (ok)
        reply(s, "226 File saved");
    else
        reply(s, "451 Error while saving");
}

static void cmd_list(session_t *s, const char *arg, bool names_only)
{
    // Skip "ls" style options such as "-la".
    while (arg && *arg == '-')
    {
        while (*arg && *arg != ' ')
            arg++;
        while (*arg == ' ')
            arg++;
    }

    char vp[VPATH_LEN], real[RPATH_LEN];
    if (!vpath_resolve(s->cwd, arg, vp, sizeof(vp)))
    {
        reply(s, "553 Invalid path");
        return;
    }
    vkind_t k = vpath_to_real(vp, real, sizeof(real));
    if (path_type(k, real, NULL) < 0)
    {
        reply(s, "550 Folder not found");
        return;
    }

    reply(s, "150 Sending list");
    int dfd = accept_data(s);
    if (dfd < 0)
    {
        reply(s, "425 Data connection failed");
        return;
    }
    bool ok = send_listing(s, dfd, k, vp, real, names_only);
    sock_close(&dfd);
    if (ok)
        reply(s, "226 List sent");
    else
        reply(s, "451 Error while listing");
}

// ---------------------------------------------------------------------------
// Command dispatcher
// ---------------------------------------------------------------------------

// Resolves arg and checks that it may be modified: always on the SD, on the
// NAND only in write mode and outside the protected system paths.
static bool writable_target(session_t *s, const char *arg, char *vp, char *real, vkind_t *kind)
{
    if (!vpath_resolve(s->cwd, arg, vp, VPATH_LEN))
    {
        reply(s, "553 Invalid path");
        return false;
    }
    vkind_t k = vpath_to_real(vp, real, RPATH_LEN);
    const char *why = write_refusal(k, vp, real);
    if (why)
    {
        reply(s, "550 Operation refused: %s", why);
        return false;
    }
    if (kind)
        *kind = k;
    return true;
}

static void handle_command(session_t *s, char *line)
{
    // Strip Telnet control bytes some clients send before ABOR.
    while (*line && !isalpha((unsigned char)*line))
        line++;

    char *arg = strchr(line, ' ');
    if (arg)
    {
        *arg++ = '\0';
        while (*arg == ' ')
            arg++;
    }
    else
    {
        arg = line + strlen(line);
    }
    for (char *p = line; *p; p++)
        *p = toupper((unsigned char)*p);

    const char *cmd = line;
    char vp[VPATH_LEN], real[RPATH_LEN];

    if (strcmp(cmd, "PASS") != 0)
        ui_log("%s %s", cmd, arg);

    // Commands allowed before login
    if (strcmp(cmd, "USER") == 0)
    {
        s->got_user = true;
        s->authed = false;
        snprintf(s->user, sizeof(s->user), "%s", arg);
        reply(s, login_is_pin ? "331 Password: the PIN shown on the DSi"
                              : "331 Password required");
        return;
    }
    if (strcmp(cmd, "PASS") == 0)
    {
        // Lock per client address after too many wrong attempts, over any
        // number of connections: a 4-digit PIN could otherwise be guessed by
        // reconnecting. Other addresses (e.g. the owner's PC) are not affected.
        time_t now = time(NULL);
        int remaining = lock_remaining(s->peer, now);
        if (remaining > 0)
        {
            reply(s, "530 Too many failed attempts: retry in %d s", remaining);
            return;
        }
        bool user_ok = login_user[0] == '\0' || strcmp(s->user, login_user) == 0;
        bool ok = s->got_user && user_ok && login_pass[0] && strcmp(arg, login_pass) == 0;
        last_attempt.ip = s->peer;
        last_attempt.ok = ok;
        last_attempt.when = now;
        last_attempt.valid = true;
        if (ok)
        {
            s->authed = true;
            s->fails = 0;
            lock_clear(s->peer);
            ui_msg(S_LOG_LOGIN_OK, ip_str(s->peer));
            reply(s, "230 Logged in");
        }
        else
        {
            s->fails++;
            ui_msg(S_LOG_LOGIN_BAD, ip_str(s->peer));
            if (lock_fail(s->peer, now))
                ui_msg(S_LOG_IP_BLOCKED, ip_str(s->peer), lock_cfg.seconds);
            reply(s, login_is_pin ? "530 Wrong PIN" : "530 Wrong user or password");
            if (s->fails >= MAX_LOGIN_FAILS)
                session_close(s);
        }
        return;
    }
    if (strcmp(cmd, "QUIT") == 0)
    {
        reply(s, "221 Goodbye");
        session_close(s);
        return;
    }
    if (strcmp(cmd, "SYST") == 0)
    {
        reply(s, "215 UNIX Type: L8");
        return;
    }
    if (strcmp(cmd, "FEAT") == 0)
    {
        reply(s, "211-Features:\r\n SIZE\r\n MDTM\r\n PASV\r\n EPSV\r\n REST STREAM\r\n UTF8\r\n211 End");
        return;
    }
    if (strcmp(cmd, "OPTS") == 0 || strcmp(cmd, "CLNT") == 0 || strcmp(cmd, "NOOP") == 0)
    {
        reply(s, "200 OK");
        return;
    }
    if (strcmp(cmd, "AUTH") == 0 || strcmp(cmd, "PBSZ") == 0 || strcmp(cmd, "PROT") == 0)
    {
        reply(s, "502 TLS not supported");
        return;
    }

    if (!s->authed)
    {
        reply(s, "530 Please log in first");
        return;
    }

    if (strcmp(cmd, "PWD") == 0 || strcmp(cmd, "XPWD") == 0)
    {
        reply(s, "257 \"%s\" is the current folder", s->cwd);
    }
    else if (strcmp(cmd, "CWD") == 0 || strcmp(cmd, "XCWD") == 0 || strcmp(cmd, "CDUP") == 0 ||
             strcmp(cmd, "XCUP") == 0)
    {
        const char *target = (cmd[0] == 'C' && cmd[1] == 'D') || strcmp(cmd, "XCUP") == 0 ? ".." : arg;
        if (!vpath_resolve(s->cwd, target, vp, sizeof(vp)))
        {
            reply(s, "553 Invalid path");
            return;
        }
        vkind_t k = vpath_to_real(vp, real, sizeof(real));
        if (path_type(k, real, NULL) != 1)
        {
            reply(s, "550 Folder not found");
            return;
        }
        snprintf(s->cwd, sizeof(s->cwd), "%s", vp);
        reply(s, "250 Folder: %s", s->cwd);
    }
    else if (strcmp(cmd, "TYPE") == 0 || strcmp(cmd, "MODE") == 0 || strcmp(cmd, "STRU") == 0)
    {
        reply(s, "200 OK");
    }
    else if (strcmp(cmd, "PASV") == 0)
    {
        int port = open_pasv(s);
        if (port < 0)
        {
            reply(s, "425 Cannot open the data port");
            return;
        }
        uint32_t ip = ntohl(my_ip);
        reply(s, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
              (unsigned)(ip >> 24) & 0xFF, (unsigned)(ip >> 16) & 0xFF,
              (unsigned)(ip >> 8) & 0xFF, (unsigned)ip & 0xFF,
              (unsigned)(port >> 8) & 0xFF, (unsigned)port & 0xFF);
    }
    else if (strcmp(cmd, "EPSV") == 0)
    {
        int port = open_pasv(s);
        if (port < 0)
        {
            reply(s, "425 Cannot open the data port");
            return;
        }
        reply(s, "229 Entering Extended Passive Mode (|||%d|)", port);
    }
    else if (strcmp(cmd, "PORT") == 0 || strcmp(cmd, "EPRT") == 0)
    {
        reply(s, "502 Use passive mode (PASV)");
    }
    else if (strcmp(cmd, "LIST") == 0)
    {
        cmd_list(s, arg, false);
    }
    else if (strcmp(cmd, "NLST") == 0)
    {
        cmd_list(s, arg, true);
    }
    else if (strcmp(cmd, "RETR") == 0)
    {
        cmd_retr(s, arg);
    }
    else if (strcmp(cmd, "STOR") == 0)
    {
        cmd_stor(s, arg, false);
    }
    else if (strcmp(cmd, "APPE") == 0)
    {
        cmd_stor(s, arg, true);
    }
    else if (strcmp(cmd, "REST") == 0)
    {
        s->rest = strtoll(arg, NULL, 10);
        if (s->rest < 0)
            s->rest = 0;
        reply(s, "350 Restarting at %lld", s->rest);
    }
#ifdef DSIFTP_TUNING
    else if (strcmp(cmd, "XWAIT") == 0)
    {
        int v = atoi(arg);
        wait_spins = (v >= 1 && v <= 1000000) ? v : 64;
        reply(s, "200 Quick retries before sleeping: %d", wait_spins);
    }
#endif
    else if (strcmp(cmd, "SIZE") == 0 || strcmp(cmd, "MDTM") == 0)
    {
        struct stat st;
        if (!vpath_resolve(s->cwd, arg, vp, sizeof(vp)))
        {
            reply(s, "550 File not found");
            return;
        }
        if (cmd[0] == 'S' && is_bench(vp)) // curl gives up if SIZE fails
        {
            reply(s, "213 %u", (unsigned)BENCH_BYTES);
            return;
        }
        if (path_type(vpath_to_real(vp, real, sizeof(real)), real, &st) != 0)
        {
            reply(s, "550 File not found");
            return;
        }
        if (cmd[0] == 'S')
        {
            reply(s, "213 %lld", (long long)st.st_size);
        }
        else
        {
            char date[32];
            struct tm tm;
            gmtime_r(&st.st_mtime, &tm);
            strftime(date, sizeof(date), "%Y%m%d%H%M%S", &tm);
            reply(s, "213 %s", date);
        }
    }
    else if (strcmp(cmd, "DELE") == 0)
    {
        vkind_t k;
        if (!writable_target(s, arg, vp, real, &k))
            return;
        if (path_type(k, real, NULL) != 0)
        {
            reply(s, "550 File not found");
            return;
        }
        if (k != V_SD && !backup_nand_file(vp, real))
        {
            reply(s, "550 Backup to the SD failed: operation cancelled");
            return;
        }
        bool ok = unlink(real) == 0;
        if (k != V_SD)
        {
            ui_msg(ok ? S_LOG_NAND_DELETED : S_LOG_NAND_NOT_DELETED, vp);
            nand_journal("delete %s %s", vp, ok ? "ok" : "FAILED");
        }
        if (ok)
            reply(s, "250 File deleted");
        else
            reply(s, "550 Cannot delete");
    }
    else if (strcmp(cmd, "MKD") == 0 || strcmp(cmd, "XMKD") == 0)
    {
        vkind_t k;
        if (!writable_target(s, arg, vp, real, &k))
            return;
        bool ok = mkdir(real, 0777) == 0;
        if (k != V_SD)
        {
            ui_msg(ok ? S_LOG_NAND_MKDIR : S_LOG_NAND_MKDIR_FAIL, vp);
            nand_journal("mkdir %s %s", vp, ok ? "ok" : "FAILED");
        }
        if (ok)
            reply(s, "257 \"%s\" created", vp);
        else
            reply(s, "550 Cannot create the folder");
    }
    else if (strcmp(cmd, "RMD") == 0 || strcmp(cmd, "XRMD") == 0)
    {
        vkind_t k;
        if (!writable_target(s, arg, vp, real, &k))
            return;
        bool ok = rmdir(real) == 0; // only empty folders: their files were backed up
        if (k != V_SD)
        {
            ui_msg(ok ? S_LOG_NAND_RMDIR : S_LOG_NAND_RMDIR_FAIL, vp);
            nand_journal("rmdir %s %s", vp, ok ? "ok" : "FAILED");
        }
        if (ok)
            reply(s, "250 Folder deleted");
        else
            reply(s, "550 Cannot delete (the folder must be empty)");
    }
    else if (strcmp(cmd, "RNFR") == 0)
    {
        vkind_t k;
        if (!writable_target(s, arg, vp, real, &k))
            return;
        if (path_type(k, real, NULL) < 0)
        {
            reply(s, "550 Not found");
            return;
        }
        snprintf(s->rnfr, sizeof(s->rnfr), "%s", vp);
        reply(s, "350 Ready for RNTO");
    }
    else if (strcmp(cmd, "RNTO") == 0)
    {
        char from_real[RPATH_LEN];
        if (s->rnfr[0] == '\0')
        {
            reply(s, "503 RNFR missing");
            return;
        }
        vkind_t k;
        if (!writable_target(s, arg, vp, real, &k))
        {
            s->rnfr[0] = '\0';
            return;
        }
        // Re-check the source: write mode may have been turned off meanwhile.
        vkind_t kf = vpath_to_real(s->rnfr, from_real, sizeof(from_real));
        const char *why = write_refusal(kf, s->rnfr, from_real);
        if (why || kf != k)
        {
            s->rnfr[0] = '\0';
            reply(s, "550 Rename refused: %s", why ? why : "source and target on different drives");
            return;
        }
        bool ok = rename(from_real, real) == 0;
        if (k != V_SD)
        {
            ui_msg(ok ? S_LOG_NAND_RENAMED : S_LOG_NAND_RENAME_FAIL, s->rnfr, vp);
            nand_journal("rename %s -> %s %s", s->rnfr, vp, ok ? "ok" : "FAILED");
        }
        s->rnfr[0] = '\0';
        if (ok)
            reply(s, "250 Renamed");
        else
            reply(s, "550 Cannot rename");
    }
    else if (strcmp(cmd, "ABOR") == 0)
    {
        sock_close(&s->pasv);
        reply(s, "226 No transfer in progress");
    }
    else if (strcmp(cmd, "ALLO") == 0)
    {
        reply(s, "202 Not needed");
    }
    else if (strcmp(cmd, "STAT") == 0 || strcmp(cmd, "HELP") == 0)
    {
        reply(s, "211 DSi FTP: /sd %s, /nand and /nand2 %s",
              !perms.sd_read ? "hidden" : perms.sd_write ? "read/write" : "read only",
              !nand_ok || !perms.nand_read ? "hidden"
              : ftp_nand_full_active() ? "FULL WRITE (experimental)"
              : ftp_nand_writable() ? "WRITE ENABLED (system paths protected)" : "read only");
    }
    else
    {
        reply(s, "502 Command not supported");
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool ftp_init(uint32_t ip, int port, bool nand_available)
{
    my_ip = ip;
    ctrl_port = (port > 0 && port < 65536) ? port : 21;
    nand_ok = nand_available;
    pasv_counter = (unsigned)(time(NULL) & 0x3FF);

    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        sess[i].ctrl = -1;
        sess[i].pasv = -1;
    }
    ftp_ready = true;

    if (!xbuf)
        xbuf = malloc(XFER_BUF_SIZE);
    if (!xbuf)
        return false;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
        return false;

    int on = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(ctrl_port);
    sa.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(listen_fd, 4) != 0)
    {
        sock_close(&listen_fd);
        return false;
    }
    set_nonblock(listen_fd);
    return true;
}

static void poll_accept(void)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int fd = accept(listen_fd, (struct sockaddr *)&sa, &sl);
    if (fd < 0)
        return;

    set_nonblock(fd);

    // Locked address: refuse right away.
    int remaining = lock_remaining(sa.sin_addr.s_addr, time(NULL));
    if (remaining > 0)
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "421 Address blocked for %d s\r\n", remaining);
        send(fd, msg, strlen(msg), 0);
        closesocket(fd);
        return;
    }

    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        session_t *s = &sess[i];
        if (s->ctrl >= 0)
            continue;
        memset(s, 0, sizeof(*s));
        s->ctrl = fd;
        s->pasv = -1;
        s->peer = sa.sin_addr.s_addr;
        strcpy(s->cwd, "/");
        ui_msg(S_LOG_CLIENT_CONNECTED, inet_ntoa(sa.sin_addr));
        reply(s, login_is_pin ? "220 DSi FTP ready. User: any. Password: the PIN on the DSi"
                              : "220 DSi FTP ready. Log in with user and password");
        return;
    }
    const char *busy = "421 Too many connections\r\n";
    send(fd, busy, strlen(busy), 0);
    closesocket(fd);
}

static void poll_session(session_t *s)
{
    char buf[512];
    for (;;)
    {
        int n = recv(s->ctrl, buf, sizeof(buf), 0);
        if (n == 0 || (n < 0 && !would_block(n)))
        {
            ui_msg(S_LOG_CLIENT_GONE);
            session_close(s);
            return;
        }
        if (n < 0)
            return; // nothing more to read now

        for (int i = 0; i < n; i++)
        {
            char c = buf[i];
            if (c == '\n')
            {
                s->line[s->linelen] = '\0';
                if (s->linelen > 0 && s->line[s->linelen - 1] == '\r')
                    s->line[s->linelen - 1] = '\0';
                s->linelen = 0;
                if (s->line[0])
                    handle_command(s, s->line);
                if (s->ctrl < 0)
                    return;
            }
            else if (s->linelen < LINE_LEN - 1)
            {
                s->line[s->linelen++] = c;
            }
        }
    }
}

void ftp_poll(void)
{
    if (listen_fd < 0)
        return;
    poll_accept();
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        if (sess[i].ctrl >= 0)
            poll_session(&sess[i]);
    }
}

void ftp_shutdown(void)
{
    if (!ftp_ready)
        return; // never initialized: the sessions don't hold sockets yet
    ftp_ready = false;
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        if (sess[i].ctrl >= 0)
        {
            const char *bye = "421 Server stopped\r\n";
            send(sess[i].ctrl, bye, strlen(bye), 0);
        }
        session_close(&sess[i]);
    }
    sock_close(&listen_fd);
}
