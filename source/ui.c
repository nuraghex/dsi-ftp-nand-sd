// Graphics for both screens (see ui.h).

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "font.h"
#include "lang.h"
#include "ui.h"

#define W 256
#define H 192

// The keyboard loads its 4 bpp palette at the start of the sub BG palette,
// so the bottom bitmap uses entries from PAL_BASE on.
#define PAL_BASE 32

static const u16 pal[COL_COUNT] = {
    [COL_BG]     = RGB15(2, 3, 6),
    [COL_HEADER] = RGB15(4, 8, 18),
    [COL_CARD]   = RGB15(5, 6, 11),
    [COL_ACCENT] = RGB15(10, 20, 31),
    [COL_WHITE]  = RGB15(31, 31, 31),
    [COL_GRAY]   = RGB15(19, 19, 22),
    [COL_DIM]    = RGB15(9, 10, 13),
    [COL_GREEN]  = RGB15(6, 25, 9),
    [COL_RED]    = RGB15(29, 5, 5),
    [COL_YELLOW] = RGB15(30, 24, 4),
    [COL_SEL]    = RGB15(8, 12, 24),
    [COL_TABOFF] = RGB15(3, 4, 8),
    [COL_DANGER_BG]   = RGB15(9, 1, 1),
    [COL_DANGER_CARD] = RGB15(14, 2, 2),
};

static u16 *fb_top;     // 16 bpp
static u16 *fb_bot;     // 8 bpp, two pixels per halfword (VRAM needs 16-bit writes)
static Keyboard *kbd;

// ---------------------------------------------------------------------------
// UTF-8 and glyphs
// ---------------------------------------------------------------------------

u32 utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    u32 c = s[0];
    int extra;
    if (c < 0x80)
        extra = 0;
    else if ((c & 0xE0) == 0xC0)
    {
        extra = 1;
        c &= 0x1F;
    }
    else if ((c & 0xF0) == 0xE0)
    {
        extra = 2;
        c &= 0x0F;
    }
    else if ((c & 0xF8) == 0xF0)
    {
        extra = 3;
        c &= 0x07;
    }
    else
    {
        *ps += 1;
        return 0xFFFD;
    }
    for (int i = 1; i <= extra; i++)
    {
        if ((s[i] & 0xC0) != 0x80) // truncated or broken sequence
        {
            *ps += i;
            return 0xFFFD;
        }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *ps += 1 + extra;
    return c;
}

int utf8_len(const char *s)
{
    int n = 0;
    while (*s)
    {
        utf8_next(&s);
        n++;
    }
    return n;
}

