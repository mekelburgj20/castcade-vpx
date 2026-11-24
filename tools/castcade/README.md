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

The instances run from a copy of `-Exe` named `-RunAs` (`cchand_GL64.exe`) beside it: the installed Castcade
companion takes any `VPinballX*.exe` that loads a table for a game of its own. `-YieldTo` (default `ccmatch_GL64`)
names another test's instances on the same PC: the run waits, looking once a minute for 45 minutes at most, while
one exists. Do not build while a test runs: the compiler takes the cores, and the frame probe then shows frames of
200 ms and more that the frame limiter slept through.

`-Scenario`:

- `swap` (default): a match as the companion runs it. After `-WarmupSeconds` in which both instances start their
  scripted game, A is up and B watches it; then, `-Swaps` times, the watcher is called back (`play`) while the player
  is sent to watch it (`watch`), a turn lasting `-WatchSeconds`. The two commands leave the companion at the same
  moment, so the test sends them in three orders by turns: the `watch` first and the `play` 150 ms later (the new
  watcher then reaches a table that is still watching: its connection must be held and served once that table
  plays), the `play` first and the `watch` 150 ms later, and both at once. Each game moves on only in its own turns.
  Pictures: the table that is sent to watch, twice 120 ms apart just before (the difference of those two is the noise
  of a running game; the second is kept); the table that is called back, right after its `state playing`, compared
  with the picture kept from before its watch; and in the middle of the turn the player's window, then the watcher's
  70 ms later (the delay it shows the stream with).
- `visit`: the first form of the test: both instances play, and `-Swaps` times, alternately, one of them is told to
  watch the other for `-WatchSeconds` and is called back, with `-PlaySeconds` of play in between.
- `contract`: what the companion's control client counts on (the rules of §3 of `docs/design/state-stream-in-match.md`
  in the castcade repo): the first two lines of a connection (`hello`, `state playing`, before anything is sent), the
  five tokens of `hello`, its stream port, and one identity for one table file; a `play` while playing; a `watch` of a
  port where nothing listens (`state failed timeout` within 5 s, then `state playing` by itself); a `watch` right
  after `state playing`; bursts in one write, of which only the last command is acted on and answered (`play` +
  `watch` while watching is a re-aim with one `state watching` and no `state playing`; `watch` + `play` is one
  `state playing`; five pairs give one answer); the same `watch` again (a re-aim to the same address: a new `state
  watching`, no `state playing` in between); a raw connection to the stream port of an instance that is watching (not
  a byte until it plays, then its HELLO; and closed, still silent, after 10 s if it does not); an unknown line, an
  empty line and a line of the other direction while watching (ignored: no answer, and the watch goes on); `watch`
  with an address that is none or not this PC's (`nowhere`, `127.example.com`, `192.168.1.20`, no port, port 0:
  `state failed badaddress`, `state playing`); a line of 3000 bytes (the connection is closed and made again); and the
  watched player closing its table (`state failed closed`).
- `deadwatch`: a watch that never shows anything, in the middle of a game: B is told to watch a port where nothing
  listens and called back 150 ms later, `-Swaps` times with 0.8 s of play in between, and every 10th time left to its
  own 4 s timeout. Each must leave B's game as a pause of that length would: with `-Check` no property and no
  internal field differs, and no event was kept from its script. The summary says how many of them began with a
  drop target moving, a light sequence playing, a lamp under `Duration`, lamps blinking, a reel turning.
- `overflow`: B runs with `CASTCADE_TEST_UNDOLIMIT=10` (a stream may put 10 properties instead of as many as the
  table has, plus 256) and is told to watch A: `state failed overflow`, `state playing`, its table as it was.
- `closewhile`: B watches A and B's table is closed: B must be back on its own game before its script ends (the log
  line `back to playing (this table closes)`) and exit with code 0. `longwatch`: B watches A for `-LongMinutes` (10);
  B's private bytes, working set, handles and threads before, 20 s in, at the end and 10 s after its return.
