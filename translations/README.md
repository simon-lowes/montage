# Montage in other languages

Each `montage_<language>.ts` file here is one translation of Montage's interface, built into Montage (Window ›
Language, applied at the next start). The files are Qt Linguist's format: open one in
[Qt Linguist](https://doc.qt.io/qt-6/linguist-translators.html), translate, and mark each text done (Ctrl+Enter) once
it reads well.

## Machine drafts

Texts no translator has looked at yet may hold a first draft from Opus-MT, made on a computer with
`montage-cli draft-interface translations/montage_<language>.ts`. A draft is shown in Montage, but stays
**unfinished** in Qt Linguist until a speaker of the language checks it and marks it done; Window › Language says how
much of each language is checked. Opus-MT learned from mixed sources, so its drafts often use the wrong word for an
editing term (a "clip", a "cut", a "track"): check those first. Rich text and plural forms are never drafted.
Montage only picks a language from the system's own when at least half of it is checked; drafts otherwise appear only
when chosen in Window › Language.

## Keep as they are

- Placeholders: `%1`, `%L1` (a number in the reader's own format), `%n` (a count: give each plural form).
- The `&` before a letter marks the key for Alt+letter; keep one in each menu entry, on a letter of the translation.
- Keyboard shortcuts after a tab, file format names (ProRes, EXR, IMF), timecodes, and the names of the agent link's
  tools and parameters (`montage_…`), which are not translated.

## Adding a language

```sh
lupdate -locations none -no-obsolete src -target-language <language> -ts translations/montage_<language>.ts
```

Then `cmake --build build --target montage_lupdate` brings every file here up to date as the source changes. Montage
needs Qt's lrelease (Qt Linguist tools) to build them in; without it, Montage is in English.

The texts under **Engine** are the names the engine gives the interface: effects and their settings, ready-made
titles, colour spaces, export presets. `montage-cli interface-texts -o src/app/EngineTexts.cpp` lists them for lupdate
when they change (a test says when the list is out of date).

## Checking with the test language

Window › Language › Test Language shows every translated text accented, about 40 % longer and between ⟦ and ⟧. Text
without the marks did not go through `tr()`, and text cut short has too little room for a longer language.
