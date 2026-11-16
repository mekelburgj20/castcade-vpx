Castcade state stream for Visual Pinball X 10.8.0 (first iteration)
====================================================================

What it does
------------
One VPX PLAYS a table as usual. Any other VPX with the SAME table file loaded can WATCH: it runs no
script and no physics, receives what the player's table does, and draws it from the watcher's own
camera. In VR that is the watcher's own head: you stand at your table and see the other player's ball,
flippers, lights and targets on it.

This build is Panasony's VPX 10.8.0 (OpenGL / VR, 64 bit) plus the stream. Without the launchers below it
behaves like that build.

Install
-------
Copy everything in this folder next to your existing VPinballX_GL64.exe (the Panasony 10.8.0 build).
Nothing is overwritten: the exe is named VPinballX_GL64_castcade.exe and uses the same ini, shaders,
scripts and tables as your normal one. If your VPX is not the Panasony 10.8.0 build, use the "full"
package instead: it is a complete folder with its own shaders and scripts.

Both players need this build and the identical .vpx file (same file, not just the same game). A watcher
whose table differs refuses the stream and says so on screen.

Ways to run it
--------------
1. Solo test, one PC:  drag a .vpx onto "Castcade Solo Test.bat".
   A player starts in a desktop window (VR off) and a watcher starts with your normal settings (in the
   headset, if that is how you play). Click the player window, put the headset on, play with the keyboard.

2. Two PCs, you play:  run "Castcade Play.bat" (or drag a .vpx onto it), press Enter to wait for watchers,
   start the table. The other PC runs "Castcade Watch.bat", types your address, starts the same table.

3. Two PCs, you watch: run "Castcade Watch.bat", type the player's address, start the same table.

4. Record a game:      drag a .vpx onto "Castcade Record.bat" and play as usual (your normal settings, so
   in the headset if that is how you play). When you close the table the game is saved in the
   castcade-recordings folder next to the exe. Nothing goes over the network: no firewall prompt.

5. Watch a recording:  drag the SAME .vpx onto "Castcade Watch Recording.bat". It takes the newest
   recording of that table (it asks when there are several) and asks which connection to simulate (Enter =
   perfect). A watcher starts with your normal settings and shows the recorded game as if the player were
   playing it live, on your table, from your viewpoint. When you are done, close the table and then VPX:
   the replay window closes with it.
   The connections other than "perfect" are SYNTHETIC: made-up delay, jitter and stalls that show what a
   worse line looks like. They are not measurements of anyone's line.
   To play a recording over and over, run castcade-replay.ps1 yourself with -Loop (how: the top of that
   file). Between two passes the watcher's table is not reset, so a light the recorded game had left on
   can stay on until the replayed game switches it.
   "Castcade Watch Recording (no smoothing).bat" does the same with the watcher's smoothing off, to compare:
   when the player's PC stalled for a moment (the recording has a gap), the smoothed watcher holds the
   picture and then plays what it missed quickly; without smoothing it jumps to where the game is.

   While the Castcade app is running, it takes a game recorded or watched with 4 or 5 for a normal game
   start: it may show a notice about the table, and it remembers VPinballX_GL64_castcade.exe as the VPX
   it starts your tables with. Close Castcade first to avoid that, until a Castcade update knows about
   these sessions.

Connection: one side waits on TCP port 47800 and the other connects to it. Either side can be the one that
waits (both launchers ask). The side that waits must answer "Allow" to the Windows Firewall prompt for
VPinballX_GL64_castcade.exe and, across the internet, forward TCP 47800 on its router to its PC. The order
of starting does not matter: a watcher retries every 2 seconds, and a watcher that joins mid-game is
brought up to date.

What you should see
-------------------
Player:  "Castcade: streaming on port 47800", then "watcher connected".
Watcher: "Castcade: connecting to ...", then "watching <table>". The watcher's flipper keys do nothing;
         exit, VR recentre and table position keys work.
Record:  "Castcade: recording to <table>.<date-time>.castrec".
Replay:  the window says where in the recording it is every 5 seconds and when the recording has ended;
         the watcher then shows "the player closed the table" and keeps the last picture.

What is not in this iteration
-----------------------------
- The ROM's sound and the external DMD window (VPinMAME runs only on the player's PC). A DMD drawn inside
  the table (the VR room / in-table DMD most VR tables have) does come across.
- B2S backglass, PuP packs, FlexDMD windows: anything that is a separate program on the player's PC.
- Taking turns inside one running game. Today a VPX is either the player or a watcher for as long as the
  table is open.

If something is wrong
---------------------
- Logs: castcade-play.log and castcade-watch.log next to the exe. One line every 5 seconds says how many
  frames arrived, how many script calls were replayed and how many failed. Recording and replay write
  castcade-record.log, castcade-watch-recording.log and castcade-replay.log.
- Ball stutters on the watcher: raise CASTCADE_STREAM_DELAY_MS in "Castcade Watch.bat" (100 by default;
  try 150 or 200 over a poor connection). "starved" in the watcher's log counts frames that had nothing new.
  A "stream gap" line in the watcher's log is a moment the PLAYER's PC stopped drawing (the player saw it
  stop too); the delay hides gaps shorter than itself. To find what stalls the player's PC, set
  CASTCADE_FRAMELOG=<a file> for the player: every frame of 25 ms or more is logged with where the time went.
- Double sound on the watcher: set CASTCADE_STREAM_SOUND=0 in "Castcade Watch.bat".
- "this is not the same table file as the player's": copy the player's exact .vpx to the watcher.

Source: https://github.com/mekelburgj20/castcade-vpx (branch castcade/state-stream), a fork of
Panasony1008/vpinball. Same licence as Visual Pinball: see LICENSE in the source.
