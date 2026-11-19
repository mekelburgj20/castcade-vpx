// license:GPLv3+

#pragma once

// Castcade state stream: the session. One VPX plays a table ("play" role) and sends what its script and its
// physics do; any number of other VPX instances with the same table file loaded ("watch" role) run neither
// script nor physics and render that stream from their own camera. A VPX in the "match" role plays its own game
// and, when told to, shows another player's table on its own for a while, then gets its own game back as it was.
//
// Configured by environment variables, read once when the player creates the session (Player::Init, just
// before the script starts):
//   CASTCADE_STREAM           play | watch | match. Absent or anything else: no session, VPX behaves as it always did.
//                             match: a play session (every play variable applies, a plain watcher or another match
//                             instance can watch it) that a companion sends to watch and calls back over the
//                             control connection below. While it watches, it is a watcher of the stream it was
//                             given (the watch variables apply: delay, sound, smoothing), its own game is paused as
//                             VPX pauses a game (the table's Paused and UnPaused events fire, so a ROM table pauses
//                             VPinMAME; no physics, no timers, no key reaches the script) while frames go on being
//                             drawn, and its own watchers see its table stand still. A watcher that connects to it
//                             while it is not playing is accepted and held, with nothing sent, until it plays again
//                             (10 s at most, then closed). It shows no notifications.
//   CASTCADE_STREAM_CONTROL   match: connect:127.0.0.1:<port>, retried every 2 s. This PC only: the address must be
//                             127.x.y.z or the word localhost, as text; nothing is looked up. Text lines, UTF-8,
//                             each ended by \n, 2048 bytes at most (a longer one closes the connection). Without a
//                             connection the table plays.
//                               this VPX sends:
//                                 hello 1 <build> <editables> <table hash> <stream port>   once per connection; 1 is
//                                                           this protocol's version; build has no spaces; editables
//                                                           is decimal, the hash 16 hex digits: the identity the
//                                                           stream's HELLO checks; the port is the one this table's
//                                                           stream listener is bound to (0: it has none)
//                                 state playing             own table shown and live; also right after hello, and the
//                                                           answer to every command that leaves it playing
//                                 state watching            the other player's table is on screen (its keyframe was
//                                                           drawn); sent again for the same stream if asked again
//                                 state failed <reason>     a watch did not work or ended by itself; this VPX is back
//                                                           on its own table without being told, and state playing
//                                                           follows. Reasons: refused (another table file), timeout
//                                                           (no stream within 4 s of the command), stalled (a stream
//                                                           that was shown delivered no frame for 3 s), lost (it
//                                                           ended and did not come back within 3 s), closed (the
//                                                           watched player closed its table), badaddress (no
//                                                           <127.x.y.z or localhost>:<port> in the command),
//                                                           overflow (the stream put more properties than this
//                                                           table has: not this table's stream)
//                               this VPX reads:
//                                 watch <host>:<port>       watch the stream at that address (it connects there; the
//                                                           host must be this PC, as for the control connection). A
//                                                           watch while watching re-aims: the stream is dropped and
//                                                           the connection made again, also to the same address,
//                                                           with no state playing in between. Every watch is
//                                                           answered within 5 s: state watching or state failed
//                                 play                      come back to the own table; answered state playing
//                               hello and the first state playing are sent as soon as the connection exists. Every
//                               token is printable ASCII without spaces; no line is longer than 200 bytes.
//                               Of several commands read in one go only the last is acted on and answered (the others
//                               name states that were superseded before this table could reach them). Any other
//                               line, an empty one too, is ignored: no answer, and a watch goes on (a newer
//                               companion may say more than this build knows).
//                             A control connection that ends brings the own table back.
//                             From the watch command until the other player's keyframe the own table is paused as
//                             VPX pauses one (nothing moves, the clock stands, no event is due); a watch that ends
//                             before its keyframe was a pause of that length and nothing else. Keys are read all
//                             the same, every frame from the watch command on, and kept from the script as they are
//                             while the other table is shown: at the return it hears only what changed (a key that
//                             was down before and has been let go: its key-up; a key pressed and let go meanwhile:
//                             nothing; a key that is down now and was not: its key-down).
//   CASTCADE_STREAM_STATICREFRESH  match: 1 = rebuild the desktop static pre-render after a keyframe and after the
//                             return, as a plain watcher does after a keyframe (two frames of well over 100 ms).
//                             Default 0: a match table's pre-render was made after its own script's start, as the
//                             watched player's was.
//   CASTCADE_STREAM_ADDR      Either role may listen or connect, so whichever side can accept connections does:
//                               listen:<port> or <port>          listen on every interface (what a real player uses)
//                               listen:<ipv4>:<port>             listen on that interface only (tests: 127.0.0.1,
//                                                                which raises no Windows firewall prompt)
//                               connect:<host>:<port> or <host>:<port>   connect, retrying every 2 s
//                             Play default: listen on every interface, port 47800 (none when recording, below).
//                             Watch has no default.
//   CASTCADE_STREAM_RECORD    play: also write the stream to a file, exactly as a watcher connected from the
//                             first frame would receive it, each message stamped with its time (format in
//                             CastcadeWire.h; tools/castcade/launchers/castcade-replay.ps1 plays it to a
//                             watcher). A folder (one that exists, a path ending in \, or a last part with no
//                             extension) gets <table>.<yyyyMMdd-HHmmss>.castrec and is created if needed.
//                             Without CASTCADE_STREAM_ADDR a recording player opens no socket at all.
//   CASTCADE_STREAM_DELAY_MS  watch: how far behind the newest received frame to render (default 50)
//   CASTCADE_STREAM_SOUND     watch: 0 = do not replay sound calls (default 1)
//   CASTCADE_STREAM_LOG       a file to append timestamped log lines to (they also go to the VPX log)
//
// Test switches (CASTCADE_TEST_NOPAUSE, CASTCADE_TEST_INPUT) work with or without a session: CastcadeTest.h.
// The frame-hitch probe (CASTCADE_FRAMELOG) too: CastcadeProbe.h. Match role only: CASTCADE_TEST_MATCHCHECK=1 reads
// every property of every part before a watch phase and after it and logs each that differs (thousands of calls at
// each turn: a test's check), and compares the parts' internal state the same way (CaptureDeepState: a lamp's stored
// state beside its play state, sequencer locks, animation phases, which hit objects collide, the timers, the game
// clock); CASTCADE_TEST_BALLLOG=<ms> logs the local balls every <ms> while the table plays;
// CASTCADE_TEST_MATCHNOUNDO=1 leaves the properties and the runtime state of a watch phase in place on purpose, to
// show that the check and the pictures see a table that did not come back; CASTCADE_TEST_UNDOLIMIT=<n> lowers the
// number of properties a stream may put, so that a real stream passes it (state failed overflow);
// CASTCADE_TEST_MUSIC=<file> calls the table's PlayMusic with that file 5 s into the session;
// CASTCADE_TEST_OPTIONEVENT=1 fires the tweak menu's "table option changed" event once in every watch phase, after
// the keyframe; CASTCADE_TEST_HOLDKEYS=1 puts key events of F13 and F14 into the key queue around every watch phase
// (one held while playing and let go after the watch command, one tapped every frame until the keyframe and three
// times after it) and logs what the script was told of them.
//
// The engine hooks (src/core/player.cpp) each call one of the members below; with no session every hook is a
// single null test, and the script call hook (CastcadeDispatch.h) a single bool test.

