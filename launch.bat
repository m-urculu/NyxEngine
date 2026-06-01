@echo off
REM Nyx launcher - runs the most recently built Nyx.exe (Release or Debug) from
REM the project root, so the engine's relative shaders\ and assets\ paths resolve.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$e = Get-ChildItem 'build\Release\Nyx.exe','build\Debug\Nyx.exe' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName; if ($e) { Start-Process -FilePath $e -WorkingDirectory '%CD%' } else { Write-Host 'No Nyx.exe found under build\. Build it first: cmake --build --preset windows-debug'; pause }"
