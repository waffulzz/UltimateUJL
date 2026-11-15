@echo off
rem Launch Um Jammer Lammy PC from the project folder
cd /d "%~dp0"
start "" "build\cmake\ujl.exe" "Um Jammer Lammy (JP).bin"