- `focus`: a table whose window does not have the focus. The harness never changes the foreground window and VPX
  counts both test windows as active, so B runs without `CASTCADE_TEST_NOPAUSE` and with `CASTCADE_TEST_PAUSE=14,3600`,
  which marks its window inactive and pauses it as `WM_ACTIVATE` does. Paused like that it is sent to watch A and
  called back six times: it must show A's table, and be paused again by VPX after each return.
- `nolistener`: one instance, told to watch a port where nothing listens. `streamlost`: B watches A and A is killed
  (the script's own PID): `state failed lost` after 3 s. `stalled`: B watches A and A is suspended (its sockets stay
  open and deliver nothing): `state failed stalled` after 3 s; A is resumed and watched again. `controllost`: B
  watches A, the script cuts B's control connection; B must be back on its own table by itself and connect again.
  `mismatch`: B loads `-OtherTable` and is told to watch A: `state failed refused`, its table never touched.
- `reaim`: a third instance C watches A, is re-aimed at B with a second `watch` (which must be answered by a second
  `state watching`), then called back. `plainwatch`: W, in the plain watch role, watches A the whole time while A and B
  swap: the match role's stream is the play role's, and stands still while A watches.

`-Check` sets `CASTCADE_TEST_MATCHCHECK=1`: before each watch phase and after it the instance reads every property of
every part that takes no argument and returns no object (3913 on Nudge Test, 19916 on Attack from Mars, 49263 on
Monster Bash) and logs each one that differs; the summary counts them. A getter that faults is named in the log and
left out (stock VPX: `Light.FilamentTemperature` of a light whose intensity is 0, six of them on Monster Bash). It
also compares, part by part, the internal state no property shows (`CaptureDeepState`: a lamp's stored state beside
its play state, sequencer locks, blink and animation phases and their time stamps, the movers, which hit objects
collide, when each timer fires next, the balls, the game clock, the ball id counter, the music): 434 parts and lists
on Attack from Mars. Every instance also logs, always, whether its physics state after the watch (what a watcher of
it is sent: moving parts, balls, the table's displacement, game clock; not the accelerometer's reading) is identical
to the one before, how many events were kept from its script before the keyframe (there must be none) and after it,
and what became of its music. `-Rom` names the
VPinMAME roms whose `nvram\<rom>.nv` and `cfg\<rom>.cfg` are copied before the run and put back after it, bytes and
time stamp, together with `cfg\default.cfg` (two instances write them when they close). `-StaticRefresh` sets
`CASTCADE_STREAM_STATICREFRESH=1`. `-NoFrameLog` runs without the frame probe. `-ExtraEnv @{...}` adds environment
variables to every instance (through `pwsh -Command "& match-test.ps1 ... -ExtraEnv @{...}"`: `-File` cannot pass a
hashtable).

`-Sound` writes `PlaySound = 1` and `PlayMusic = 1` into the ini, both at volume 0, and has each match instance call
the table's own `PlayMusic` with a copy of `-MusicFile` 5 s after its start (`CASTCADE_TEST_MUSIC`; the two tables on
the test PC whose scripts call `PlayMusic` both need PinUP Player beside them). The summary then says how often a
watch began with the table's music playing and how often it was playing again after the return. `-StreamSound` sets
`CASTCADE_STREAM_SOUND=1` (a match runs with 0). Limit of that mode: a watcher stops every sound when a stream begins
and ends, so a looping sound the own table had started (`PlaySound` with a loop count of -1) is stopped by the first
watch and stays off until the script starts it again; with `CASTCADE_STREAM_SOUND=0` no sound of the own table is
touched.

`-AttractSeconds <n>` moves every key of the scripted game n seconds back: the tables stay in their attract mode that
long, so the first watch phases begin while an original table plays its light sequences (the summary counts the
phases that began with a sequence playing), and the game starts after them. `-CacheMode 1` writes `CacheMode = 1`
into the ini (VPX then loads every texture to the GPU before the game starts).

`-OptionEvent` sets `CASTCADE_TEST_OPTIONEVENT=1`: each match instance fires the tweak menu's "table option changed"
event (`OptionEvent 1`) once in every watch phase, after the keyframe; the run fails unless each was kept from the
script and fired at the return.

