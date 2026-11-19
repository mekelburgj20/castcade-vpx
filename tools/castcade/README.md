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

# tools/castcade/match-test.ps1

Turn hand-off test for the match role (`CASTCADE_STREAM=match`, `CASTCADE_STREAM_CONTROL=connect:127.0.0.1:<port>`;
both are described at the top of `src/castcade/CastcadeStream.h`). It starts two or three VPX instances on one PC
and plays the companion's part itself: it listens on each instance's control port, reads `hello` and the `state`
lines, and sends `watch` and `play`. Same window, ini, input and capture rules as `pair.ps1` (desktop windows from a
generated `-Ini`, VR off, sound off, keys fired inside VPX through `CASTCADE_TEST_INPUT`, PrintWindow, `WM_CLOSE`,
loopback only). Ports: control `BasePort+1..3`, streams `BasePort+11..13`, within 48100-48199. Run with `pwsh`.

    pwsh -NoProfile -File match-test.ps1 -Table "<table.vpx>" [-Rom <rom>] [-Swaps 50] [-Check] [-Scenario swap]

The instances run from a copy of `-Exe` named `-RunAs` (`ccmatch_GL64.exe`) beside it: the installed Castcade
companion takes any `VPinballX*.exe` that loads a table for a game of its own. Do not build while a test runs: the
compiler takes the cores, and the frame probe then shows frames of 200 ms and more that the frame limiter slept
through.

`-Scenario`:

- `swap` (default): instances A and B both play their own scripted game; `-Swaps` times, alternately, one of them is
  told to watch the other for `-WatchSeconds` and is then called back, with `-PlaySeconds` of play in between. Around
  every watch phase the watcher's window is captured: twice 120 ms apart before it (the difference of those two is the
  noise of a running game), in the middle of it (the watched player's window first, the watcher's 70 ms later, which
  is the delay it shows the stream with), and right after `state playing`.
