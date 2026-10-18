@echo off
REM Assembles a self-contained, redistributable TAREEK-Vis folder in dist\TAREEK-Vis.
REM Run this AFTER a successful Release build (build.bat).
REM The resulting folder needs no Qt, no compiler, and no MSYS2 on the target machine.

setlocal

set PATH=C:\msys64\ucrt64\bin;%PATH%

set PROJECT_DIR=%~dp0
set BUILD_DIR=%PROJECT_DIR%build
set DIST_DIR=%PROJECT_DIR%dist\TAREEK-Vis

if not exist "%BUILD_DIR%\TAREEK-Vis.exe" (
    echo Build not found at %BUILD_DIR%\TAREEK-Vis.exe
    echo Run build.bat first.
    exit /b 1
)

echo Cleaning previous dist...
if exist "%PROJECT_DIR%dist" rmdir /s /q "%PROJECT_DIR%dist"
mkdir "%DIST_DIR%"

echo Copying executable...
copy /y "%BUILD_DIR%\TAREEK-Vis.exe" "%DIST_DIR%\" >nul

REM The licenses of the app and of the libraries and icons it ships must go
REM with every copy. The About box points to THIRD_PARTY_LICENSES.md.
echo Copying license files...
copy /y "%PROJECT_DIR%LICENSE" "%DIST_DIR%\" >nul
copy /y "%PROJECT_DIR%THIRD_PARTY_LICENSES.md" "%DIST_DIR%\" >nul

REM Ship only the plugins the app uses. Left out:
REM   generic             - touch-screen input (TUIO), not used
REM   qgif, qico          - the app reads PNG (OSM) and JPEG (satellite) tiles only
REM   qglib               - GLib network-status backend; Windows uses qnetworklistmanager
REM   qcertonlybackend    - TLS fallback; HTTPS tiles go through qschannelbackend
REM   D3Dcompiler_47.dll  - Windows 10 and 11 already have it in System32
echo Running windeployqt...
windeployqt6.exe --release --no-translations --no-system-d3d-compiler --no-opengl-sw --skip-plugin-types generic --exclude-plugins qgif,qico,qglib,qcertonlybackend "%DIST_DIR%\TAREEK-Vis.exe"
if errorlevel 1 (
    echo windeployqt failed!
    exit /b 1
)

REM A login shell builds PATH from MSYSTEM. Without MSYSTEM=UCRT64 it leaves out
REM /ucrt64/bin, ldd reports every MinGW DLL as "not found", and the loop below
REM copies nothing - the dist then fails to start on any PC without MSYS2.
echo Resolving and copying UCRT64 runtime DLLs (ldd, incl. ICU, transitively)...
set MSYSTEM=UCRT64
C:\msys64\usr\bin\bash.exe -lc "set -e; cd '%DIST_DIR:\=/%'; changed=1; while [ \"$changed\" = 1 ]; do changed=0; for f in $(find . -iname '*.dll' -o -iname 'TAREEK-Vis.exe'); do for dep in $(ldd \"$f\" 2>/dev/null | grep -oi '/ucrt64/[^ ]*\.dll'); do base=$(basename \"$dep\"); if [ ! -f \"./$base\" ]; then cp -u \"$dep\" .; changed=1; fi; done; done; done"
if errorlevel 1 (
    echo Failed to resolve/copy UCRT64 runtime DLLs via ldd!
    exit /b 1
)
for %%D in (libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll zlib1.dll) do (
    if not exist "%DIST_DIR%\%%D" (
        echo Runtime DLL %%D was not copied. The dist would not start on a clean PC.
        exit /b 1
    )
)

if exist "%PROJECT_DIR%ffmpeg\ffmpeg.exe" (
    echo Bundling FFmpeg...
    mkdir "%DIST_DIR%\ffmpeg"
    copy /y "%PROJECT_DIR%ffmpeg\ffmpeg.exe" "%DIST_DIR%\ffmpeg\" >nul
    if exist "%PROJECT_DIR%ffmpeg\LICENSE.txt" (
        copy /y "%PROJECT_DIR%ffmpeg\LICENSE.txt" "%DIST_DIR%\ffmpeg\" >nul
    ) else (
        echo WARNING: ffmpeg\LICENSE.txt not found. Put the FFmpeg license there before you share this folder.
    )
) else (
    echo FFmpeg not found at %PROJECT_DIR%ffmpeg\ffmpeg.exe - skipping.
    echo Video recording will require system FFmpeg on the target machine.
)

echo.
echo Done. Self-contained app is at: %DIST_DIR%
echo This folder can be zipped and shared directly, or wrapped by installer\TAREEK-Vis.iss.

endlocal
