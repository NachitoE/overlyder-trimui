# Translations (language files)

All user-facing strings live in **flat JSON files**, one per language, inside
`app/lang/` in the source tree and `lang/` in the installed app:

```
app/lang/es.json    Spanish
app/lang/en.json    English
app/lang/pt.json    Portuguese
```

There is a **single source of truth for the keys**:
`app/lang/keys.json`, a JSON array of every `S_*` key name. `src/lang_keys.h`
is generated from it by `scripts/gen_lang.py` and contains the `S_*` enum plus
the `g_tr_keys[]` array. No key is written twice by hand. `src/overlay_ui.c`
includes that generated header and only keeps a runtime table
(`g_tr_values[S_COUNT][256]`), filled from the language JSON at startup.
`TR(S_KEY)` therefore reads the loaded string, not a compiled-in literal.

Whenever `keys.json` changes, run:

```sh
python scripts/gen_lang.py
```

This regenerates `src/lang_keys.h` and adds/removes keys in all three language
files (existing translations are preserved; new keys start as empty strings —
fill them in afterwards).

## JSON format

Each file is a single flat object: `"S_KEY": "translated text"`. Keys match the
entries in `app/lang/keys.json` exactly. To add a string, add its key there,
run `scripts/gen_lang.py`, then fill the new value in the three files.

Example (`app/lang/en.json`):

```json
{
  "S_AJUSTES": "SETTINGS",
  "S_VOL": "Volume",
  "S_KEYBOARD": "Keyboard",
  "S_CFG_SIGSTOP": "Layer, no SIGSTOP"
}
```

UTF-8 is used directly (Spanish/Portuguese accents are plain characters, not
escapes). The minimal reader in `src/util.c` (`json_load_strings()`) also
accepts `\"`, `\\` and `\n` escapes.

## Loading rules

At startup `main()` sets `g_lang` from `lang=` in `overlay.cfg`, then calls
`lang_load(g_lang)` **before** `items_set_language()`. The loader looks for the
file in this order:

1. `$OVERLAY_LANG_DIR/<code>.json` if that environment variable is set,
2. `<directory of overlay_ui>/../lang/<code>.json`,
3. `/mnt/SDCARD/Apps/OverLyder/lang/<code>.json`.

`<code>` is `es`, `en` or `pt`. The result is logged to `/tmp/overlayd.log` as
`language en: 83/83 strings from …`.

If the file is missing or a key is absent, Fallback is the **key name** itself
(e.g. `S_VOL`) instead of an empty label, so the UI never shows blank rows. The
app still works without the lang folder, but with untranslated key names.

Changing **Idioma** in the app's settings screen calls `lang_load()` again
immediately, so the whole UI switches language without restarting. The option
is written as `lang=es|en|pt` in `overlay.cfg` when settings are saved.

## Adding a new language

1. Add the new key to `app/lang/keys.json` (the central list).
2. Run `python scripts/gen_lang.py` — this regenerates `src/lang_keys.h` and
   adds the key to `es/en/pt.json`.
3. Fill the translated value in each `app/lang/<code>.json`.
4. For a whole new language: add a `LANG_*` enum value, bump `NLANG`, append
   its code to `g_lang_codes[]` and `LANG_NAME()` in `src/overlay_ui.c`, accept
   the new `lang=` value in `cfg_load()` in `src/util.c`, add the code to
   `LANGS` in `scripts/gen_lang.py`, and add the file to `install.sh` and to
   the release package in `scripts/build-win.ps1`.

## Packaging / install

- `scripts/build-win.ps1` copies `app/lang/*.json` into `dist/OverLyder/lang/`.
- `install.sh` copies them into `/mnt/SDCARD/Apps/OverLyder/lang/`.
- `uninstall.sh` removes the whole app directory, including `lang/`.

No new binary is needed for translations; the loader is part of `overlay_ui`.
