#!/usr/bin/env bash
# USA (SCUS-94448) recompile -- used only as an internal analysis/tracing tool, not shipped.
set -euo pipefail
cd "$(dirname "$0")/.."
export MSYS_NO_PATHCONV=1
DISC="${1:-Um Jammer Lammy (USA).bin}"
mkdir -p build/exe_us/ov build/analysis
python -I tools/psxdisc.py get "$DISC" /SCUS_944.48 build/exe_us/SCUS_944.48
for f in $(python -I tools/psxdisc.py ls "$DISC" | awk '{print $NF}' | grep '/COMOD'); do
  python -I tools/psxdisc.py get "$DISC" "$f" "build/exe_us/ov/$(basename "$f")"
done
python -I tools/recomp/analyze.py build/exe_us/SCUS_944.48 --seeds tools/recomp/seeds_us.txt \
  --listing build/analysis/us.lst --json build/analysis/us.json
python -I tools/recomp/gen.py build/exe_us/SCUS_944.48 build/gen_us --seeds tools/recomp/seeds_us.txt \
  --overlays build/exe_us/ov 801c5750 \
  --overrides tools/recomp/overrides_us.txt
