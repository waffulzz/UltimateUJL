#!/usr/bin/env bash
# USA (SCUS-94448) recompile -- used only as an internal analysis/tracing tool, not shipped.
set -euo pipefail
cd "$(dirname "$0")/.."
export MSYS_NO_PATHCONV=1
# python3 on macOS/Linux; Git Bash on Windows usually only has `python`. Override with PYTHON=...
PY="${PYTHON:-$(command -v python3 2>/dev/null || command -v python)}"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) PY="${PYTHON:-python}" ;; esac
DISC="${1:-Um Jammer Lammy (USA).bin}"
mkdir -p build/exe_us/ov build/analysis
"$PY" -I tools/psxdisc.py get "$DISC" /SCUS_944.48 build/exe_us/SCUS_944.48
for f in $("$PY" -I tools/psxdisc.py ls "$DISC" | awk '{print $NF}' | grep '/COMOD'); do
  "$PY" -I tools/psxdisc.py get "$DISC" "$f" "build/exe_us/ov/$(basename "$f")"
done
"$PY" -I tools/recomp/analyze.py build/exe_us/SCUS_944.48 --seeds tools/recomp/seeds_us.txt \
  --listing build/analysis/us.lst --json build/analysis/us.json
"$PY" -I tools/recomp/gen.py build/exe_us/SCUS_944.48 build/gen_us --seeds tools/recomp/seeds_us.txt \
  --overlays build/exe_us/ov 801c5750 \
  --overrides tools/recomp/overrides_us.txt
