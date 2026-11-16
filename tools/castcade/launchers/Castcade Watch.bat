@echo off
rem Castcade state stream: watch another player's game in your own VPX, from your own viewpoint (in VR: your own head).
rem Keep this file next to VPinballX_GL64_castcade.exe. Drag the SAME .vpx file the player uses onto it, or run it and
rem open that table from the editor.
setlocal
cd /d "%~dp0"
if not exist "%~dp0VPinballX_GL64_castcade.exe" (
  echo VPinballX_GL64_castcade.exe is not in this folder: %~dp0
  pause
  exit /b 1
)
echo.
echo   Castcade - WATCH (you see the other player's game on your own table)
echo.
echo   Type the player's address (their public IP, or their PC's name or IP on your network).
echo.
echo   Or press Enter to wait for the player to connect to YOU on TCP port 47800, if you are the
echo   one who can accept connections (firewall "Allow", router forward of TCP 47800).
echo.
set "HOST="
set /p "HOST=  Player's address (Enter = wait for the player): "
set "CASTCADE_STREAM=watch"
if "%HOST%"=="" (set "CASTCADE_STREAM_ADDR=listen:47800") else (set "CASTCADE_STREAM_ADDR=connect:%HOST%:47800")
rem How far behind the newest received frame to render, in milliseconds. 50 is enough on one network;
rem over the internet 100 to 150 hides the jitter. Raise it if the ball stutters, lower it for less delay.
set "CASTCADE_STREAM_DELAY_MS=100"
rem 1 = replay the table's own sound effects here (flippers, bumpers, rolling). 0 = silent table, for when the
rem player's sound already reaches you another way.
set "CASTCADE_STREAM_SOUND=1"
rem 1 = when the player's frames stop for a moment (the player's PC stalled), catch up quickly when they come back.
rem 0 = jump to where the game is instead (as before), to compare.
set "CASTCADE_STREAM_SMOOTH=1"
set "CASTCADE_STREAM_LOG=%~dp0castcade-watch.log"
if "%~1"=="" (
  start "" "%~dp0VPinballX_GL64_castcade.exe"
) else (
  start "" "%~dp0VPinballX_GL64_castcade.exe" -Play "%~1"
)