static const u8 *glyph_find(const u16 *cps, const u8 (*px)[8], int n, u32 cp)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (cps[mid] == cp)
            return px[mid];
        if (cps[mid] < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

static const u8 *glyph(u32 cp)
{
    const u8 *g = NULL;
    if (cp <= 0xFFFF)
    {
        // Simplified Chinese shapes first when the interface is in Chinese
        if (lang_get() == LANG_ZH)
            g = glyph_find(font_zh_cp, font_zh_px, font_zh_count, cp);
        if (!g)
            g = glyph_find(font_cp, font_px, font_count, cp);
    }
    return g ? g : glyph_find(font_cp, font_px, font_count, '?');
}

static bool is_cjk(u32 c)
{
    return (c >= 0x3000 && c <= 0x9FFF) || (c >= 0xFF00 && c <= 0xFFEF);
}

// Characters that must not start a line (closing punctuation, long vowel).
static bool no_line_start(u32 c)
{
    static const u16 list[] = { 0x3001, 0x3002, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF01, 0xFF1F,
                                0xFF09, 0x300D, 0x300F, 0x3011, 0x30FC, 0x2026, 0x30FB };
    if (c == ',' || c == '.' || c == ':' || c == ';' || c == '!' || c == '?' || c == ')')
        return true;
    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++)
        if (c == list[i])
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// Setup and primitives
// ---------------------------------------------------------------------------

void ui_init(void)
{
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    int bg = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    fb_top = bgGetGfxPtr(bg);

    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    // Bitmap at +64 KB: the keyboard uses tile base 0 and map base 20 (40 KB).
    int bgb = bgInitSub(3, BgType_Bmp8, BgSize_B8_256x256, 4, 0);
    bgSetPriority(bgb, 3);
    fb_bot = bgGetGfxPtr(bgb);
    for (int i = 0; i < COL_COUNT; i++)
        BG_PALETTE_SUB[PAL_BASE + i] = pal[i];

    ui_fill(SCR_TOP, 0, 0, W, H, COL_BG);
    ui_fill(SCR_BOT, 0, 0, W, H, COL_BG);
}

static inline void plot_bot(int x, int y, u8 c)
{
    u16 *p = &fb_bot[(y * W + x) >> 1];
    *p = (x & 1) ? (u16)((*p & 0x00FF) | (c << 8)) : (u16)((*p & 0xFF00) | c);
}

void ui_fill(ui_screen_t s, int x, int y, int w, int h, int col)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0)
        return;

    if (s == SCR_TOP)
    {
        u16 c = pal[col] | BIT(15);
        for (int j = 0; j < h; j++)
        {
            u16 *row = fb_top + (y + j) * W + x;
            for (int i = 0; i < w; i++)
                row[i] = c;
        }
        return;
    }

    u8 c = (u8)(PAL_BASE + col);
    u16 pair = (u16)(c | (c << 8));
    for (int j = 0; j < h; j++)
    {
        int xs = x, xe = x + w;
        if (xs & 1)
            plot_bot(xs++, y + j, c);
        if ((xe & 1) && xe > xs)
            plot_bot(--xe, y + j, c);
        u16 *p = &fb_bot[((y + j) * W + xs) >> 1];
        for (int i = xs; i < xe; i += 2)
            *p++ = pair;
    }
}

void ui_text(ui_screen_t s, int x, int y, const char *str, int col, int scale)
{
    while (*str && x < W)
    {
        const u8 *g = glyph(utf8_next(&str));
        for (int r = 0; r < 8; r++)
        {
            u8 bits = g[r];
            for (int b = 0; b < 8; b++)
            {
                if (!(bits & (0x80 >> b)))
                    continue;
                if (scale == 1 && s == SCR_BOT)
                {
                    int px = x + b, py = y + r;
                    if (px >= 0 && px < W && py >= 0 && py < H)
                        plot_bot(px, py, (u8)(PAL_BASE + col));
                }
                else
                {
                    ui_fill(s, x + b * scale, y + r * scale, scale, scale, col);
                }
            }
        }
        x += 8 * scale;
    }
}

int ui_text_w(const char *str, int scale)
{
    return utf8_len(str) * 8 * scale;
}

void ui_clip(char *dst, size_t sz, const char *src, int max)
{
    bool cut = utf8_len(src) > max;
    int keep = cut ? max - 1 : max;
    size_t used = 0;
    const char *p = src;
    for (int i = 0; i < keep && *p; i++)
    {
        const char *start = p;
        utf8_next(&p);
        size_t n = (size_t)(p - start);
        if (used + n + 2 > sz) // room for "~" and the terminator
            break;
        memcpy(dst + used, start, n);
        used += n;
    }
    if (cut && used + 1 < sz)
        dst[used++] = '~';
    dst[used] = '\0';
}

