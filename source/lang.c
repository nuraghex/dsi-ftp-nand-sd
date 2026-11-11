// Interface languages (see lang.h).

#include <string.h>

#include <nds.h>

#include "lang.h"

static const char *const lang_en[S_COUNT] = {
#define X(id, width, color, en) [id] = en,
    LANG_STRINGS(X)
#undef X
};

static const unsigned char colors[S_COUNT] = {
#define X(id, width, color, en) [id] = color,
    LANG_STRINGS(X)
#undef X
};

static const char *const *const tables[LANG_COUNT] = {
    [LANG_EN] = lang_en, [LANG_IT] = lang_it, [LANG_ES] = lang_es, [LANG_FR] = lang_fr,
    [LANG_DE] = lang_de, [LANG_RU] = lang_ru, [LANG_ZH] = lang_zh, [LANG_JA] = lang_ja,
};

static const char *const codes[LANG_COUNT] = { "en", "it", "es", "fr", "de", "ru", "zh", "ja" };

static const char *const names[LANG_COUNT] = {
    "English", "Italiano", "Español", "Français", "Deutsch", "Русский", "中文", "日本語",
};

static lang_t current = LANG_EN;

void lang_set(lang_t l)
{
    if (l < LANG_COUNT)
        current = l;
}

lang_t lang_get(void)
{
    return current;
}

const char *T(str_t id)
{
    if (id >= S_COUNT)
        return "?";
    const char *s = tables[current][id];
    return s ? s : lang_en[id];
}

const char *T_en(str_t id)
{
    return id < S_COUNT ? lang_en[id] : "?";
}

lang_color_t lang_color(str_t id)
{
    return id < S_COUNT ? (lang_color_t)colors[id] : LC_N;
}

const char *lang_code(lang_t l)
{
    return l < LANG_COUNT ? codes[l] : "en";
}

bool lang_from_code(const char *code, lang_t *out)
{
    for (int i = 0; i < LANG_COUNT; i++)
    {
        if (strcmp(code, codes[i]) == 0)
        {
            *out = (lang_t)i;
            return true;
        }
    }
    return false;
}

const char *lang_name(lang_t l)
{
    return l < LANG_COUNT ? names[l] : "?";
}

// DSi system settings: 0 Japanese, 1 English, 2 French, 3 German,
// 4 Italian, 5 Spanish, 6 Chinese, 7 Korean (no Korean UI: English).
lang_t lang_system(void)
{
    static const lang_t map[8] = { LANG_JA, LANG_EN, LANG_FR, LANG_DE, LANG_IT, LANG_ES, LANG_ZH, LANG_EN };
    return map[PersonalData->language & 7];
}
