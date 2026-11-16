# tools/castcade/pair.ps1

Runs two VPX instances side by side on one PC: **play** (`CASTCADE_STREAM=play`) and **watch**
(`CASTCADE_STREAM=watch`, connects to `127.0.0.1:<Port>`). Optionally plays a game on the play
instance with synthetic key presses, captures paired screenshots, closes both with `WM_CLOSE`,
and compares the pictures. Run with `pwsh` (PowerShell 7, 64-bit).

    pwsh -NoProfile -File pair.ps1 -Table "<table.vpx>" [-Exe <VPinballX_GL64.exe>] [-Seconds 60] [-Drive none|game]

Parameters: `-Table` (required), `-WatchTable` (default same; a different one tests a refused
mismatch), `-Exe` (default `C:\code\repos\castcade-vpx\build\Release\VPinballX_GL64.exe`), `-WatchExe`,
`-Seconds` (run time after both player windows exist, 60), `-Drive none|game`, `-WatchDelaySeconds`
(late joiner, 0), `-WatchFirst` (watcher first, player 6 s later), `-Port` (47800), `-DelayMs` (50),
`-ShotEvery` (5 s), `-OutDir`, `-Width`/`-Height` (960x540), `-KeepOpen` (leave both running, prints PIDs),
`-WindowTimeout` (120 s).

`-Drive game` (play instance only): after 8 s, Add Credit x2, Start, plunger, then seeded
pseudo-random flipper taps, plunger every 12 s, Start every 45 s. The generated inis set the keys
StartGame=2, AddCredit=6, Plunger=28 (Enter), LFlip=42, RFlip=54, ExitGame=16, VR off.

Output: `C:\code\repos\castcade-vpx-deps\run\pair\<yyyyMMdd-HHmmss>\` with `play.ini`, `watch.ini`,
`play\` and `watch\` (working dirs: `vpinball.log`, `castcade.log`), `shots\tSSS-play|watch.png`,
`compare.csv` (time, mean_abs_diff, pct_over_40 at 192x108), `sheet.png` (up to 8 pairs, play left,
watch right), `pair.log`, `pids.json`. Exit code 0 only if both instances started and exited cleanly
and the registry / settings restore succeeded (comparison numbers do not matter).

Safety rules enforced: only PIDs it started are closed (WM_CLOSE; kill only as a last resort on its own
PID); windows are found by PID, never by title alone; every key down/up is gated on the play player window
being the foreground window (one attempt to take focus, else the key is skipped and counted; a pressed key is
always released when the window is foreground); screenshots use PrintWindow and do not take focus; always
`-Ini` (the owner's ini is never touched); VR stays off; the HKCU type-library key, editor key and
`%APPDATA%\VPinballX\BAMViewSettings.xml` are backed up and restored in a `finally` block; no process or
earlier-run PID may be alive at start; the port check binds loopback only (0.0.0.0 raises a Windows
Firewall prompt).
