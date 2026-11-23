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

for b in boards:
    env = b["env"]
    factory = root / ".pio" / "build" / env / "firmware.factory.bin"
    if not factory.exists():
        sys.exit(f"missing {factory} - run `pio run -e {env}` first")
    shutil.copy(factory, out / "firmware" / f"{env}.bin")
    manifest = {
        "name": f"CYD UniFi Monitor - {b['short']}",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32", "parts": [{"path": f"../firmware/{env}.bin", "offset": 0}]}],
    }
    (out / "manifests" / f"{env}.json").write_text(json.dumps(manifest, indent=2))

(out / "boards.json").write_text(json.dumps({"version": version, "boards": boards}, indent=2, ensure_ascii=False),
                                 encoding="utf-8")
print(f"site for v{version} with {len(boards)} boards -> {out}")
