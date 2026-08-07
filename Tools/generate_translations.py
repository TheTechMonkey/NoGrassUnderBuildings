"""Generate initial machine-assisted PO files from the English localization source."""

from pathlib import Path
import re
import time

from deep_translator import GoogleTranslator


ROOT = Path(__file__).resolve().parents[1]
LOC = ROOT / "Content" / "Localization" / "NoGrassUnderBuildings"
SOURCE = (LOC / "en" / "NoGrassUnderBuildings.po").read_text(encoding="utf-8")

# Unreal culture -> Google Translate language. Regional English cultures intentionally
# use Unreal's normal fallback to the native English resource instead of duplicate files.
LANGUAGES = {
    "zh-Hant": "zh-TW", "it": "it", "uk": "uk", "bg": "bg", "cs": "cs",
    "nl": "nl", "ar": "ar", "eo": "eo", "fi": "fi", "hu": "hu",
    "lv": "lv", "no": "no", "ro": "ro", "sr-Cyrl": "sr", "th": "th",
    "tr": "tr", "vi": "vi", "fa": "fa",
}

ENTRY_RE = re.compile(r'(?ms)(msgctxt "[^"]+"\nmsgid ")((?:[^"\\]|\\.)*)("\nmsgstr ")((?:[^"\\]|\\.)*)(")')


def unescape_po(value: str) -> str:
    return value.replace(r'\"', '"').replace(r'\\', '\\').replace(r'\n', '\n')


def escape_po(value: str) -> str:
    return value.replace('\\', r'\\').replace('"', r'\"').replace('\n', r'\n')


source_strings = [unescape_po(match.group(2)) for match in ENTRY_RE.finditer(SOURCE)]
if len(source_strings) != 14:
    raise RuntimeError(f"Expected 14 localized strings, found {len(source_strings)}")

for culture, target in LANGUAGES.items():
    output_file = LOC / culture / "NoGrassUnderBuildings.po"
    if output_file.exists():
        print(f"Keeping existing {culture}")
        continue
    translator = GoogleTranslator(source="en", target=target)
    translated = translator.translate_batch(source_strings)
    iterator = iter(translated)

    def replace(match: re.Match[str]) -> str:
        return match.group(1) + match.group(2) + match.group(3) + escape_po(next(iterator)) + match.group(5)

    output = ENTRY_RE.sub(replace, SOURCE)
    output = output.replace('Language: en\\n', f'Language: {culture}\\n')
    output = output.replace('English translation.', f'{culture} machine-assisted translation.')
    directory = LOC / culture
    directory.mkdir(parents=True, exist_ok=True)
    output_file.write_text(output, encoding="utf-8", newline="\n")
    print(f"Generated {culture}")
    time.sleep(0.25)