`-HoldKeys` sets `CASTCADE_TEST_HOLDKEYS=1`: each match instance puts key events of its own into VPX's key queue
(`PinInput::PushQueue`, where the keyboard read puts a real key; nothing is sent to the system): F13 is held while the
table plays and let go in the first frame after a `watch` command, F14 is tapped once a frame until the keyframe (30
times at most) and three times after it. After every watch phase the script must have been told the key-up of F13
and nothing else, and no event of the two may reach it through the queue; that holds for a watch that never showed
a table too (`-Scenario deadwatch -HoldKeys`: without the key read during the hold all of them reached the script
after the return, 581 events in 20 dead watches, and the 32-entry queue lost the rest).

`-Sidecar <table>.vbs` (and `-HookLib`, default the owner's `scripts\castcade.vbs`) runs a patched table, as match
tables are: each instance gets its own copy `cchand-<x> <table>.vpx` in its run folder with the sidecar under the same
name and the hook library beside it. The hook names its journal after the table id in the sidecar's `Castcade_Init`
line, not after the file, and `Castcade_Guard` names the original table and the sidecar it would rename: in the
copy the id gets the prefix `cchand-<x> ` and both paths are the copies' (nothing else is changed), so the hook
writes `%LOCALAPPDATA%\Castcade\journal\cchand-<x>_....jsonl` and cannot reach the installed table's files. In
the `swap` scenario the journal of the table that watches must not grow during the watch. At the end both journals
are copied into the run folder (`journal-<x>.jsonl`), removed from Castcade's folder, and read by `-JournalTool` (the
castcade repo's `vpx-hook/spike/tail-journal.py`): balls seen ending, `game_end`. `-PlayOutSeconds <n>` lets both
tables play on for n seconds after the scenario with no flipper pressed (and starts one game of one player only), so
that the games end.

Output under `C:\code\repos\castcade-vpx-deps\run\match\<yyyyMMdd-HHmmss>-<label>\`: `match.log`, `swaps.csv` (one row
per watch phase: the milliseconds to `state watching` and to `state playing`, the four picture differences),
`summary.json`, `control-<instance>.log` (every control line with its time), `<instance>\castcade.log`, `frames.log`,
`vpinball.log`, `input.txt`, and `shots\` (the pictures of every watch phase that failed and of about a dozen others).

The run fails (exit code 1, `PROBLEM:` lines) when: a watch phase did not end as the scenario expects; a `watch` was
answered after 5 s or a `play` after 2 s; a picture after a watch differs from the one before it by more than
`-MaxPictureDiff` (mean absolute difference of the RGB values, 0 to 255, of the two captures scaled to 192x108; default
1.5) and by more than 1.25 times the largest difference of the run's pairs taken 120 ms apart with no watch (the two
pictures are 50 to 150 ms of the game apart, and Flash switches its whole lighting between two frames when it serves
a ball: 6 to 7 on that scale; such a picture is named in a `NOTE:` line), or, over a `swap` run, the median of those differences is above 1.25 times the median difference of the two
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

# tools/castcade/editor-close-test.ps1

The editor-close crash (I11.11.1). Starts one VPX on a table (`-Exe`, not a `VPinballX*` name; `-Table`), lets it
play 6 s, posts `WM_COMMAND` with wParam `0xF060` (`SC_CLOSE`) to the editor frame, which is what a click on the table
view's close "x" at the right of the menu bar posts, and waits 8 s. Exit code 1 and `RESULT: the process ended ...,
exit code -1073741819` on a build without the fix; exit code 0 and `RESULT: the game went on; closed normally
afterwards, exit code 0` with it (it then closes the player window, and VPX must end by itself).

    pwsh -NoProfile -File editor-close-test.ps1 -Exe <cchand_GL64.exe> -Table "<table.vpx>"

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
`CASTCADE_STREAM_PAUSECLOCK=0` (player) and `CASTCADE_STREAM_RESYNC=0` (watcher) give back what each end did before
2026-11-24 (I11.4.17, below): a paused player's stamp stands still, and a watcher holds its frames as stamped and
never starts its clock again.
`CASTCADE_TEST_PAUSE=<at s>,<for s>[,<every s>]` pauses a player as losing the window's focus does and resumes it
as getting it back does; `CASTCADE_TEST_DROP=<s>[,<s>...]` cuts a watcher's connection that many seconds after its
session began (it reconnects as usual); `CASTCADE_TEST_EXITWARN=<n>` writes n warnings as a session ends, while
its log's thread mirrors them into the VPX log. `hitch-test.ps1 -NoStreamLog` leaves CASTCADE_STREAM_LOG unset, so
every session line goes to the VPX log.

# src/castcade/tests/timeline_test: the stream's clock without VPX (I11.4.17)

`src/castcade/CastcadeTimeline.h` holds the two ends of the stream's clock with nothing of VPX in them: `PlayStamp`
(the stamp a player gives a frame) and `WatchTimeline` (the frames a watcher holds, and which of them the next
picture shows). `timeline_test` runs them on a simulated clock: a player that stamps as the fork does, a connection
(37 ms and a frame 25 ms late now and then), a watcher at 90 frames a second with a delay of 100 ms. It starts no
VPX, opens no socket and writes no file. Build and run it (a second or two):

    cmake -G "Visual Studio 17 2022" -A x64 -S src\castcade\tests -B <a build folder outside the repository>
    cmake --build <that folder> --config Release --target timeline_test
    <that folder>\Release\timeline_test.exe        (-v: every stats line of every run)

Scenarios: `steady` and `gap` (a stream whose clock runs: both ends must show it exactly as before 2026-11-24, and
the test compares every count), `pause` (the first Congo match of 2026-11-23: the player's VPX has been paused for
122 s when the watcher connects and plays 39 s later), `pause, exact`, `pause while watched`, `frozen` (the match
role's one stamp while the watched table itself watches) and `back` (stamps set back by 10 s). Each runs four times:
the watcher's rules and the player's stamps of before that date (`BEFORE`, where `pause` must fail as the watcher did
that night: intervals with frames received and `replayed calls 0`, a timeline of thousands of frames, a span below
zero, "2389 ms behind when frames came back"), a new watcher against an older player, an older watcher against a new
player, and both new. Then the rule of last resort alone (frames nothing will ever make due are shown within 2.1 s)
and the player's stamps through a pause. Exit code 0 only when every check passes.

The same on two VPX instances, with one build (the watcher's log is `<run>\watchA\castcade.log`, the player's
`<run>\play\castcade.log`; copy `build\Release\VPinballX_GL64.exe` to a name that does not start with `VPinballX`
first, and delete the copy afterwards):

    pwsh -NoProfile -Command "& tools\castcade\replay-test.ps1 -Mode record -WithWatcher -Seconds 80 -Exe <copy> -Table '<table.vpx>' -PlayEnv @{ CASTCADE_TEST_PAUSE = '15,40'; CASTCADE_TEST_NOPAUSE = '0'; CASTCADE_STREAM_PAUSECLOCK = '0' }"

That is a new watcher against a player that stamps as before, paused from 15 s after its first frame for 40 s: a
second into the pause the watcher logs `the player's clock stands still`, every stats line of the pause has `timeline
1 frames` and `the player's clock stands still`, and when the pause ends `timeline resynchronised: the player's clock
runs again after standing for 40.0 s, 40.0 s ahead of where it stood`.
Add `-WatchEnv @{ CASTCADE_STREAM_RESYNC = '0' }` for the fault itself (once a frame of the pause is stamped late,
which is chance: stats lines with a timeline that grows by the frames received, and no call replayed if the table's
frame timers make any); leave `CASTCADE_STREAM_PAUSECLOCK` out for a new player (its log says `this table's game
stands still (window not active, paused by VPX)` and `runs again after standing still for 40.0 s`; the watcher's
timeline stays at its usual 15 frames or so and nothing is resynchronised, with or without `CASTCADE_STREAM_RESYNC=0`).
