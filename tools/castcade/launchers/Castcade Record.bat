@echo off
rem Castcade: record a game you play, to watch it later in your own VPX as if someone were playing it live
rem ("Castcade Watch Recording.bat"). Keep this file next to VPinballX_GL64_castcade.exe. Drag a .vpx onto it, or
rem run it and open the table from the editor. Play as usual: the recording is complete when you close the table.
rem It goes to the castcade-recordings folder beside this file. Nothing is sent over the network.
setlocal
cd /d "%~dp0"
if not exist "%~dp0VPinballX_GL64_castcade.exe" (
  echo VPinballX_GL64_castcade.exe is not in this folder: %~dp0
  pause
  exit /b 1
)
set "CASTCADE_STREAM=play"
rem No address: the player records and opens no network connection at all
set "CASTCADE_STREAM_ADDR="
set "CASTCADE_STREAM_RECORD=%~dp0castcade-recordings\"
set "CASTCADE_STREAM_LOG=%~dp0castcade-record.log"
if "%~1"=="" (
  start "" "%~dp0VPinballX_GL64_castcade.exe"
) else (
  start "" "%~dp0VPinballX_GL64_castcade.exe" -Play "%~1"
)
