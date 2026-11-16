// license:GPLv3+

#pragma once

// Castcade state stream: the session. One VPX plays a table ("play" role) and sends what its script and its
// physics do; any number of other VPX instances with the same table file loaded ("watch" role) run neither
// script nor physics and render that stream from their own camera.
//
// Configured by environment variables, read once when the player creates the session (Player::Init, just
// before the script starts):
//   CASTCADE_STREAM           play | watch. Absent or anything else: no session, VPX behaves as it always did.
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
// The frame-hitch probe (CASTCADE_FRAMELOG) too: CastcadeProbe.h.
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
   virtual bool IsWatcher() const = 0;

   // A watcher, or a player with CASTCADE_TEST_NOPAUSE=1, keeps running when its window loses the focus
   virtual bool NeverPauses() const = 0;

   // Player::CreateBall / Player::DestroyBall. The play role numbers the balls it streams; a watcher's balls
   // are created by its StateApplier, so it ignores these.
   virtual void OnBallCreated(Ball* ball) = 0;
   virtual void OnBallDestroyed(Ball* ball) = 0;

   // Once per rendered frame from Player::PrepareFrame. Watch: after the animation pass, applies the stream for
   // this frame. Play: after the frame's script timers, sends the frame. Both: network I/O and notifications.
   virtual void OnFrame() = 0;

   // Play role, after each physics step of Player::UpdatePhysics; targetUsec is the time (VPX usec()) the loop
   // simulates up to. When the loop is catching up after a stall of the render thread, the session sends the
   // intermediate states as frames of their own, so that a watcher can show the ball's path through the stall.
   virtual void OnPhysicsStep(unsigned long long targetUsec) = 0;

protected:
   // Player befriends Session for this one write: a watcher rebuilds the static pre-render after a keyframe
   static void MarkStaticPrepassDirty(Player* player);
};

}
