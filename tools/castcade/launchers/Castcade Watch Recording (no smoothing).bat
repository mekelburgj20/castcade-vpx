@echo off
rem Castcade: as "Castcade Watch Recording.bat", with the watcher's smoothing OFF (CASTCADE_STREAM_SMOOTH=0): after a
rem gap in the recording the picture jumps to where the game is, as builds before 2026-11-16 did. To compare the two.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0castcade-watch-recording.ps1" -Table "%~1" -Smooth 0
if errorlevel 1 pause
