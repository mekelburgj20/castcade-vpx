@echo off
rem Castcade state stream: play a table and let others watch it, each from their own VPX and their own viewpoint.
rem Keep this file next to VPinballX_GL64_castcade.exe. Drag a .vpx onto it, or run it and open the table from the editor.
setlocal
cd /d "%~dp0"
if not exist "%~dp0VPinballX_GL64_castcade.exe" (
  echo VPinballX_GL64_castcade.exe is not in this folder: %~dp0
  pause
  exit /b 1
)
echo.
echo   Castcade - PLAY (others watch your game)
echo.
echo   Press Enter to wait for watchers on TCP port 47800. Your PC must be reachable there:
echo   answer "Allow" when Windows Firewall asks about VPinballX_GL64_castcade.exe, and forward
echo   TCP 47800 on your router unless the watcher is on your own network.
echo.
echo   Or type the watcher's address, if the watcher is the one who can accept connections
echo   and is already waiting for you.
echo.
set "HOST="
set /p "HOST=  Watcher's address (Enter = wait for watchers): "
set "CASTCADE_STREAM=play"
if "%HOST%"=="" (set "CASTCADE_STREAM_ADDR=listen:47800") else (set "CASTCADE_STREAM_ADDR=connect:%HOST%:47800")
set "CASTCADE_STREAM_LOG=%~dp0castcade-play.log"
if "%~1"=="" (
  start "" "%~dp0VPinballX_GL64_castcade.exe"
) else (
  start "" "%~dp0VPinballX_GL64_castcade.exe" -Play "%~1"
)
