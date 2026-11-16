#!/usr/bin/env bash
# Extract the boot executable from the user's JP disc and regenerate recompiled C.
# usage: tools/prepare.sh ["path/to/Um Jammer Lammy (JP).bin"]
set -euo pipefail
cd "$(dirname "$0")/.."
export MSYS_NO_PATHCONV=1
# python3 on macOS/Linux; Git Bash on Windows usually only has `python`. Override with PYTHON=...
PY="${PYTHON:-$(command -v python3 2>/dev/null || command -v python)}"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) PY="${PYTHON:-python}" ;; esac
DISC="${1:-Um Jammer Lammy (JP).bin}"
mkdir -p build/exe build/analysis
"$PY" -I tools/psxdisc.py get "$DISC" /SCPS_180.11 build/exe/SCPS_180.11
"$PY" -I tools/recomp/analyze.py build/exe/SCPS_180.11 --seeds tools/recomp/seeds_jp.txt \
  --listing build/analysis/jp.lst --json build/analysis/jp.json
mkdir -p build/exe/ov
for f in $("$PY" -I tools/psxdisc.py ls "$DISC" | awk '{print $NF}' | grep '/COMOD'); do
  "$PY" -I tools/psxdisc.py get "$DISC" "$f" "build/exe/ov/$(basename "$f")"
done
"$PY" -I tools/recomp/gen.py build/exe/SCPS_180.11 build/gen_jp --seeds tools/recomp/seeds_jp.txt \
  --overlays build/exe/ov 801c5110 \
  --overrides tools/recomp/overrides_jp.txt --symbols tools/recomp/symbols_jp.txt