int ui_wrap(const char *text, int cols, char (*out)[UI_LINE_BYTES], int max_lines)
{
    if (cols < 1)
        return 0; // no progress possible
    int n = 0;
    const char *p = text;
    bool soft = false; // this line continues a wrapped one
    while (*p && n < max_lines)
    {
        if (soft) // a wrapped line doesn't start with the spaces of the break
            while (*p == ' ')
                p++;
        if (!*p)
            break;
        const char *start = p, *q = p;
        const char *brk_end = NULL, *brk_next = NULL;
        const char *end = NULL, *next = NULL;
        int w = 0;
        while (*q && *q != '\n')
        {
            const char *before = q;
            u32 c = utf8_next(&q);
            if (w == cols)
            {
                soft = true;
                if (c == ' ')
                {
                    end = before; // the space falls exactly at the break
                    next = q;
                }
                else if (brk_end)
                {
                    end = brk_end;
                    next = brk_next;
                }
                else
                {
                    end = next = before; // one long word: cut it
                }
                break;
            }
            w++;
            if (c == ' ')
            {
                brk_end = before;
                brk_next = q;
            }
            else if (*q && *q != '\n')
            {
                const char *peek = q;
                u32 nx = utf8_next(&peek);
                if ((is_cjk(c) || is_cjk(nx)) && nx != ' ' && !no_line_start(nx))
                {
                    brk_end = q;
                    brk_next = q;
                }
            }
        }
        if (!end) // the rest of the paragraph fits
        {
            end = q;
            next = *q == '\n' ? q + 1 : q;
            soft = false;
        }
        while (end > start && end[-1] == ' ')
            end--;
        size_t len = (size_t)(end - start);
        if (len >= UI_LINE_BYTES)
            len = UI_LINE_BYTES - 1;
        memcpy(out[n], start, len);
        out[n][len] = '\0';
        n++;
        p = next;
    }
    return n;
}

void ui_bars(ui_screen_t s, int x, int y, int bars)
{
    for (int i = 0; i < 4; i++)
    {
        int h = 3 + i * 2;
        ui_fill(s, x + i * 5, y + 9 - h, 3, h, i < bars ? COL_GREEN : COL_DIM);
    }
}

// ---------------------------------------------------------------------------
// Top screen
// ---------------------------------------------------------------------------

static void draw_dot(int x, int y, int col)
{
    ui_fill(SCR_TOP, x + 1, y, 4, 6, col);
    ui_fill(SCR_TOP, x, y + 1, 6, 4, col);
}

static void draw_battery(int x, int y, int level, bool charging)
{
    char buf[UI_LINE_BYTES];
    ui_fill(SCR_TOP, x, y, 22, 1, COL_GRAY);
    ui_fill(SCR_TOP, x, y + 8, 22, 1, COL_GRAY);
    ui_fill(SCR_TOP, x, y, 1, 9, COL_GRAY);
    ui_fill(SCR_TOP, x + 21, y, 1, 9, COL_GRAY);
    ui_fill(SCR_TOP, x + 22, y + 3, 2, 3, COL_GRAY);
    if (level >= 0)
    {
        int col = level <= 3 ? COL_RED : level <= 7 ? COL_YELLOW : COL_GREEN;
        ui_fill(SCR_TOP, x + 2, y + 2, (level * 19) / 15, 5, col);
    }
    ui_clip(buf, sizeof(buf),
            level < 0 ? "?" : charging ? T(S_BATT_CHARGING) : level <= 3 ? T(S_BATT_LOW) : T(S_BATT_OK), 18);
    ui_text(SCR_TOP, x + 30, y + 1, buf, charging ? COL_GREEN : COL_WHITE, 1);
}

