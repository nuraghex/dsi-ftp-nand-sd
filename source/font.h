// 8x8 glyph tables for every script of the interface (see tools/mkfont.py).
#ifndef DSIFTP_FONT_H
#define DSIFTP_FONT_H

#include <nds.h>

// Sorted code points and their glyphs: 8 rows, most significant bit left.
extern const u16 font_cp[];
extern const u8 font_px[][8];
extern const int font_count;

// Simplified Chinese shapes of ideographs that Japanese draws differently.
extern const u16 font_zh_cp[];
extern const u8 font_zh_px[][8];
extern const int font_zh_count;

#endif
