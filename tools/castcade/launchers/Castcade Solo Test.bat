@echo off
rem Castcade state stream: one person, one PC. A player instance runs in a desktop window (VR off) and a watcher
rem instance runs with your normal settings (in the headset, if that is how you play). Drag a .vpx onto this file.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0castcade-solo.ps1" -Table "%~1"
if errorlevel 1 pause