void ui_draw_status(const ui_status_t *st)
{
    bool full = st->nand_mode == NAND_FULL;
    bool writing = st->nand_mode != NAND_RO;
    // Full NAND write: the whole screen turns dark red.
    int bg = full ? COL_DANGER_BG : COL_BG;
    int card = full ? COL_DANGER_CARD : COL_CARD;
    ui_fill(SCR_TOP, 0, 0, W, H, bg);
    char buf[UI_LINE_BYTES];

    // Header
    const char *right = full ? T(S_HDR_BRICK) : writing ? T(S_HDR_NAND_WRITE) : "v" APP_VERSION;
    ui_fill(SCR_TOP, 0, 0, W, 26, writing ? COL_RED : COL_HEADER);
    ui_text(SCR_TOP, 8, 5, "DSi FTP", COL_WHITE, 2);
    // Author beside the name (lower line), version or warning top right
    ui_text(SCR_TOP, 124, 14, "@" APP_AUTHOR, writing ? COL_WHITE : COL_GRAY, 1);
    ui_clip(buf, sizeof(buf), right, 16);
    ui_text(SCR_TOP, W - 8 - ui_text_w(buf, 1), 4, buf, writing ? COL_WHITE : COL_GRAY, 1);

    // Status rows
    const int xl = 22, xv = 76;
    int y = 34;

    const char *sd_text = !st->sd_ok ? T(S_SD_NOT_FOUND) : !st->sd_read ? T(S_HIDDEN_PERMS)
                        : st->sd_write ? T(S_READ_WRITE) : T(S_READ_ONLY);
    draw_dot(10, y + 1, !st->sd_ok ? COL_RED : !st->sd_read ? COL_DIM : COL_GREEN);
    ui_text(SCR_TOP, xl, y, "SD", COL_GRAY, 1);
    ui_clip(buf, sizeof(buf), sd_text, 22);
    ui_text(SCR_TOP, xv, y, buf, COL_WHITE, 1);
    y += 14;

    const char *nand_text = !st->nand_ok ? T(S_NAND_NOT_AVAIL) : !st->nand_read ? T(S_HIDDEN_PERMS)
                          : full ? T(S_NAND_FULL_ON) : writing ? T(S_NAND_WRITE_ON) : T(S_READ_ONLY);
    draw_dot(10, y + 1, !st->nand_ok || !st->nand_read ? COL_DIM : writing ? COL_RED : COL_GREEN);
    ui_text(SCR_TOP, xl, y, "NAND", COL_GRAY, 1);
    ui_clip(buf, sizeof(buf), nand_text, 22);
    ui_text(SCR_TOP, xv, y, buf, writing ? (full ? COL_YELLOW : COL_RED) : COL_WHITE, 1);
    y += 14;

    draw_dot(10, y + 1, st->ssid ? COL_GREEN : COL_YELLOW);
    ui_text(SCR_TOP, xl, y, "Wi-Fi", COL_GRAY, 1);
    ui_clip(buf, sizeof(buf), st->ssid ? st->ssid : st->wifi_state, 17);
    ui_text(SCR_TOP, xv, y, buf, COL_WHITE, 1);
    if (st->ssid)
        ui_bars(SCR_TOP, W - 30, y - 1, st->bars);
    y += 14;

    ui_clip(buf, sizeof(buf), T(S_BATT_LABEL), 6);
    ui_text(SCR_TOP, xl, y, buf, COL_GRAY, 1);
    draw_battery(xv, y - 1, st->battery, st->charging);

    // Connection card
    ui_fill(SCR_TOP, 6, 94, W - 12, 76, card);
    ui_fill(SCR_TOP, 6, 94, W - 12, 2, writing ? COL_RED : COL_ACCENT);
    if (st->ip)
    {
        char addr[32];
        if (st->port && st->port != 21)
            snprintf(addr, sizeof(addr), "%s:%d", st->ip, st->port);
        else
            snprintf(addr, sizeof(addr), "%s", st->ip);
        ui_clip(buf, sizeof(buf), T(S_FTP_ADDRESS), 28);
        ui_text(SCR_TOP, 14, 102, buf, COL_GRAY, 1);
        // Large if it fits in the card (13 chars), small otherwise
        if (ui_text_w(addr, 2) <= W - 28)
            ui_text(SCR_TOP, 14, 114, addr, COL_WHITE, 2);
        else
            ui_text(SCR_TOP, 14, 118, addr, COL_WHITE, 1);
        if (st->custom_login)
        {
            char label[UI_LINE_BYTES];
            ui_clip(label, sizeof(label), T(S_USER_LABEL), 7);
            ui_text(SCR_TOP, 14, 140, label, COL_GRAY, 1);
            int ux = 14 + ui_text_w(label, 1) + 8;
            ui_clip(buf, sizeof(buf), (st->user && st->user[0]) ? st->user : T(S_ANY), (W - 14 - ux) / 8);
            ui_text(SCR_TOP, ux, 140, buf, COL_ACCENT, 1);
            ui_clip(buf, sizeof(buf), T(S_CUSTOM_PASSWORD), 29);
            ui_text(SCR_TOP, 14, 154, buf, COL_GRAY, 1);
        }
        else
        {
            ui_clip(buf, sizeof(buf), T(S_USER_FREE), 16); // ends before the PIN
            ui_text(SCR_TOP, 14, 140, buf, COL_GRAY, 1);
            ui_clip(buf, sizeof(buf), T(S_PASSWORD_PIN), 16);
            ui_text(SCR_TOP, 14, 154, buf, COL_GRAY, 1);
            ui_text(SCR_TOP, W - 14 - ui_text_w(st->pin, 3), 138, st->pin, COL_ACCENT, 3);
        }
    }
    else
    {
        ui_clip(buf, sizeof(buf), st->wifi_state, 28);
        ui_text(SCR_TOP, (W - ui_text_w(buf, 1)) / 2, 128, buf, COL_YELLOW, 1);
    }

    // Footer (red warning in full write mode)
    ui_fill(SCR_TOP, 0, H - 18, W, 18, full ? COL_RED : COL_HEADER);
    ui_clip(buf, sizeof(buf), full ? T(S_FOOT_FULL) : st->ip ? T(S_FOOT_MAIN) : T(S_FOOT_START), 31);
    ui_text(SCR_TOP, 6, H - 13, buf, full ? COL_WHITE : COL_GRAY, 1);
}

