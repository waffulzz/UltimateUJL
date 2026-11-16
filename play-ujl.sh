#!/usr/bin/env sh
# Launch Um Jammer Lammy PC from the project folder (macOS / Linux counterpart of "Play UJL.bat")
cd "$(dirname "$0")" || exit 1
exec ./build/cmake/ujl "Um Jammer Lammy (JP).bin" "$@"