- `contract`: what the companion's control client counts on: the first two lines of a connection (`hello`, `state
  playing`, before anything is sent), the tokens of `hello` and one identity for one table file, a `play` while
  playing, a `watch` of a port where nothing listens (`state failed timeout` within 5 s, then `state playing` by
  itself), a `watch` right after `state playing`, `play` and `watch` in one write, the same `watch` twice, an unknown
  command, a `watch` without an address, and the watched player closing its table (`state failed closed`).
- `nolistener`: one instance, told to watch a port where nothing listens. `streamlost`: B watches A and A is killed
  (the script's own PID): `state failed lost` after 10 s. `controllost`: B watches A, the script cuts B's control
  connection; B must be back on its own table by itself and connect again. `mismatch`: B loads `-OtherTable` and is
  told to watch A: `state failed refused`, its table never touched.
- `reaim`: a third instance C watches A, is re-aimed at B with a second `watch` (which must be answered by a second
  `state watching`), then called back. `plainwatch`: W, in the plain watch role, watches A the whole time while A and B
  swap: the match role's stream is the play role's, and stands still while A watches.

`-Check` sets `CASTCADE_TEST_MATCHCHECK=1`: before each watch phase and after it the instance reads every property of
every part that takes no argument (about 4000 on Nudge Test, 22000 on Attack from Mars) and logs each one that differs;
the summary counts them. Every instance also logs, always, whether its physics state after the watch (the bytes a
watcher of it is sent: moving parts, balls, nudge, game clock) is identical to the one before. `-Rom` names the
VPinMAME roms whose `nvram\<rom>.nv` and `cfg\<rom>.cfg` are copied before the run and put back after it (two
instances write them when they close). `-StaticRefresh` sets `CASTCADE_STREAM_STATICREFRESH=1`. `-NoFrameLog` runs
without the frame probe. `-ExtraEnv @{...}` adds environment variables to every instance.

Output under `C:\code\repos\castcade-vpx-deps\run\match\<yyyyMMdd-HHmmss>-<label>\`: `match.log`, `swaps.csv` (one row
per watch phase: the milliseconds to `state watching` and to `state playing`, the four picture differences),
`summary.json`, `control-<instance>.log` (every control line with its time), `<instance>\castcade.log`, `frames.log`,
`vpinball.log`, `input.txt`, and `shots\` (the pictures of every watch phase that failed and of about a dozen others).

The run fails (exit code 1, `PROBLEM:` lines) when: a watch phase did not end as the scenario expects; a `watch` was
answered after 5 s or a `play` after 2 s; a picture after a watch differs from the one before it by more than
`-MaxPictureDiff` (mean absolute difference of the RGB values, 0 to 255, of the two captures scaled to 192x108; default
1.5), or, over a `swap` run, the median of those differences is above 1.25 times the median difference of the two
pictures taken 120 ms apart before each watch, plus 0.1 (the pictures are a weak witness on a busy table: with
`-ExtraEnv @{ CASTCADE_TEST_MATCHNOUNDO = '1' }`, which leaves the other player's lamps on the table, Attack from Mars
stays under 1.5 on every swap and fails only this median and the property check, with about 200 properties each
time); an instance reports a different physics state, a property it could not put back, a replayed call that failed
or, with `-Check`, a property that differs; a frame of `-MaxSwitchFrameMs` (100) or more ended within a second after
a switch (entering, the keyframe, leaving) or began up to 0.3 s before one; an instance did not exit with code 0; or
the registry, `BAMViewSettings.xml` or the VPinMAME files could not be put back. The summary also counts, from
`CASTCADE_TEST_BALLLOG`, the balls each game put in play on its own game clock (a ball in the upper 60 % of the range
of y positions seen, after 2 s without one), how many of them after its first watch phase, and how many watch phases
began with a ball of its own in motion.

Like `replay-test.ps1` it puts back what it found of the type-library registration, which can be another test build's
value: import `run\regbackup\hkcu-typelib-before.reg` once after the last run.

# tools/castcade/replay-test.ps1

Record and replay test for `CASTCADE_STREAM_RECORD` and `launchers/castcade-replay.ps1`, in the same style
as `pair.ps1` (desktop windows from a generated `-Ini`, VR off, sound off, keys fired inside VPX through
`CASTCADE_TEST_INPUT`, PrintWindow captures, `WM_CLOSE`, loopback only; ports 47920-47939). Run with `pwsh`.

    pwsh -NoProfile -File replay-test.ps1 -Mode record -Table "<table.vpx>" -InputFile <keys.txt> [-WithWatcher] [-Seconds 60] [-Port 47921]
    pwsh -NoProfile -File replay-test.ps1 -Mode replay -Table "<table.vpx>" -Recording <file.castrec> [-Connection perfect|good|fair|poor] [-Loop -Passes 2] [-ShotsFrom <record run>]

`-Mode record`: a player records into `<run>\rec\`; with `-WithWatcher` it also listens on
`127.0.0.1:<Port>` and a live watcher A connects, without it the player has no address and its sockets are
logged 10 s in. Player (and A) captures every `-ShotEvery` s and just before the close; `shots.json` holds each
player capture's time on the recording's clock. `-Mode replay`: a watcher B connects to the replay tool, run
under Windows PowerShell 5.1 as a user would run it; with `-ShotsFrom` B is captured at the recording times of
the player's captures (plus the delay), then after the end (`final-watchB.png`, and `final-late-watchB.png`
once the "player closed the table" note is gone). Output under
`C:\code\repos\castcade-vpx-deps\run\rr\<yyyyMMdd-HHmmss>-<mode>\`: `run.log`, per-instance `castcade.log`,
`shots\`, `replay.log`, `watchB-totals.json` (the watcher's stats lines summed: received, starved, buffering,
jumps, failed, unresolved). It does not restore the type-library registration or `BAMViewSettings.xml`: do
that once after the last run. `-Sound` turns VPX's table sounds on at volume 0 in every instance (the
StopSound audit, `CASTCADE_STREAM_STOPAUDIT=1`, needs it on the watcher); `-PlayEnv` / `-WatchEnv` add
environment variables (`@{ CASTCADE_STREAM_SMOOTH = '0' }`); `-FrameLog <folder>` runs the frame probe in
every instance; `-WatchExe` runs the watchers from another build than the player (an older watcher against a
newer player).

# tools/castcade/hitch-test.ps1

One VPX plays a scripted game with the frame probe on (`CASTCADE_FRAMELOG`, every frame of 25 ms or more
with VPX's profiler sections and the render thread's sampled call stacks), the stream off (`-Mode off`: no
session, each hook one null test) or recording (`-Mode record`). Same window, ini and input rules as above;
no socket is opened.

    pwsh -NoProfile -File hitch-test.ps1 -Table "<table.vpx>" -InputFile <keys.txt> [-Mode off|record] [-Seconds 200] [-Sound] [-ExtraEnv @{...}]

Output under `C:\code\repos\castcade-vpx-deps\run\hitch\<yyyyMMdd-HHmmss>-<label>\`: `run.log`,
`play\frames.log`, `play\castcade.log`, `rec\` and `summary.json` (counts of frames of 25, 50, 100, 200 and
500 ms or more, and the dominant section of each frame of 50 ms or more).

Test switches the fork reads (with or without a stream session): `CASTCADE_TEST_INPUT`, `CASTCADE_TEST_NOPAUSE`,
`CASTCADE_TEST_STALL=<ms>,<every s>[,<first s>]` (stalls the render thread on purpose, after the physics),
`CASTCADE_FRAMELOG`, `CASTCADE_FRAMELOG_MS`; on a session: `CASTCADE_STREAM_STOPFILTER=0`,
`CASTCADE_STREAM_CATCHUP=0` (player), `CASTCADE_STREAM_SMOOTH=0`, `CASTCADE_STREAM_STOPAUDIT=1` (watcher).
`CASTCADE_TEST_PAUSE=<at s>,<for s>[,<every s>]` pauses a player as losing the window's focus does and resumes it
as getting it back does; `CASTCADE_TEST_DROP=<s>[,<s>...]` cuts a watcher's connection that many seconds after its
session began (it reconnects as usual); `CASTCADE_TEST_EXITWARN=<n>` writes n warnings as a session ends, while
its log's thread mirrors them into the VPX log. `hitch-test.ps1 -NoStreamLog` leaves CASTCADE_STREAM_LOG unset, so
every session line goes to the VPX log.
