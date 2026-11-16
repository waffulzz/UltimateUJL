# source tools/env.sh  -- build environment for this shell.
# Windows (Git Bash): puts clang (llvm-mingw), cmake and ninja on PATH and stops MSYS path mangling.
# macOS / Linux: nothing to put on PATH (install clang, cmake, ninja and python3 with your package manager);
# it only sets CC/CXX to clang when they aren't set yet.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    _WG="$HOME/AppData/Local/Microsoft/WinGet/Packages"
    _LLVM=$(ls -d $_WG/MartinStorsjo.LLVM-MinGW*/llvm-mingw*/bin 2>/dev/null | head -1)
    _NINJA=$(ls -d $_WG/Ninja-build.Ninja* 2>/dev/null | head -1)
    export PATH="$_LLVM:/c/Program Files/CMake/bin:$_NINJA:$PATH"
    export MSYS_NO_PATHCONV=1
    ;;
  *)
    : "${CC:=clang}" "${CXX:=clang++}"
    export CC CXX
    ;;
esac
