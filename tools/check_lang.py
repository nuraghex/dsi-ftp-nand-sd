#!/usr/bin/env python3
"""Checks the translations in source/lang_xx.c against source/lang_strings.h.

Errors (exit code 1):
  * printf specifiers different from the English text (would crash),
  * characters without a glyph in source/font_data.c,
  * ids that don't exist.
Warnings:
  * missing translations (English is shown),
  * texts longer than their width (clipped with "~" on screen),
  * wrapped texts with more lines than allowed.

Usage: tools/check_lang.py [source_dir]
"""
import os
import re
import sys

STR_RE = r'(?:"(?:[^"\\]|\\.)*"\s*)+'
SPEC_RE = re.compile(r"%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?[diouxXscpf]")


def c_string(pieces):
    out = ""
    for piece in re.findall(r'"((?:[^"\\]|\\.)*)"', pieces):
        out += re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), piece)
    return out


def is_cjk(c):
    o = ord(c)
    return 0x3000 <= o <= 0x9FFF or 0xFF00 <= o <= 0xFFEF


NO_START = set(",.:;!?)") | set("、。，．：；！？）」』】ー…・")


def wrap(text, cols):
    """Same rules as ui_wrap() in ui.c."""
    lines = []
    for para in text.split("\n"):
        if para == "":
            lines.append("")
            continue
        i, soft = 0, False
        while i < len(para):
            if soft:
                while i < len(para) and para[i] == " ":
                    i += 1
                if i >= len(para):
                    break
            start, brk, j, w, end, nxt = i, None, i, 0, None, None
            while j < len(para):
                c = para[j]
                if w == cols:
                    soft = True
                    if c == " ":
                        end, nxt = j, j + 1
                    elif brk:
                        end, nxt = brk
                    else:
                        end = nxt = j
                    break
                w += 1
                if c == " ":
                    brk = (j, j + 1)
                elif j + 1 < len(para):
                    n = para[j + 1]
                    if (is_cjk(c) or is_cjk(n)) and n != " " and n not in NO_START:
                        brk = (j + 1, j + 1)
                j += 1
            if end is None:
                end, nxt, soft = j, j, False
            lines.append(para[start:end].rstrip(" "))
            i = nxt
    return lines


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "source")
    master = open(os.path.join(src, "lang_strings.h"), encoding="utf-8").read()
    en = {}
    for m in re.finditer(r"X\((S_\w+),\s*(-?\d+),\s*(LC_\w+),\s*(" + STR_RE + r")\)", master):
        en[m.group(1)] = (int(m.group(2)), c_string(m.group(4)))

    font = open(os.path.join(src, "font_data.c"), encoding="ascii").read()
    table = font.split("const u16 font_cp[] = {")[1].split("};")[0]
    glyphs = {int(x, 16) for x in re.findall(r"0x([0-9A-F]{4})", table)}

    errors = warnings = 0
    for lang in ("en", "it", "es", "fr", "de", "ru", "zh", "ja"):
        if lang == "en":
            tr = {sid: text for sid, (_, text) in en.items()}
        else:
            path = os.path.join(src, f"lang_{lang}.c")
            if not os.path.exists(path):
                print(f"[{lang}] file missing")
                errors += 1
                continue
            body = open(path, encoding="utf-8").read()
            tr = {m.group(1): c_string(m.group(2))
                  for m in re.finditer(r"\[(S_\w+)\]\s*=\s*(" + STR_RE + r")", body)}
        for sid in tr:
            if sid not in en:
                print(f"[{lang}] ERROR unknown id {sid}")
                errors += 1
        missing = [sid for sid in en if sid not in tr]
        if missing:
            print(f"[{lang}] {len(missing)} missing: {', '.join(missing[:8])}{' ...' if len(missing) > 8 else ''}")
            warnings += len(missing)
        for sid, text in tr.items():
            if sid not in en:
                continue
            width, en_text = en[sid]
            if SPEC_RE.findall(text) != SPEC_RE.findall(en_text):
                print(f"[{lang}] ERROR {sid}: specifiers {SPEC_RE.findall(text)} != {SPEC_RE.findall(en_text)}")
                errors += 1
            bad = sorted({c for c in text if c != "\n" and ord(c) not in glyphs})
            if bad:
                print(f"[{lang}] ERROR {sid}: no glyph for {' '.join(bad)}")
                errors += 1
            plain = SPEC_RE.sub("", text)
            if width > 0:
                if "\n" in text or len(plain) > width:
                    print(f"[{lang}] {sid}: {len(plain)} > {width} columns: {plain!r}")
                    warnings += 1
            else:
                n = len(wrap(plain, 31))
                if n > -width:
                    print(f"[{lang}] {sid}: {n} lines > {-width}")
                    warnings += 1
    print(f"{errors} errors, {warnings} warnings")
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
