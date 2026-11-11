// Minimal FTP server for the Nintendo DSi.
// Virtual root: /sd -> "sd:/", /nand -> "nand:/", /nand2 -> "nand2:/".
// What is visible and writable is decided by ftp_perms_t.
#ifndef DSIFTP_FTP_H
#define DSIFTP_FTP_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// Access permissions. /nand2 (camera photos) follows the NAND permissions.
//
// - nand_write: writes allowed outside the protected system paths; every NAND
//   file is backed up to the SD before being overwritten or deleted.
// - nand_full (EXPERIMENTAL, untested): no protected paths at all. Backups
//   are still made.
//
// The caller is responsible for nand_WriteProtect() (driver level).
typedef struct {
    bool sd_read;
    bool sd_write;
    bool nand_read;
    bool nand_write;
    bool nand_full;
} ftp_perms_t;

// Tells the server whether the NAND is mounted, before any permission is set.
// No network activity (can be called before the Wi-Fi is up).
void ftp_preinit(bool nand_available);

// ip: IPv4 address in network byte order (as returned by the network stack).
// port: control port (21 by default). Set the login with ftp_set_login().
bool ftp_init(uint32_t ip, int port, bool nand_available);

// Non-blocking: call once per frame.
void ftp_poll(void);

void ftp_shutdown(void);

// Sets the permissions. The flags are independent (each button changes only
// its own); the effective rules are: a drive without read is hidden, so its
// write has no effect; full write only acts on top of protected write.
void ftp_set_perms(const ftp_perms_t *p);
const ftp_perms_t *ftp_get_perms(void);

// Effective NAND state (flags combined with the rules above).
bool ftp_nand_writable(void);     // protected or full write is in effect
bool ftp_nand_full_active(void);  // full write is in effect

// Login credentials (sessions already logged in stay connected).
// user "" = any user name. is_pin only changes the messages sent to clients.
void ftp_set_login(const char *user, const char *pass, bool is_pin);

// Login lock per client address: after max_fails wrong passwords (over any
// number of connections) that address is refused for `seconds`.
typedef struct {
    bool enabled;
    int max_fails;
    int seconds;
} ftp_lock_cfg_t;
void ftp_set_lock(const ftp_lock_cfg_t *c);
const ftp_lock_cfg_t *ftp_get_lock(void);

typedef struct { uint32_t ip; int remaining; } ftp_blocked_t;   // ip: network order
int ftp_get_blocked(ftp_blocked_t *out, int max);               // currently locked addresses
void ftp_unblock_all(void);

typedef struct { uint32_t ip; bool authed; } ftp_client_t;
int ftp_get_clients(ftp_client_t *out, int max);                // open control connections
void ftp_disconnect_all(void);

typedef struct { uint32_t ip; bool ok; time_t when; bool valid; } ftp_attempt_t;
const ftp_attempt_t *ftp_get_last_attempt(void);                // last PASS received

// Provided by main.c: short status line on screen.
void ui_log(const char *fmt, ...);

// Provided by main.c: lets the system breathe during long transfers.
void net_yield(void);

#endif
