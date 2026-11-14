// Shared state and helpers of DSi FTP.
#ifndef DSIFTP_APP_H
#define DSIFTP_APP_H

#include <stdbool.h>
#include <nds.h>

#define APP_VERSION "1.0.0-beta.4"
#define APP_AUTHOR  "nuraghex"   // GitHub account, shown on the top screen

#define CFG_DIR   "sd:/_nds/dsiftp"
#define LOG_PATH  CFG_DIR "/log.txt"

extern bool g_sd_ok;        // SD card mounted
extern bool g_nand_ok;      // NAND mounted (read-only unless write mode is on)
extern u32 g_frames;        // frame counter

// Diagnostic log on the SD (never contains passwords).
void flog(const char *fmt, ...);

// Waits for the next frame while letting the lwIP cothread run.
// Never use swiWaitForVBlank(): it doesn't yield, and the IP stack stops.
void frame_yield(void);

// frame_yield() + scanKeys(); START quits the application.
void wait_frame(void);

// Disconnects cleanly and quits (write protection is restored first).
void app_exit(void);

#endif
