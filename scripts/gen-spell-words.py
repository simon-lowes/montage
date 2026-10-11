#!/usr/bin/env python3
"""Builds Montage's spelling word lists from SCOWL (http://wordlist.aspell.net/).

Usage: gen-spell-words.py <scowl>/final <out dir>

Writes english.txt (words every English variant shares), american.txt and british.txt (each variant's own; British
with both -ise and -ize spellings), from
SCOWL's lists up to size 60 (the size of the usual en_US and en_GB spelling dictionaries): words, capitalised words,
contractions, abbreviations and proper names. Possessives are left out when their stem is listed (the checker strips
"'s"). Each word appears under the smallest size it is in ("=10", "=20", ...), which ranks suggestions: size 10 are the
most common words.
"""
import os
import sys

SIZES = [10, 20, 35, 40, 50, 55, 60]
TYPES = ["words", "upper", "contractions", "abbreviations", "proper-names"]
NOTICE = """# Word list from SCOWL (Spell Checker Oriented Word Lists), size 60, by scripts/gen-spell-words.py.
#
# Copyright 2000-2018 by Kevin Atkinson
#
# Permission to use, copy, modify, distribute and sell these word lists, the associated scripts, the output created
# from the scripts, and its documentation for any purpose is hereby granted without fee, provided that the above
# copyright notice appears in all copies and that both that copyright notice and this permission notice appear in
# supporting documentation. Kevin Atkinson makes no representations about the suitability of this array for any
# purpose. It is provided "as is" without express or implied warranty.
#
# SCOWL's other sources (Moby Words II, Brian Kelk's UK English Wordlist, 12Dicts, ENABLE and others) are in the
# public domain or under terms that allow this use; see SCOWL's Copyright file.
"""


def load(final, kind):
    level = {}
    for size in SIZES:
        for t in TYPES:
            path = os.path.join(final, f"{kind}-{t}.{size}")
            if not os.path.exists(path):
                continue
            with open(path, encoding="latin-1") as f:
                for line in f:
                    w = line.strip()
                    if w and w not in level:
                        level[w] = size
    return level


def write(path, words):
    every = set(words)
    by = {}
    for w, size in words.items():
        if w.endswith("'s") and w[:-2] in every:
            continue
        by.setdefault(size, []).append(w)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(NOTICE)
        for size in SIZES:
            if size in by:
                f.write(f"={size}\n")
                f.write("\n".join(sorted(by[size])) + "\n")


def main():
    final, out = sys.argv[1], sys.argv[2]
    english = load(final, "english")
    american = {w: s for w, s in load(final, "american").items() if w not in english}
    # British English takes both -ise and Oxford -ize spellings, as the usual en_GB dictionaries do.
    british = load(final, "british")
    for w, s in load(final, "british_z").items():
        british.setdefault(w, s)
    british = {w: s for w, s in british.items() if w not in english}
    write(os.path.join(out, "english.txt"), english)
    write(os.path.join(out, "american.txt"), american)
    write(os.path.join(out, "british.txt"), british)


if __name__ == "__main__":
    main()
