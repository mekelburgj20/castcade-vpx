// license:GPLv3+

#pragma once

// Castcade test tooling, independent of the stream session, so that a VPX with no session at all (the frame
// probe's baseline runs) can be driven the same way as a player:
//   CASTCADE_TEST_NOPAUSE=1     never pause when the window loses the focus (a watcher never does anyway)
//   CASTCADE_TEST_INPUT=<file>  scripted key presses, fired inside VPX through the same queue a real key takes.
//                               One event per line, '#' starts a comment:  <ms> <down|up> <key>
//                               <ms> counts from the first frame (wall clock); <key> is start, credit, plunger,
//                               lflip, rflip (the player's own bindings) or a decimal DirectInput scan code.
//   CASTCADE_TEST_STALL=<ms>,<every s>[,<first s>]  stall the render thread for <ms> every <every s> seconds, in
//                               the frame after the physics (where a script waiting on a file stalls it): gives the
//                               stream's catch-up frames and the watcher's smoothing gaps to work on
//   CASTCADE_TEST_PAUSE=<at s>,<for s>[,<every s>]  pause the player as losing the window's focus does, and resume
//                               it as getting the focus back does (the physics clock moves over the pause)
// The watcher's own switch (CASTCADE_TEST_DROP) and the one at a session's end (CASTCADE_TEST_EXITWARN) are in
// CastcadeStream.cpp.
// Its log lines ("test input ...") go to CASTCADE_STREAM_LOG when that is set, and to the VPX log.
// Never created on a watcher.

class Player;

namespace castcade
{

class TestHarness
{
public:
   // nullptr unless one of the variables above is set
   static TestHarness* Create(Player* player);

   virtual ~TestHarness() = default;

   virtual bool NeverPauses() const = 0;

   // Once per rendered frame, from Player::PrepareFrame after the frame's script timers
   virtual void OnFrame() = 0;
};

}
