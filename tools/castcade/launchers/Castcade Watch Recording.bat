@echo off
rem Castcade: watch a game recorded with "Castcade Record.bat" in your own VPX, from your own viewpoint (in VR: your
rem own head), as if the player were live. Drag the SAME .vpx the recording was made with onto this file.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0castcade-watch-recording.ps1" -Table "%~1"
if errorlevel 1 pause
