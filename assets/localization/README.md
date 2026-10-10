# Translating the added PC menus

`native_menu_ru.json` contains the Russian text for the in-game video settings,
keyboard/mouse bindings, and exit confirmation, including live values and feedback.

Save the file as UTF-8. Edit each `text` value, retaining its uppercase English key
and `max_glyphs` limit. For example:

```json
"EXIT GAME": {"text": "ВЫЙТИ ИЗ ИГРЫ", "max_glyphs": 20}
```

The original menu grid has fixed widths. Use compact uppercase translations within
those limits. Physical key names such as W, Ctrl and LMB remain recognizable.
The generator checks Cyrillic encoding and layout limits, then converts the text
to CP1251 bytes for the Russian dump's original font. Paste Russian text into this
catalog rather than into generated XCR files or C++ byte strings.

From a source checkout with your own game files, run:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

This regenerates both the Russian loose menu (`CubeWnd.pc.ru.xcr`) and the compiled
live-value dictionary from the same catalog. Rebuild the executable after changing
the catalog so static labels and live values stay consistent. Generated menus are
staged beside the executable and included in Windows releases and Linux setup.

Russian menus activate when the runtime recognizes an approved Russian executable
and uses the game's English content slot. Choose **English**, or **System default**
on Russian Windows. The original dump supplies the other translated game strings,
fonts and audio; keep its executable and matching assets together. Language changes
take effect after restarting.
