"""Assemble the web-flasher site: docs/ + built firmware + ESP Web Tools manifests.

Usage (after `pio run` has built every environment):
    python tools/make_site.py [output_dir]      # default: _site

Used by .github/workflows/build.yml, and handy locally to test the flasher:
    python tools/make_site.py && python -m http.server -d _site 8000
"""
import json
import pathlib
import re
import shutil
import sys

root = pathlib.Path(__file__).resolve().parent.parent
out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else root / "_site")

version = re.search(r'FIRMWARE_VERSION\s+"([^"]+)"', (root / "include" / "config.h").read_text()).group(1)
boards = json.loads((root / "tools" / "boards.json").read_text(encoding="utf-8"))

if out.exists():
    shutil.rmtree(out)
shutil.copytree(root / "docs", out)
(out / "firmware").mkdir(exist_ok=True)
(out / "manifests").mkdir(exist_ok=True)

def find_boot_app0():
    """boot_app0.bin ships with the Arduino framework package."""
    pio_home = pathlib.Path.home() / ".platformio" / "packages"
    for p in sorted(pio_home.glob("framework-arduinoespressif32*/tools/partitions/boot_app0.bin")):
        return p
    sys.exit("can't find boot_app0.bin in ~/.platformio/packages")


for b in boards:
    env = b["env"]
    build = root / ".pio" / "build" / env
    factory = build / "firmware.factory.bin"
    if factory.exists():
        # One merged image flashed at 0x0
        shutil.copy(factory, out / "firmware" / f"{env}.bin")
        parts = [{"path": f"../firmware/{env}.bin", "offset": 0}]
    else:
        # Separate images at the standard ESP32 Arduino offsets
        if not (build / "firmware.bin").exists():
            sys.exit(f"missing {build / 'firmware.bin'} - run `pio run -e {env}` first")
        pieces = [("bootloader.bin", build / "bootloader.bin", 0x1000),
                  ("partitions.bin", build / "partitions.bin", 0x8000),
                  ("boot_app0.bin", find_boot_app0(), 0xE000),
                  ("app.bin", build / "firmware.bin", 0x10000)]
        parts = []
        for suffix, src, offset in pieces:
            shutil.copy(src, out / "firmware" / f"{env}-{suffix}")
            parts.append({"path": f"../firmware/{env}-{suffix}", "offset": offset})
    manifest = {
        "name": f"CYD UniFi Monitor - {b['short']}",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32", "parts": parts}],
    }
    (out / "manifests" / f"{env}.json").write_text(json.dumps(manifest, indent=2))

(out / "boards.json").write_text(json.dumps({"version": version, "boards": boards}, indent=2, ensure_ascii=False),
                                 encoding="utf-8")
print(f"site for v{version} with {len(boards)} boards -> {out}")