static void dialog_frame(const char *title, int title_col)
{
    char buf[UI_LINE_BYTES];
    ui_fill(SCR_TOP, 0, 0, W, H, COL_BG);
    ui_fill(SCR_TOP, 0, 0, W, 26, title_col);
    ui_clip(buf, sizeof(buf), title, 30);
    ui_text(SCR_TOP, 8, 9, buf, COL_WHITE, 1);
}

void ui_draw_dialog(const char *title, int title_col, const char *const *lines, int n)
{
    char buf[UI_LINE_BYTES];
    dialog_frame(title, title_col);
    if (n > 14)
        n = 14;
    int y0 = n > 12 ? 31 : 36, step = n > 12 ? 11 : 12;
    for (int i = 0; i < n; i++)
    {
        ui_clip(buf, sizeof(buf), lines[i], 31);
        ui_text(SCR_TOP, 6, y0 + i * step, buf, COL_WHITE, 1);
    }
}

void ui_draw_dialog_text(const char *title, int title_col, const char *text)
{
    static char lines[14][UI_LINE_BYTES];
    const char *ptrs[14];
    int n = ui_wrap(text, 31, lines, 14);
    for (int i = 0; i < n; i++)
        ptrs[i] = lines[i];
    ui_draw_dialog(title, title_col, ptrs, n);
}

// ---------------------------------------------------------------------------
// Bottom screen: tabs, console, settings
// ---------------------------------------------------------------------------

#define LOG_LINES     48
#define LOG_COLS      31
#define CONSOLE_Y     (UI_TAB_H + 4)
#define CONSOLE_LH    9
#define CONSOLE_ROWS  ((UI_FOOTER_Y - CONSOLE_Y) / CONSOLE_LH)

static char log_buf[LOG_LINES][UI_LINE_BYTES];
static u8 log_col[LOG_LINES];
static int log_head, log_count;
static bool panel_active;
static ui_tab_t panel_tab;

void ui_panel_set(bool active, ui_tab_t tab)
{
    panel_active = active;
    panel_tab = tab;
}

void ui_log_push(const char *text, int col)
{
    static char lines[4][UI_LINE_BYTES];
    int n = ui_wrap(text, LOG_COLS, lines, 4);
    for (int i = 0; i < n; i++)
    {
        memcpy(log_buf[log_head], lines[i], sizeof(log_buf[0]));
        log_col[log_head] = (u8)col;
        log_head = (log_head + 1) % LOG_LINES;
        if (log_count < LOG_LINES)
            log_count++;
    }
    if (panel_active && panel_tab == TAB_CONSOLE)
        ui_draw_console();
}

#define TAB_W (W / TAB_COUNT)   // 64 px: names up to 8 characters

