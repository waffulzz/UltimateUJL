# source tools/env.sh  -- puts clang (llvm-mingw), cmake, ninja on PATH for Git Bash
_WG="$HOME/AppData/Local/Microsoft/WinGet/Packages"
_LLVM=$(ls -d $_WG/MartinStorsjo.LLVM-MinGW*/llvm-mingw*/bin 2>/dev/null | head -1)
_NINJA=$(ls -d $_WG/Ninja-build.Ninja* 2>/dev/null | head -1)
export PATH="$_LLVM:/c/Program Files/CMake/bin:$_NINJA:$PATH"
export MSYS_NO_PATHCONV=1
