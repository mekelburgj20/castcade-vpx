// license:GPLv3+

#pragma once

// Castcade frame-hitch probe. Independent of the stream session: any VPX run with CASTCADE_FRAMELOG=<file> logs
// every frame that took CASTCADE_FRAMELOG_MS (default 25) or more, with
//   - its wall time and its start and end on the NowUsec clock (the clock of the stream logs and recordings),
//   - where VPX's own frame profiler put the time: script (with the events and timers that ran), physics (the
//     physics steps, without the script they called), collect (building the render frame), submit (the GPU
//     render queue), flip (the swap / present), sleep (the frame limiter), castcade (the session's OnFrame), misc,
//     and what no section covers (the message pump, mainly),
//   - and, on x64, where the render thread actually was: a thread of the probe's own samples the render thread's
//     call stack every 2 ms once a frame has run past 60% of the threshold, and the frame's line names the modules
//     (and the nearest exported function) on the most frequent stacks.
// Every write the recorder and the stream log make is also logged ("io" lines), so that a long frame can be
// checked against them. All file writes are made by the probe's threads: on the render thread the probe costs a
// clock read and a few counters per frame, and a short copy of the profiler's data for a long frame.
//
// Player creates the probe with the player and calls OnFrameEnd just before each g_frameProfiler.NewFrame.

namespace castcade
{

class FrameProbe
{
public:
   // nullptr unless CASTCADE_FRAMELOG names a file that can be opened
   static FrameProbe* Create();

   virtual ~FrameProbe() = default; // writes the summary, stops the sampler, closes the file

   // Render thread, once per frame, just before g_frameProfiler.NewFrame: physMsec is the physics clock
   // (Player::m_time_msec), physIterations the physics steps of the last UpdatePhysics
   virtual void OnFrameEnd(unsigned int physMsec, unsigned int physIterations) = 0;
};

}