void ui_draw_tabs(ui_tab_t active, ui_nand_mode_t mode, bool alert_access)
{
    static const str_t names[TAB_COUNT] = { S_TAB_CONSOLE, S_TAB_SERVER, S_TAB_NET, S_TAB_ACCESS };
    char buf[UI_LINE_BYTES];
    // NAND writable: red underline. Full write: the whole tab bar turns red.
    int on_col = mode == NAND_FULL ? COL_RED : COL_HEADER;
    int off_col = mode == NAND_FULL ? COL_DANGER_CARD : COL_TABOFF;
    int line_col = mode != NAND_RO ? COL_RED : COL_ACCENT;
    if (mode == NAND_FULL)
        line_col = COL_YELLOW;
    for (int t = 0; t < TAB_COUNT; t++)
    {
        int x = t * TAB_W;
        bool on = (t == (int)active);
        ui_fill(SCR_BOT, x, 0, TAB_W, UI_TAB_H, on ? on_col : off_col);
        if (on)
            ui_fill(SCR_BOT, x, UI_TAB_H - 3, TAB_W, 3, line_col);
        int col = on ? COL_WHITE : COL_GRAY;
        if (t == TAB_ACCESS && alert_access)
            col = COL_RED == on_col ? COL_YELLOW : COL_RED; // stays visible on a red bar
        ui_clip(buf, sizeof(buf), T(names[t]), 8);
        ui_text(SCR_BOT, x + (TAB_W - ui_text_w(buf, 1)) / 2, 7, buf, col, 1);
        if (t > 0)
            ui_fill(SCR_BOT, x, 0, 1, UI_TAB_H, COL_BG);
    }
}

void ui_draw_console(void)
{
    ui_fill(SCR_BOT, 0, UI_TAB_H, W, UI_FOOTER_Y - UI_TAB_H, COL_BG);
    int n = log_count < CONSOLE_ROWS ? log_count : CONSOLE_ROWS;
    for (int i = 0; i < n; i++)
    {
        int idx = (log_head - n + i + LOG_LINES) % LOG_LINES;
        ui_text(SCR_BOT, 2, CONSOLE_Y + i * CONSOLE_LH, log_buf[idx], log_col[idx], 1);
    }
}

void ui_draw_rows(const ui_setting_t *items, int n, int sel, int y0, int row_h)
{
    char buf[UI_LINE_BYTES];
    int ch = row_h - 3;                    // card height
    int label_y = (ch - 18) / 2;           // label + value = 18 px
    int btn_h = ch >= 24 ? 17 : 15;
    ui_fill(SCR_BOT, 0, y0 - 2, W, UI_FOOTER_Y - (y0 - 2), COL_BG);
    for (int i = 0; i < n; i++)
    {
        int y = y0 + i * row_h;
        ui_fill(SCR_BOT, 4, y, W - 8, ch, i == sel ? COL_SEL : COL_CARD);
        if (i == sel)
            ui_fill(SCR_BOT, 4, y, 3, ch, COL_ACCENT);
        // Text must end before the button
        char btn[UI_LINE_BYTES] = "";
        if (items[i].button)
            ui_clip(btn, sizeof(btn), items[i].button, 11);
        int bw = btn[0] ? ui_text_w(btn, 1) + 14 : 0;
        int bx = W - 10 - bw;
        int max_chars = ((btn[0] ? bx - 6 : W - 8) - 12) / 8;
        ui_clip(buf, sizeof(buf), items[i].label, max_chars);
        ui_text(SCR_BOT, 12, y + label_y, buf, COL_GRAY, 1);
        if (items[i].value)
        {
            ui_clip(buf, sizeof(buf), items[i].value, max_chars);
            ui_text(SCR_BOT, 12, y + label_y + 10, buf, items[i].value_col, 1);
        }
        if (btn[0])
        {
            int by = y + (ch - btn_h) / 2;
            ui_fill(SCR_BOT, bx, by, bw, btn_h, items[i].button_col);
            ui_text(SCR_BOT, bx + 7, by + (btn_h - 8) / 2, btn, COL_WHITE, 1);
        }
    }
}

int ui_rows_hit(int py, int n, int y0, int row_h)
{
    if (py < y0)
        return -1;
    int r = (py - y0) / row_h;
    return r < n ? r : -1;
}

