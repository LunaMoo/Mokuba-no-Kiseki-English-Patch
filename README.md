# Mobile Suit Gundam: Path of the Trojan Horse — English Translation v0.1

Fan translation of **Kidou Senshi Gundam: Mokuba no Kiseki** (機動戦士ガンダム 木馬の軌跡,
PSP, NPJH-50442) into English.

Everything is translated: menus, briefings, mission objectives, unit/weapon/pilot names,
help screens, tutorials, the reference manual, in-game graphics (HUD, stage titles, result
screens, maps), the title screen logo and the save icons.

## Two editions

| Edition | Patch file | Output | Runs on |
|---|---|---|---|
| **Standard** | `mokuba_english.patch` | `<name>_english.iso` | PSP (custom firmware) and PPSSPP |
| **PPSSPP texture-pack edition** | `mokuba_english_ppsspp.patch` | `<name>_english_ppsspp.iso` | PPSSPP |

Pick **Standard** if you're not sure.

The **PPSSPP edition** is meant for HD texture packs and has a few extras:

- All text is drawn from one fixed font texture, so a single PNG gives an HD font everywhere.
- Every texture is stored at its full power-of-two width and height (the game is patched to
  use that layout), so a texture dump holds exactly one copy of the image plus transparent
  padding, and PPSSPP texture hashes are stable.
- Letters are placed with sub-pixel rounding, so narrow text no longer gets random gaps.
- Text lines can be longer, so the stage briefings use the full, uncondensed translation.
- Names such as "White Base" are shown in full (the Standard edition uses "W.Base").
- English subtitles for the movies and for the voiced lines in missions (talking heads)
  TODO: test subtitles synchronization in all movies to catch some uncertain ones.
- Labels that would run off the right edge of the screen are moved back onto it.
- It needs PPSSPP's PSP model set to PSP-2000/3000 (the default), because it uses the
  extra memory those models have.
- The ISO is about 235 MB bigger.

## How to patch

You need your own copy of the game as an ISO:
`Kidou Senshi Gundam - Mokuba no Kiseki (Japan).iso`.
CSO/CHD files have to be converted back to ISO first.

**Standard edition:** drag the ISO onto **`mokuba_patcher.exe`**.

**PPSSPP edition:** this edition also patches the game's executable, but the patch only
contains the changes, not the executable itself. You supply your own decrypted copy first:

1. In PPSSPP: **Settings → Tools → Developer tools → Dump decrypted EBOOT.BIN on game boot**.
2. Start the original Japanese game once (the title screen is enough).
3. Copy the dumped file from PPSSPP's `memstick/PSP/SYSTEM/DUMP/` folder
   (e.g. `NPJH50442_EBOOT.BIN`) into the folder that contains your ISO.
   Any file name works; the patcher recognises it by its checksum.
4. Drag the ISO onto **`make_ppsspp_edition.bat`**.

A new ISO is created next to the original. Your original ISO is never changed.
The patcher checks the ISO first and refuses one that doesn't match the expected game.

From a command line:
```
mokuba_patcher <game.iso>                                  -> Standard edition
mokuba_patcher <game.iso> mokuba_english_ppsspp.patch [decrypted EBOOT]   -> PPSSPP edition
```

**Linux (x86-64):** use `mokuba_patcher_linux` the same way from a terminal (`chmod +x` it first if needed), or
`./make_ppsspp_edition.sh <game.iso>` for the PPSSPP edition.

**Any OS with Python 3.8+ (macOS, Linux on ARM, ...):** `mokuba_patcher.py` does the same
thing with no extra modules:
```
python3 mokuba_patcher.py <game.iso>                                  -> Standard edition
python3 mokuba_patcher.py <game.iso> mokuba_english_ppsspp.patch      -> PPSSPP edition
```
You can also build the C patcher yourself: `gcc -O2 -o mokuba_patcher source/mokuba_patcher.c`.

## HD textures (PPSSPP edition)

### TODO - only font included right now since I was unhappy with quality, plan to do a cleaner images that will also replace the current quick work back at x1 res without texture replacement.

1. Copy the folder `hd_textures/NPJH50442` to `PSP/TEXTURES/` (so you get
   `PSP/TEXTURES/NPJH50442/textures.ini`).
2. Enable **Settings → Tools → Developer tools → Replace textures**.

The pack needs a recent PPSSPP (a build that includes PR #23101). Its `textures.ini` uses
`[hashranges]` with address 0 so that 512-pixel-tall textures are always hashed whole and
have just one name; older versions miss some of the replaced textures.

The pack has a 4× font, 4× redraws of every texture the translation changed (tutorials,
briefings, mission cards, menus, labels) and upscales of the remaining UI textures with text.
`hd_font/make_hd_font.py` rebuilds the font from a texture dump with another typeface or scale.

## Notes

- The PSP's own system dialogs (save/load confirmations) follow the system language.
  In PPSSPP set **Settings → System → Language** to English.
- Save files are compatible with the Japanese version.

## Files

```
mokuba_patcher.exe            the patcher (Windows)
mokuba_patcher_linux          the patcher (Linux x86-64, static)
mokuba_patcher.py             the patcher as a Python 3 script (any OS)
mokuba_english.patch          Standard edition
mokuba_english_ppsspp.patch   PPSSPP texture-pack edition
make_ppsspp_edition.bat       drag-and-drop helper for the PPSSPP edition (Windows)
make_ppsspp_edition.sh        the same for Linux
hd_textures/NPJH50442/        HD texture pack for the PPSSPP edition
hd_font/                      HD font for the PPSSPP edition + its generator
source/                       patcher source code (C, no dependencies) + Visual Studio project
LICENSE                       MIT licence for the patcher source and tools
```

## Building the patcher

- Visual Studio: open `source/mokuba_patcher.sln` and build Release.
- Or run `source/build.bat` from a Developer Command Prompt (MSVC) or with MinGW in PATH.

## License

The patcher source code and tools are released under the MIT License (see `LICENSE`).
The patch files contain only translated text, edited graphics and the changes to the game's
executable; the game itself is not included.

## Disclaimer

This is an unofficial fan translation, not affiliated with or endorsed by Bandai Namco,
Sotsu or Sunrise. No game data is needed beyond your own legally obtained copy of the game.
Mobile Suit Gundam © SOTSU・SUNRISE.
