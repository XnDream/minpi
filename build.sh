#!/bin/bash
# build.sh - 生成/更新 build/build.bat 并编译
set -e
cd "$(dirname "$0")"

VCVARS=$(find "C:/Program Files (x86)/Microsoft Visual Studio/2022" \
               "C:/Program Files/Microsoft Visual Studio/2022" \
               -name "vcvarsall.bat" 2>/dev/null | head -1)
if [ -z "$VCVARS" ]; then
    echo "Error: vcvarsall.bat not found. Install Visual Studio Build Tools 2022."
    exit 1
fi
VCVARS_WIN=$(cygpath -w "$VCVARS")
echo "Using MSVC: $VCVARS_WIN"

mkdir -p build

cat > build/build.bat <<BATEOF
@echo off
setlocal
set "VCVARS=$VCVARS_WIN"
call "%VCVARS%" x64 >nul 2>&1
if errorlevel 1 (echo vcvarsall failed & exit /b 1)

cl /nologo /Zi /O2 /W3 /utf-8 /std:c++14 /EHsc /D_CRT_SECURE_NO_WARNINGS ^
   /Isrc /Fo:build\\ /Fd:build\\ /Fe:build\\minpi.exe ^
   src\\main.cpp src\\ai\\ai.cpp src\\agent\\agent.cpp src\\core\\json.cpp src\\core\\term.cpp src\\core\\codecvt.cpp src\\core\\tools.cpp ^
   /link winhttp.lib ws2_32.lib
if errorlevel 1 (echo Build failed. & exit /b 1)
echo Build OK: build/minpi.exe
endlocal
BATEOF

cmd //c build\\build.bat