void ui_draw_title_bar(const char *title, int col, const char *back)
{
    char buf[UI_LINE_BYTES];
    ui_fill(SCR_BOT, 0, 0, W, UI_TAB_H, col);
    ui_clip(buf, sizeof(buf), title, back ? (W - UI_BACK_W - 10) / 8 : 31);
    ui_text(SCR_BOT, 6, 7, buf, COL_WHITE, 1);
    if (back)
    {
        ui_fill(SCR_BOT, W - UI_BACK_W, 0, UI_BACK_W, UI_TAB_H, COL_SEL);
        ui_clip(buf, sizeof(buf), back, UI_BACK_W / 8);
        ui_text(SCR_BOT, W - UI_BACK_W + (UI_BACK_W - ui_text_w(buf, 1)) / 2, 7, buf, COL_WHITE, 1);
    }
}

bool ui_back_hit(int px, int py)
{
    return py >= 0 && py < UI_TAB_H && px >= W - UI_BACK_W;
}

void ui_draw_footer(const char *hint)
{
    char buf[UI_LINE_BYTES];
    ui_fill(SCR_BOT, 0, UI_FOOTER_Y, W, H - UI_FOOTER_Y, COL_HEADER);
    ui_clip(buf, sizeof(buf), hint, 31);
    ui_text(SCR_BOT, 4, UI_FOOTER_Y + 4, buf, COL_GRAY, 1);
}

int ui_tabs_hit(int px, int py)
{
    if (py < 0 || py >= UI_TAB_H)
        return -1;
    int t = px / TAB_W;
    return t < TAB_COUNT ? t : TAB_COUNT - 1;
}

// ---------------------------------------------------------------------------
// Keyboard (text BG0 of the sub engine, drawn above the bitmap)
// ---------------------------------------------------------------------------

void ui_keyboard_show(void)
{
    kbd = keyboardInit(NULL, 0, BgType_Text4bpp, BgSize_T_256x512, 20, 0, false, true);
    if (kbd)
        bgSetPriority(kbd->background, 0);
    keyboardShow();
}

void ui_keyboard_hide(void)
{
    keyboardHide();
}

bool ui_text_input(const char *title, const char *label, const char *subject, char *out,
                   size_t sz, size_t min_len, bool digits_only)
{
    size_t len = strlen(out);
    char hint[UI_LINE_BYTES], head[UI_LINE_BYTES];
    snprintf(hint, sizeof(hint), T(S_MIN_CHARS), (unsigned)min_len);
    static char help[4][UI_LINE_BYTES];
    int help_n = ui_wrap(T(S_KBD_HELP), 31, help, 4);

    ui_panel_set(false, TAB_CONSOLE);
    ui_fill(SCR_BOT, 0, 0, W, H, COL_BG);
    ui_fill(SCR_BOT, 0, 0, W, 20, COL_HEADER);
    ui_clip(head, sizeof(head), label, 31);
    ui_text(SCR_BOT, 6, 6, head, COL_WHITE, 1);
    ui_keyboard_show();

    bool redraw = true;
    while (1)
    {
        if (redraw)
        {
            char l1[UI_LINE_BYTES], l3[48];
            snprintf(l1, sizeof(l1), " ");
            ui_clip(l1 + 1, sizeof(l1) - 1, subject ? subject : "", 29);
            snprintf(l3, sizeof(l3), " %.28s_", len > 28 ? out + len - 28 : out); // ASCII only
            const char *lines[12] = { label, l1, "", l3, "", len < min_len ? hint : "", "", "" };
            int n = 8;
            for (int i = 0; i < help_n && n < 12; i++)
                lines[n++] = help[i];
            ui_draw_dialog(title, COL_HEADER, lines, n);
            redraw = false;
        }
        wait_frame();
        if (keysDown() & KEY_B)
        {
            ui_keyboard_hide();
            return false;
        }
        int c = keyboardUpdate();
        if (c == NOKEY || c == 0)
            continue;
        if (c == DVK_ENTER)
        {
            if (len >= min_len)
                break;
            continue;
        }
        if (c == DVK_BACKSPACE)
        {
            if (len > 0)
                out[--len] = '\0';
            redraw = true;
        }
        else if (c >= 32 && c < 127 && len < sz - 1 && (!digits_only || (c >= '0' && c <= '9')))
        {
            out[len++] = (char)c;
            out[len] = '\0';
            redraw = true;
        }
    }
    ui_keyboard_hide();
    return true;
}
