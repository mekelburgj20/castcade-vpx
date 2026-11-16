# tools/castcade/pair.ps1

Runs two VPX instances side by side on one PC: **play** (`CASTCADE_STREAM=play`, listens on
`127.0.0.1:<Port>`) and **watch** (`CASTCADE_STREAM=watch`, connects to it). Optionally has the play
instance play a game from a script file, captures paired screenshots, closes both with `WM_CLOSE`,
and compares the pictures. Run with `pwsh` (PowerShell 7, 64-bit).

    pwsh -NoProfile -File pair.ps1 -Table "<table.vpx>" [-Exe <VPinballX_GL64.exe>] [-Seconds 60] [-Drive none|game]

Parameters: `-Table` (required), `-WatchTable` (default same; a different one tests a refused
mismatch), `-Exe` (default `C:\code\repos\castcade-vpx\build\Release\VPinballX_GL64.exe`), `-WatchExe`,
`-Seconds` (run time after both player windows exist, 60), `-Drive none|game`, `-Seed` (1),
`-WatchDelaySeconds` (late joiner, 0), `-WatchFirst` (watcher first, player 6 s later), `-Port` (47800),
`-DelayMs` (50), `-ShotEvery` (5 s), `-OutDir`, `-Width`/`-Height` (960x540), `-KeepOpen` (leave both
running, prints PIDs), `-WindowTimeout` (120 s).

`-Drive game` writes `<run>\play\input.txt` (`<ms> <down|up> <key>`, ms from the play instance's first
rendered frame, keys `start credit plunger lflip rflip`, `#` comments) and passes it as
`CASTCADE_TEST_INPUT`; the build fires the events inside VPX. Schedule: credit x2 from 8 s, start,
plunger 3 s later, then seeded flipper taps every 0.7-1.6 s, plunger every 12 s, start every 45 s, for
`-Seconds` + 10 s. Both instances get `CASTCADE_TEST_NOPAUSE=1`.

Output: `C:\code\repos\castcade-vpx-deps\run\pair\<yyyyMMdd-HHmmss>\` with `play.ini`, `watch.ini`,
`play\` and `watch\` (working dirs: `vpinball.log`, `castcade.log`, `input.txt`), `shots\tSSS-play|watch.png`,
`compare.csv` (time, mean_abs_diff, pct_over_40 at 192x108), `sheet.png` (up to 8 pairs, play left,
watch right), `pair.log`, `pids.json`. The summary shows the event count written and the number of
`test input` lines in `play\castcade.log`. Exit code 0 only if both instances started and exited cleanly
and the registry / settings restore succeeded (comparison numbers do not matter).

Safety: the harness sends no keyboard or mouse input to any window and never changes the foreground
window. It closes only PIDs it started (`WM_CLOSE`; kill only as a last resort on its own PID), finds
windows by PID, always passes `-Ini` (the owner's ini is never touched), keeps VR off, backs up and
restores the HKCU type-library key, editor key and `%APPDATA%\VPinballX\BAMViewSettings.xml` in a
`finally` block, refuses to start if an earlier run's PID is alive, and checks the port on loopback only.
Screenshots use PrintWindow, which does not take focus.
