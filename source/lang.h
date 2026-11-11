// Interface languages. Texts: lang_strings.h (English) and lang_xx.c.
#ifndef DSIFTP_LANG_H
#define DSIFTP_LANG_H

#include <stdbool.h>

// Console colour of a message (see ui_msg()).
typedef enum { LC_N, LC_OK, LC_ER, LC_WA } lang_color_t;

#include "lang_strings.h"

typedef enum {
#define X(id, width, color, en) id,
    LANG_STRINGS(X)
#undef X
    S_COUNT
} str_t;

// Order of the language menu.
typedef enum {
    LANG_EN, LANG_IT, LANG_ES, LANG_FR, LANG_DE, LANG_RU, LANG_ZH, LANG_JA,
    LANG_COUNT
} lang_t;

// Translations: NULL entries fall back to English.
extern const char *const lang_it[S_COUNT];
extern const char *const lang_es[S_COUNT];
extern const char *const lang_fr[S_COUNT];
extern const char *const lang_de[S_COUNT];
extern const char *const lang_ru[S_COUNT];
extern const char *const lang_zh[S_COUNT];
extern const char *const lang_ja[S_COUNT];

void lang_set(lang_t l);
lang_t lang_get(void);
const char *T(str_t id);        // current language
const char *T_en(str_t id);     // English (log.txt)
lang_color_t lang_color(str_t id);

const char *lang_code(lang_t l);              // "en", "it", ... (settings.ini)
bool lang_from_code(const char *code, lang_t *out);
const char *lang_name(lang_t l);              // name in its own language
lang_t lang_system(void);                     // DSi system language

// Console message (implemented in main.c): the screen gets the current
// language, log.txt the English text, both with the same arguments. The
// arguments must not depend on the language (numbers, addresses, paths).
void ui_msg(str_t id, ...);

#endif
