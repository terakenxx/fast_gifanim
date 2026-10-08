@echo off
rem FastGif build script (no CRT, single exe) -> publish\fastgif.exe
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
where cl >nul 2>&1 && goto build
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
:build
if not exist build mkdir build
if not exist publish mkdir publish

rc /nologo /fo build\fastgif.res fastgif.rc || exit /b 1

cl /nologo /c /O1 /Oi /GS- /Gs1000000 /Gy /GF /GR- /EHs-c- /W4 /utf-8 /Zc:threadSafeInit- ^
   /DUNICODE /D_UNICODE /Fobuild\main.obj main.cpp || exit /b 1

link /nologo build\main.obj build\fastgif.res kernel32.lib user32.lib gdi32.lib dwmapi.lib ^
   /SUBSYSTEM:WINDOWS /ENTRY:entry /NODEFAULTLIB /OPT:REF /OPT:ICF ^
   /MERGE:.rdata=.text /MANIFEST:NO /DEBUG:NONE /EMITPOGOPHASEINFO /DYNAMICBASE /NXCOMPAT ^
   /OUT:publish\fastgif.exe || exit /b 1

for %%f in (publish\fastgif.exe) do echo publish\fastgif.exe: %%~zf bytes
