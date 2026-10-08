@echo off
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
where cl >nul 2>&1 && goto build
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
:build
cl /nologo /O2 /EHsc /utf-8 /DUNICODE /D_UNICODE test_wic.cpp /Fe:test_wic.exe /Fo:test_wic.obj
cl /nologo /O2 /EHsc /utf-8 /DUNICODE /D_UNICODE bench.cpp /Fe:bench.exe /Fo:bench.obj user32.lib gdi32.lib
