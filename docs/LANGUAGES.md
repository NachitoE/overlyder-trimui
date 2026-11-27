# Translations (language files)

All user-facing strings live in **flat JSON files**, one per language, inside
`app/lang/` in the source tree and `lang/` in the installed app:

```
app/lang/es.json    Spanish
app/lang/en.json    English
app/lang/pt.json    Portuguese
```

The C source no longer contains translated text. `src/overlay_ui.c` only holds
the **schema** — the list of key names (`g_tr_keys[]`, one entry per `S_*`
enum value) — and a runtime table (`g_tr_values[S_COUNT][256]`) filled from the
JSON file at startup. `TR(S_KEY)` therefore reads the loaded string, not a
compiled-in literal.

## JSON format

Each file is a single flat object: `"S_KEY": "translated text"`. Keys match the
`S_*` enum identifiers exactly, so adding/reordering a string means editing the
enum and the key array in `src/overlay_ui.c` and adding the same key to every
JSON file.

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

1. Add an entry to the `S_*` language enum only if a new string is needed.
2. Add the key to `g_tr_keys[]` in `src/overlay_ui.c`.
3. Add a `LANG_*` enum value, bump `NLANG`, append the code to
   `g_lang_codes[]` and `LANG_NAME()` in `src/overlay_ui.c`, and accept the new
   `lang=` value in `cfg_load()` in `src/util.c`.
4. Create `app/lang/<code>.json` with **all** keys from `g_tr_keys[]`.
5. Add the file to `install.sh` and to the release package in
   `scripts/build-win.ps1`.

## Packaging / install

- `scripts/build-win.ps1` copies `app/lang/*.json` into `dist/OverLyder/lang/`.
- `install.sh` copies them into `/mnt/SDCARD/Apps/OverLyder/lang/`.
- `uninstall.sh` removes the whole app directory, including `lang/`.

No new binary is needed for translations; the loader is part of `overlay_ui`.
