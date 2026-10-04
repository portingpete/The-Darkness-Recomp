# Xelu Light input prompts

Original, unmodified PNGs from [Xelu's Free Controller & Key Prompts](https://thoseawesomeguys.com/prompts/), by Nicolae (Xelu) Berbece. Downloaded September 10, 2026. The bundled `LICENSE.txt` is the original pack readme: CC0, commercial use permitted, attribution optional.

DarkRecomp embeds 64px versions of this artwork, baked directly from the original 100px PNGs. `tools/compile_prompt_icons.py` trims transparent export padding, resamples with preserved aspect ratio and alpha, and combines authored keycaps where a shared game texture represents several bindings. Standalone reload, aim, and number keys are also included for gameplay prompts. Run it with Python and Pillow to regenerate `renderer/engine/xelu_light.generated.h`; `--check` verifies reproducibility and `--preview PATH.png` creates a contact sheet. Normal builds do not require Pillow or load these PNGs at runtime.

Controller input continues to select the game's original controller artwork.
