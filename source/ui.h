// Graphics for both screens.
//
// Top screen:    16-bit bitmap (main engine BG3, VRAM A).
// Bottom screen: 8-bit bitmap (sub engine BG3, VRAM C + 64 KB) with a small
//                palette, plus the on-screen keyboard on BG0 when needed.
// Text is UTF-8, drawn with 8x8 glyphs (font_data.c): Latin, Greek,
// Cyrillic, kana and CJK, every character one 8-pixel column.
#ifndef DSIFTP_UI_H
#define DSIFTP_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <nds.h>

// One screen line: 32 columns of up to 3 UTF-8 bytes, plus "~" and NUL.
#define UI_LINE_BYTES 104

// Next code point of a UTF-8 string (U+FFFD for broken sequences).
u32 utf8_next(const char **s);
int utf8_len(const char *s);   // code points = screen columns

// Logical colours, valid on both screens.
enum {
    COL_BG, COL_HEADER, COL_CARD, COL_ACCENT, COL_WHITE, COL_GRAY, COL_DIM,
    COL_GREEN, COL_RED, COL_YELLOW, COL_SEL, COL_TABOFF,
    COL_DANGER_BG, COL_DANGER_CARD,   // full NAND write mode
    COL_COUNT
};

// NAND access shown on screen
typedef enum { NAND_RO, NAND_WRITE, NAND_FULL } ui_nand_mode_t;

typedef enum { SCR_TOP, SCR_BOT } ui_screen_t;
typedef enum { TAB_CONSOLE, TAB_SERVER, TAB_NET, TAB_ACCESS, TAB_COUNT } ui_tab_t;

typedef struct {
    bool sd_ok;
    bool nand_ok;
    bool sd_read, sd_write;  // permissions
    bool nand_read;
    ui_nand_mode_t nand_mode;
    const char *wifi_state;  // e.g. "connected", "searching..."
    const char *ssid;        // NULL if not connected
    int bars;                // signal 0..4
    const char *ip;          // NULL if the server isn't running
    const char *pin;         // shown when custom_login is false
    bool custom_login;       // custom user/password instead of the PIN
    const char *user;        // custom login user ("" = any)
    int port;                // FTP control port
    int battery;             // 0..15, -1 unknown
    bool charging;
} ui_status_t;

// One row of the settings tab.
typedef struct {
    const char *label;
    const char *value;
    int value_col;
    const char *button;      // NULL: no button
    int button_col;
} ui_setting_t;

void ui_init(void);

// Primitives
void ui_fill(ui_screen_t s, int x, int y, int w, int h, int col);
void ui_text(ui_screen_t s, int x, int y, const char *str, int col, int scale);
int ui_text_w(const char *str, int scale);
// Copies at most max columns; a longer text ends with "~".
void ui_clip(char *dst, size_t sz, const char *src, int max);
// Word wrap ("\n" = new line; Chinese/Japanese break between characters).
// Returns the number of lines written.
int ui_wrap(const char *text, int cols, char (*out)[UI_LINE_BYTES], int max_lines);
void ui_bars(ui_screen_t s, int x, int y, int bars);

// Top screen
void ui_draw_status(const ui_status_t *st);
void ui_draw_dialog(const char *title, int title_col, const char *const *lines, int n);
void ui_draw_dialog_text(const char *title, int title_col, const char *text); // wrapped

// Bottom screen: tabbed panel (Console / Impostazioni)
#define UI_TAB_H     22
#define UI_FOOTER_Y  176
void ui_panel_set(bool active, ui_tab_t tab); // panel shown? (console auto-redraw)
// alert_access: draws the "Accessi" tab label in red (an address is blocked).
void ui_draw_tabs(ui_tab_t active, ui_nand_mode_t mode, bool alert_access);
void ui_draw_console(void);
void ui_draw_footer(const char *hint);
int ui_tabs_hit(int px, int py);                // tab index or -1

// Rows with label, value and an optional button (settings tab, Permessi menu).
void ui_draw_rows(const ui_setting_t *items, int n, int sel, int y0, int row_h);
int ui_rows_hit(int py, int n, int y0, int row_h);  // row index or -1

// Full-width title bar on the bottom screen with a "back" button on the right.
#define UI_BACK_W 72
void ui_draw_title_bar(const char *title, int col, const char *back);
bool ui_back_hit(int px, int py);

// Console tab: the text is wrapped over several lines, all in colour col.
void ui_log_push(const char *text, int col);

// On-screen keyboard on the bottom screen (BG0, above the bitmap).
void ui_keyboard_show(void);
void ui_keyboard_hide(void);

// Text entry with the on-screen keyboard. out holds the initial text and the
// result. Enter confirms (only with at least min_len characters), B cancels.
// digits_only: accept only 0-9 (e.g. a port number).
bool ui_text_input(const char *title, const char *label, const char *subject, char *out,
                   size_t sz, size_t min_len, bool digits_only);

#endif
