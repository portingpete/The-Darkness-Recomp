# Xelu Light input prompts

Original, unmodified PNGs from [Xelu's Free Controller & Key Prompts](https://thoseawesomeguys.com/prompts/),
by Nicolae (Xelu) Berbece. Downloaded September 10, 2026. The bundled
[LICENSE.txt](LICENSE.txt) is the original pack readme: CC0, commercial use
permitted, attribution optional.

DarkRecomp embeds 32px versions of this artwork. Normal play and C++ builds use
the checked-in header, so players do not need these loose images or Pillow.
Controller input continues to select the game's original controller artwork.

To regenerate the artwork after changing a source PNG, use Python with Pillow
from the repository root:

```powershell
python tools/compile_prompt_icons.py
python tools/compile_prompt_icons.py --check
python tools/compile_prompt_icons.py --preview build_native/input-prompts.png
```

The script trims transparent export padding, preserves aspect ratio and alpha,
and combines authored keycaps where a shared game texture represents several
bindings. It writes `renderer/engine/xelu_light.generated.h`; `--check` verifies
reproducibility and `--preview` creates a contact sheet.

See [CONTROLS.md](../../../CONTROLS.md) for the active-input switching behavior.