class Player;
class Ball;

namespace castcade
{

class Session
{
public:
   // Reads the configuration. Returns nullptr when CASTCADE_STREAM does not ask for a session. Called before
   // the table script starts: a play session records from the script's first call on.
   static Session* Create(Player* player);

   virtual ~Session() = default;

   // A watcher never starts the table script and runs no physics: Player checks this at each of those sites.
   // A match session answers true while it shows another player's table (never when the script is started).
   virtual bool IsWatcher() const = 0;

   // A watcher, or a player with CASTCADE_TEST_NOPAUSE=1, keeps running when its window loses the focus
   virtual bool NeverPauses() const = 0;

   // Player::RecomputePauseState, last word: the player stays paused whatever the focus says. A match session
   // answers true from a "watch" command until the other player's keyframe is here: its own table stands still, as
   // under VPX's own pause, and a watch that never shows anything was a pause and nothing else.
   virtual bool HoldsPause() const { return false; }

   // Player::CreateBall / Player::DestroyBall. The play role numbers the balls it streams; a watcher's balls
   // are created by its StateApplier, so it ignores these.
   virtual void OnBallCreated(Ball* ball) = 0;
   virtual void OnBallDestroyed(Ball* ball) = 0;

   // Once per rendered frame from Player::PrepareFrame. Watch: after the animation pass, applies the stream for
   // this frame. Play: after the frame's script timers, sends the frame. Both: network I/O and notifications.
   // (A match session that goes back to playing in the watcher's call is called again in the player's, in the same
   // frame: the frame timers in between already run on its own table.)
   virtual void OnFrame() = 0;

   // Play role, after each physics step of Player::UpdatePhysics; targetUsec is the time (VPX usec()) the loop
   // simulates up to. When the loop is catching up after a stall of the render thread, the session sends the
   // intermediate states as frames of their own, so that a watcher can show the ball's path through the stall.
   virtual void OnPhysicsStep(unsigned long long targetUsec) = 0;

   // PinInput::FireKeyEvent, before the table's KeyDown / KeyUp event reaches the script. True: the session keeps the
   // event from the script (match role, while another player's table is shown; the script hears of a key that
   // changed meanwhile when its own table is back).
   virtual bool OnKeyEvent(int dispid, int keycode) { return false; }

protected:
   // Player befriends Session for this one write: a watcher rebuilds the static pre-render after a keyframe
   static void MarkStaticPrepassDirty(Player* player);
};

}
