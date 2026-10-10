"""Compile the editable UTF-8 PC menu catalog to the game's CP1251 glyph bytes."""
import argparse
import json
from pathlib import Path


DEFAULT_CATALOG = Path(__file__).resolve().parents[1] / "assets/localization/native_menu_ru.json"


def load_catalog(path=DEFAULT_CATALOG):
    catalog = json.loads(Path(path).read_text(encoding="utf-8"))
    if (catalog.get("format"), catalog.get("language"), catalog.get("encoding")) != (1, "ru", "cp1251"):
        raise ValueError("Unsupported native menu catalog format, language or encoding")
    entries = catalog.get("strings")
    if not isinstance(entries, dict) or not entries:
        raise ValueError("Native menu catalog has no strings")
    result = {}
    for english, entry in entries.items():
        if not english or not english.isascii() or english != english.upper():
            raise ValueError(f"Expected an uppercase English menu key: {english!r}")
        if not isinstance(entry, dict):
            raise ValueError(f"Invalid native menu entry: {english}")
        text, maximum = entry.get("text"), entry.get("max_glyphs")
        if not isinstance(text, str) or not text or text != text.upper():
            raise ValueError(f"Expected uppercase Russian menu text: {english}")
        if type(maximum) is not int or not 1 <= maximum <= 40:
            raise ValueError(f"Invalid native menu glyph limit: {english}")
        if any(ord(c) < 32 or ord(c) == 127 for c in english + text):
            raise ValueError(f"Native menu text contains a control character: {english}")
        try:
            encoded = text.encode("cp1251")
        except UnicodeEncodeError as error:
            raise ValueError(f"Native menu text is not representable in CP1251: {english}") from error
        if len(encoded) > maximum:
            raise ValueError(f"Native menu text exceeds {maximum} glyphs: {english}")
        # Registry preserves arbitrary byte strings via its Latin1 transport.
        # Both the XCR and C++ dictionary must contain these exact glyph bytes.
        result[english] = encoded.decode("latin1")
    return result


def compile_header(translations):
    lines = ["// Generated from assets/localization/native_menu_ru.json. Do not edit.",
             "#pragma once", "#include <string_view>", "",
             "namespace DarkRecomp::Native {",
             "struct NativeMenuTranslation { std::string_view english, russian; };",
             "inline constexpr NativeMenuTranslation kRussianNativeMenuText[]{"]
    for english, russian in sorted(translations.items()):
        encoded = "".join(f"\\x{byte:02x}" for byte in russian.encode("latin1"))
        lines.append(f"    {{{json.dumps(english)}, \"{encoded}\"}},")
    lines.extend(["};", "",
                  "constexpr std::string_view russianNativeMenuText(std::string_view english) noexcept {",
                  "    for (const auto& entry : kRussianNativeMenuText)",
                  "        if (entry.english == english) return entry.russian;",
                  "    return english;", "}", "}", ""])
    return "\n".join(lines)


def write_if_changed(path, contents):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != contents:
        path.write_bytes(contents)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, default=DEFAULT_CATALOG)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.catalog.resolve() == args.output.resolve():
        parser.error("Never overwrite the translation catalog")
    write_if_changed(args.output, compile_header(load_catalog(args.catalog)).encode("ascii"))
