// license:GPLv3+

#pragma once

// Castcade state stream: the two ends of the stream's clock. PlayStamp is the stamp a player gives each frame.
// WatchTimeline is the watcher's side: the frames received and not shown yet, and the clock that picks what the next
// picture shows (snapshot interpolation: a fixed delay behind the newest frame, on the player's clock).
// Neither knows anything of VPX, so that both can be run alone: the session (CastcadeStream.cpp) gives the timeline
// its frames and is called back to show a state and to replay a frame's script calls; tests/timeline_test.cpp feeds
// it frame times shaped like a log's and prints the stats lines a watcher would have written (I11.4.17, 2026-11-24:
// the code was SessionImpl::StepTimeline and SessionImpl::PhysicsStamp until then).
//
// I11.4.17, what happened on 2026-11-23: a player whose VPX is paused (its window is not the active one) runs no
// physics, so its physics clock stands, and goes on sending a frame with every frame it draws. Its stamps were the
// physics clock read through two counters (NowUsec() - (usec() - physics clock)): one number for minutes, give or
// take the microsecond between the two readings, and more whenever the thread was interrupted between them. The
// timeline passes frames in the order they came and only up to the first one whose stamp is past the shown time, and
// the shown time never passes the newest stamp: one frame stamped later than those behind it held every frame after
// it, with nothing replayed and the timeline growing by 120 frames a second, until the player's game went on and its
// stamps jumped ahead by the length of the pause (35 to 40 s into the first ball of two matches). The rule that jumps
// when the timeline is over 2 s never saw it: it measures the timeline in stamps, and those stood.
// Since then: a paused player's stamp runs on with the wall clock and never goes back (PlayStamp); and whatever a
// player sends, the watcher keeps its frames in stamp order, treats a stamp that stands as a paused player, and
// starts its clock again (a "timeline resynchronised" line) when the player's clock ran back, when it runs again
// after standing, or when a frame was not shown within 2 s of the delay it was due after.
//
// Frame: uint64_t time (the player's stamp, on the player's clock), uint64_t arrived (NowUsec here), bool replayed
// (set by the host when it replays the frame).
// Host: void TimelineShow(Frame& a, Frame& b, float t)   the state a fraction t of the way from a to b (may be one frame)
//       void TimelineReplay(Frame& f)                    the frame's script calls; sets f.replayed
//       float TimelineBallStep()                         how far a ball moved between the last two states shown
//       bool TimelineConnected()                         a player is connected and its HELLO was accepted
//       void TimelineLog(const std::string& line)        a line for the session's log

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace castcade
{

constexpr uint64_t TIMELINE_MAX_USEC = 2000000;
constexpr int64_t CATCHUP_MIN_LAG_USEC = 20000; // a hold shorter than this resumes by jumping, as it always did
constexpr size_t OFFSET_WINDOW = 300;
// I11.4.17 (2026-11-24), the watcher's rules for a player's clock that does not run as the wall clock does.
// The physics clock moves in whole steps of 1000 us: a stamp less than half a step past the one before has not moved
// (the microsecond between the player's two counter readings, or a few hundred when its thread was interrupted).
constexpr int64_t STAMP_STEP_MIN_USEC = 500;
// A stamp before the last one by less than this is that stamp read late, and is put in order; by this much or more
// it is another clock (no thread is held 100 ms between two instructions without the whole frame being that late)
constexpr int64_t STAMP_BACK_MAX_USEC = 100000;
// Frames that keep coming under one stamp for this long: the player's game is paused
constexpr uint64_t CLOCK_STANDS_USEC = 1000000;
constexpr int MAX_RESYNCS_LOGGED = 20;

inline std::string TimelineFmt(const char* const fmt, ...)
{
   char buf[1024];
   va_list ap;
   va_start(ap, fmt);
   const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   return n < 0 ? std::string() : std::string(buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
}

// Play role: the stamp of a frame, on the NowUsec clock. It is the time the physics state was simulated up to: after
// a stall of the render thread that is the time before the stall, not the moment the frame is sent (the watcher
// places the frame by it). While the physics clock stands (the game is paused: Player::m_pause, no UpdatePhysics) the
// stamp runs on with the wall clock, at the distance it had when the physics clock last moved: the frames of a paused
// player carry what its frame timers still do (lamps, the DMD), a watcher shows them at the pace they happened, and
// when the game goes on (Player::m_noTimeCorrect moves the physics clock to now) the stamp is where the physics clock
// lands, with no jump. No stamp is before the one sent last. (I11.4.17, 2026-11-24: before, a paused player's stamp
// stood still and wobbled by the microsecond between the two counters, more when the thread was interrupted there.)
class PlayStamp final
{
public:
   // wall: NowUsec(). vpxNow: VPX's usec(), read just before it. physics: the physics clock (Player::m_curPhysicsFrameTime,
   // on the usec() clock). runOn false: the stamp of before 2026-11-24, for tests of a watcher against an older player
   // (CASTCADE_STREAM_PAUSECLOCK=0).
   uint64_t Stamp(const uint64_t wall, const uint64_t vpxNow, const uint64_t physics, const bool runOn = true)
   {
      const int64_t behind = (int64_t)vpxNow - (int64_t)physics;
      if (!m_have || physics != m_physics)
      {
         m_physics = physics;
         m_behind = behind;
         m_movedAt = wall;
      }
      uint64_t stamp = (uint64_t)((int64_t)wall - (runOn ? m_behind : behind));
      if (runOn && m_have && stamp < m_last)
         stamp = m_last;
      m_last = stamp;
      m_have = true;
      return stamp;
   }

   // How long the physics clock has stood, as of the last stamp (a few ms at most while the game runs)
   uint64_t StoodUsec(const uint64_t wall) const { return m_have && wall > m_movedAt ? wall - m_movedAt : 0; }
   // Match role: the game comes back from a watch phase, which is not a pause to report
   void Touch(const uint64_t wall) { m_movedAt = wall; }

private:
   bool m_have = false;
   uint64_t m_physics = 0; // the physics clock at the last stamp
   int64_t m_behind = 0;   // usec() minus the physics clock when the physics clock last moved
   uint64_t m_movedAt = 0; // NowUsec then
   uint64_t m_last = 0;    // the last stamp given
};

template <class Frame, class Host> class WatchTimeline final
{
public:
   explicit WatchTimeline(Host& host)
      : m_host(host)
   {
   }

   // Session start: CASTCADE_STREAM_DELAY_MS, CASTCADE_STREAM_SMOOTH and CASTCADE_STREAM_RESYNC (0: the rules of before
   // 2026-11-24, I11.4.17, for tests: frames as stamped, no resynchronisation)
   void Configure(const int64_t delayUsec, const bool smooth, const bool resync = true)
   {
      m_delayUsec = delayUsec;
      m_smooth = smooth;
      m_resync = resync;
   }

   // A new player's HELLO: its clock is another one
   void OnHello()
   {
      m_offsetSamples.clear();
      m_haveStamp = m_clockStands = false;
   }

   // A frame or a keyframe was received
   void OnArrival(const uint64_t now)
   {
      if (m_held && m_lastAnyArrival)
         m_holdArrivalGapMax = (std::max)(m_holdArrivalGapMax, now - m_lastAnyArrival);
      m_lastAnyArrival = now;
   }

   // A keyframe, stamped like a frame: what was held belongs to the state before it
   void OnKeyframe(const uint64_t stamp, const uint64_t now)
   {
      AddOffsetSample((int64_t)now - (int64_t)stamp);
      m_frames.clear();
      m_haveShown = m_held = m_catchup = m_epOpen = false;
      // The frames that follow are measured against it
      m_haveStamp = true;
      m_lastStamp = m_standStamp = stamp;
      m_standSince = now;
      m_standFrames = 0;
      m_clockStands = false;
   }

   // A frame, received now. The clock offset is measured with the stamp as the player sent it; the timeline holds
   // the frame under a stamp that is never before the one that came before it.
   void Push(std::unique_ptr<Frame> f, const uint64_t now)
   {
      const uint64_t stamp = f->time;
      if (m_resync && m_haveStamp)
      {
         const int64_t back = (int64_t)m_lastStamp - (int64_t)stamp;
         const int64_t moved = (int64_t)stamp - (int64_t)m_standStamp;
         if (back >= STAMP_BACK_MAX_USEC)
            // Another time base, earlier than the one the held frames are stamped with: they would hold every frame
            // from here on until the new clock reached their stamps
            Resync(now, TimelineFmt("the player's clock ran back by %.1f ms", (double)back / 1000.));
         else
         {
            if (back > 0)
            {
               // This stamp read late, or the one before it: in the order they came, which is the order the player
               // made them in (one TCP connection)
               f->time = m_lastStamp;
               ++m_iOrdered;
               ++m_orderedTotal;
               m_iBackMax = (std::max)(m_iBackMax, back);
            }
            // A clock that stands goes on by the length of the pause (a second at least) or, if it ever just carried
            // on, by more every frame: a single stamp ahead by less than what is put in order is one read late
            if (moved >= (m_clockStands ? STAMP_BACK_MAX_USEC : STAMP_STEP_MIN_USEC))
            {
               if (m_clockStands)
                  // The game goes on: its stamps are a pause ahead of where they stood. What is held is the paused
                  // table, and the clock offset measured while the stamps stood is of no use any more
                  Resync(now, TimelineFmt("the player's clock runs again after standing for %.1f s, %.1f s ahead of where it stood",
                                 (double)(now - m_standSince) / 1e6, (double)moved / 1e6));
               m_standStamp = stamp;
               m_standSince = now;
               m_standFrames = 0;
            }
            else
            {
               ++m_standFrames;
               if (!m_clockStands && now - m_standSince >= CLOCK_STANDS_USEC)
               {
                  m_clockStands = true;
                  ++m_standsTotal;
                  if (m_standsTotal <= MAX_RESYNCS_LOGGED)
                     m_host.TimelineLog(TimelineFmt("the player's clock stands still: the %llu frames of the last %.1f s carry one stamp (its game is paused); "
                                                    "they are shown as they arrive",
                        (unsigned long long)m_standFrames, (double)(now - m_standSince) / 1e6));
               }
            }
         }
      }
      if (!m_haveStamp) // the first frame of a clock: after a resynchronisation, or (m_resync off) never measured
      {
         m_standStamp = stamp;
         m_standSince = now;
         m_standFrames = 0;
      }
      AddOffsetSample((int64_t)now - (int64_t)stamp);
      m_lastStamp = f->time;
      m_haveStamp = true;
      m_frames.push_back(std::move(f));
   }

   // Every frame received has been shown and its calls replayed
   bool Drained() const
   {
      return std::none_of(m_frames.begin(), m_frames.end(), [](const std::unique_ptr<Frame>& f) { return !f->replayed; });
   }

   // The stats line: newest stamp minus oldest, of the frames held. Signed: below zero when a frame is held behind
   // one with a later stamp (CASTCADE_STREAM_RESYNC=0 only; until 2026-11-24 that printed as 18446744073709552 ms)
   double SpanMs() const { return m_frames.size() > 1 ? (double)((int64_t)m_frames.back()->time - (int64_t)m_frames.front()->time) / 1000. : 0.; }

   void ResetInterval()
   {
      m_iGaps = 0;
      m_iMaxStep = 0.f;
      m_iStarved = m_iBuffering = m_iJumps = 0;
      m_iOrdered = m_iResyncs = 0;
      m_iBackMax = 0;
   }

   // I11.4.17 (2026-11-24). The stamps of what is held are of no use for showing it: all of it is shown now (the
   // newest state, and every call in the order the player made them), and the clock starts again with the next frame,
   // as after a keyframe: the offset is measured anew and the picture is the delay behind once more.
   void Resync(const uint64_t now, const std::string& why)
   {
      const size_t held = m_frames.size();
      size_t waiting = 0;
      uint64_t oldest = now;
      if (!m_frames.empty())
      {
         Frame& last = *m_frames.back();
         m_host.TimelineShow(last, last, 0.f);
         for (const std::unique_ptr<Frame>& f : m_frames)
            if (!f->replayed)
            {
               ++waiting;
               oldest = (std::min)(oldest, f->arrived);
               m_host.TimelineReplay(*f);
            }
         m_frames.clear();
      }
      if (m_epOpen)
         LogGap();
      m_offsetSamples.clear();
      m_haveShown = m_held = m_catchup = false;
      m_haveStamp = m_clockStands = false;
      ++m_iResyncs;
      ++m_resyncsTotal;
      if (m_resyncsTotal <= MAX_RESYNCS_LOGGED)
         m_host.TimelineLog(TimelineFmt("timeline resynchronised: %s; %zu frames held, %zu of them not shown yet%s, are shown now and the clock offset is measured again%s",
            why.c_str(), held, waiting, waiting ? TimelineFmt(" (the oldest received %.0f ms ago)", (double)(now - oldest) / 1000.).c_str() : "",
            m_resyncsTotal == MAX_RESYNCS_LOGGED ? " (further ones are only counted)" : ""));
   }

   // When the player stalls, its frames stop for the length of the stall and then come all at once (a catch-up sends
   // the states it simulated meanwhile, CastcadeStream OnPhysicsStep). The watcher shows a fixed delay behind the
   // newest frame, so it holds the last picture for the stall minus the delay whatever it does (a guess forward would be
   // wrong at the first thing the ball hits). What smoothing changes is the moment frames come back: without it the
   // shown time jumps to the target and the ball leaps over everything it did during the hold; with it the shown time
   // carries on from where it held and catches up with the target over 80 to 300 ms (half the lag), eased at both
   // ends, so the missed motion is played quickly instead of skipped. With no hold of more than CATCHUP_MIN_LAG_USEC the
   // shown time is the target, exactly as without smoothing; calls are replayed once each and in order either way.
   void Step(const uint64_t now)
   {
      if (m_frames.empty())
         return;
      // I11.4.17 (2026-11-24). A frame is due the delay after it arrived, give or take what the connection does. One
      // that is still not shown 2 s past that waits for a time the shown time is not going to reach in any way this
      // code knows of (the rule below measures the timeline in stamps, and is blind when the stamps are what is
      // wrong): never wait on, whatever the cause.
      if (m_resync)
         for (const std::unique_ptr<Frame>& f : m_frames)
            if (!f->replayed)
            {
               if (now > f->arrived && now - f->arrived > (uint64_t)m_delayUsec + TIMELINE_MAX_USEC)
               {
                  Resync(now, TimelineFmt("a frame received %.1f s ago was not due yet (its stamp is %.1f s ahead of the time shown)", (double)(now - f->arrived) / 1e6,
                                 m_haveShown ? (double)((int64_t)f->time - m_shown) / 1e6 : 0.));
                  return;
               }
               break; // frames are replayed in the order they came: the first one waiting is the oldest
            }
      const int64_t target = (int64_t)now - m_offset - m_delayUsec;
      int64_t render = target;

      // A stalled watcher (or a stale clock offset) lets the timeline grow: jump to the newest minus the delay
      const int64_t newest = (int64_t)m_frames.back()->time;
      bool jumped = false;
      if (newest - (int64_t)m_frames.front()->time > (int64_t)TIMELINE_MAX_USEC && newest - m_delayUsec > render)
      {
         render = newest - m_delayUsec;
         jumped = true;
         m_catchup = false;
         if (m_iJumps++ < 3)
            m_host.TimelineLog(TimelineFmt("timeline over 2 s (%zu frames): jumping to the newest frame minus the delay", m_frames.size()));
      }

      if (m_smooth && !jumped && m_haveShown)
      {
         // Frames have come back when the newest stamp has moved on: by a physics step at least. A stamp that stands
         // and wobbles is a paused player (I11.4.17, 2026-11-24: its every wobble upwards began a catch-up, which showed
         // the stamp before it for one more frame and logged a "stream gap" that was none)
         if (!m_catchup && m_held && newest - m_shown >= (m_resync ? STAMP_STEP_MIN_USEC : 1) && target - m_shown > CATCHUP_MIN_LAG_USEC)
         {
            m_catchup = true;
            m_catchupStart = now;
            m_catchupLag = (double)(target - m_shown);
            // Half the lag: the shown time runs at up to 4 times the speed of play (smoothstep peaks at 1.5 times the
            // mean); a third of the lag (6 times) gave single-frame ball steps of up to 2.5 times normal play's largest
            m_catchupLen = std::clamp(0.5 * m_catchupLag, 80000., 300000.);
            if (m_epOpen) // a hold too short to count as a gap of its own, inside one being measured
            {
               m_epUntil = (std::max)(m_epUntil, now + (uint64_t)m_catchupLen + 100000);
               m_epCatchupMs = (std::max)(m_epCatchupMs, m_catchupLen / 1000.);
            }
         }
         if (m_catchup)
         {
            const double u = (double)(now - m_catchupStart) / m_catchupLen;
            if (u >= 1.)
               m_catchup = false;
            else
               render = target - (int64_t)(m_catchupLag * (1. - u * u * (3. - 2. * u)));
         }
      }

      // Nothing newer than the newest frame to show: hold it (also what happened before, with render past it)
      const bool held = render > newest;
      if (held)
      {
         render = newest;
         m_catchup = false; // a new hold during a catch-up: it starts again from here when frames come back
      }
      if (held && !m_held)
      {
         m_heldSince = now;
         m_heldStamp = newest;
         m_holdArrivalGapMax = 0;
      }
      else if (!held && m_held && m_haveShown)
      {
         const double heldMs = (double)(now - m_heldSince) / 1000.;
         // A gap is frames that stopped arriving: the longest time without one while the picture held (OnArrival
         // measures it). A picture held while frames keep coming is a paused player, whose stamps stand still: nothing
         // to measure there
         const double arrivalGapMs = (double)m_holdArrivalGapMax / 1000.;
         if (heldMs >= 20. && arrivalGapMs >= 20.)
         {
            if (m_epOpen)
               LogGap();
            m_epOpen = true;
            m_epHeldSince = m_heldSince;
            m_epHeldMs = heldMs;
            m_epGapMs = arrivalGapMs;
            m_epLagMs = (double)(target - m_heldStamp) / 1000.;
            m_epCatchupMs = m_catchup ? m_catchupLen / 1000. : 0.;
            m_epUntil = now + (uint64_t)(m_epCatchupMs * 1000.) + 100000;
            m_epMaxStep = 0.f;
            m_epFirstMoveMs = -1.;
         }
      }
      m_held = held;
      m_shown = render;
      m_haveShown = true;

      // a: the newest frame at or before the render time; b: the one after it
      size_t ia = SIZE_MAX;
      for (size_t i = 0; i < m_frames.size(); ++i)
      {
         if ((int64_t)m_frames[i]->time <= render)
            ia = i;
         else
            break;
      }
      if (ia == SIZE_MAX)
      {
         ++m_iBuffering; // the first frames after a keyframe are still younger than the delay
         return;
      }
      Frame& a = *m_frames[ia];
      Frame* const b = ia + 1 < m_frames.size() ? m_frames[ia + 1].get() : nullptr;
      if (b && b->time > a.time)
      {
         const float t = std::clamp((float)((double)(render - (int64_t)a.time) / (double)(b->time - a.time)), 0.f, 1.f);
         m_host.TimelineShow(a, *b, t);
      }
      else
      {
         m_host.TimelineShow(a, a, 0.f);
         if (!b && m_host.TimelineConnected())
            ++m_iStarved;
      }
      // How far the picture moved in this frame: the gap being measured, or the reference of normal play
      const float step = m_host.TimelineBallStep();
      m_iMaxStep = (std::max)(m_iMaxStep, step);
      if (m_epOpen)
      {
         m_epMaxStep = (std::max)(m_epMaxStep, step);
         if (m_epFirstMoveMs < 0. && step >= 1.f) // the first frame after the hold in which a ball visibly moves
            m_epFirstMoveMs = (double)(now - m_epHeldSince) / 1000.;
         if (now >= m_epUntil)
            LogGap();
      }
      else if (!held)
      {
         if (m_stepHist.empty())
            m_stepHist.assign(8001, 0);
         ++m_stepHist[std::min<size_t>((size_t)(step * 2.f), m_stepHist.size() - 1)];
         ++m_stepFrames;
         m_stepMax = (std::max)(m_stepMax, step);
      }
      // Calls after the state, in arrival order: a ball put finds its ball, lights change in step with the balls
      for (size_t i = 0; i <= ia; ++i)
         if (!m_frames[i]->replayed)
            m_host.TimelineReplay(*m_frames[i]);
      for (size_t i = 0; i < ia; ++i)
         m_frames.pop_front();
   }

   void LogGap()
   {
      m_epOpen = false;
      ++m_iGaps;
      ++m_gapsTotal;
      const std::string how = m_epCatchupMs > 0. ? TimelineFmt("caught up in %.0f ms", m_epCatchupMs)
                            : m_smooth           ? std::string("no catch-up (lag under 20 ms)")
                                                 : std::string("jumped (smoothing off)");
      const std::string move = m_epFirstMoveMs >= 0. ? TimelineFmt("a ball moved again %.0f ms after the hold began", m_epFirstMoveMs) : std::string("no ball moved");
      m_host.TimelineLog(TimelineFmt("stream gap: frames stopped arriving for %.0f ms, picture held %.0f ms, %.0f ms behind when frames came back, %s; %s; largest ball step %.1f VPU in one frame",
         m_epGapMs, m_epHeldMs, m_epLagMs, how.c_str(), move.c_str(), m_epMaxStep));
   }

   std::string StepSummary() const
   {
      if (m_stepFrames == 0)
         return "no ball moved";
      const auto pct = [this](const double p)
      {
         const uint64_t want = (uint64_t)(p * (double)m_stepFrames);
         uint64_t sum = 0;
         for (size_t i = 0; i < m_stepHist.size(); ++i)
         {
            sum += m_stepHist[i];
            if (sum > want)
               return (double)i * 0.5;
         }
         return (double)(m_stepHist.size() - 1) * 0.5;
      };
      return TimelineFmt("frames %llu, median %.1f, p99 %.1f, p99.9 %.1f, max %.1f VPU", (unsigned long long)m_stepFrames, pct(0.5), pct(0.99), pct(0.999), m_stepMax);
   }

   // What the session's stats line, its match role and the test read
   std::deque<std::unique_ptr<Frame>> m_frames;
   int64_t m_offset = 0;              // NowUsec here minus the player's stamp: the smallest of the last OFFSET_WINDOW frames
   uint64_t m_lastAnyArrival = 0;     // when the last frame was received (any stamp)
   bool m_epOpen = false;             // a gap is being measured (LogGap closes it)
   uint64_t m_iStarved = 0, m_iBuffering = 0, m_iJumps = 0; // per stats interval
   uint64_t m_iGaps = 0, m_gapsTotal = 0;
   float m_iMaxStep = 0.f;
   // I11.4.17: stamps that came before the one ahead of them and were put in order (and the furthest back of them, us);
   // times the clock was started again; times the player's clock stood for CLOCK_STANDS_USEC
   uint64_t m_iOrdered = 0, m_orderedTotal = 0, m_iResyncs = 0, m_resyncsTotal = 0, m_standsTotal = 0;
   int64_t m_iBackMax = 0;
   bool m_clockStands = false;        // the player's stamps have stood for CLOCK_STANDS_USEC and still do

private:
   void AddOffsetSample(const int64_t sample)
   {
      m_offsetSamples.push_back(sample);
      if (m_offsetSamples.size() > OFFSET_WINDOW)
         m_offsetSamples.pop_front();
      m_offset = *std::min_element(m_offsetSamples.begin(), m_offsetSamples.end());
   }

   Host& m_host;
   int64_t m_delayUsec = 50000;
   // What happens when the stream had a gap (Step). CASTCADE_STREAM_SMOOTH=0: the shown time jumps to the target when
   // frames come back, as before; 1 (default): it catches up over a short eased interval.
   bool m_smooth = true;
   bool m_resync = true;
   std::deque<int64_t> m_offsetSamples;
   // The player's clock as the frames show it (Push)
   bool m_haveStamp = false;   // a frame or keyframe of this clock has been seen
   uint64_t m_lastStamp = 0;   // the stamp the last frame is held under
   uint64_t m_standStamp = 0;  // the last stamp that had moved on from the one before, as the player sent it
   uint64_t m_standSince = 0;  // when it arrived
   uint64_t m_standFrames = 0; // frames since, none of which has moved on from it
   bool m_haveShown = false, m_held = false, m_catchup = false;
   int64_t m_shown = 0;         // the stream time the last frame showed
   uint64_t m_catchupStart = 0; // NowUsec
   double m_catchupLag = 0., m_catchupLen = 0.;
   // The gap being measured: how long the picture held, and the largest ball step until the catch-up is over
   uint64_t m_heldSince = 0, m_epHeldSince = 0;
   uint64_t m_holdArrivalGapMax = 0; // the longest time without a frame received while the picture held
   int64_t m_heldStamp = 0;
   uint64_t m_epUntil = 0;
   double m_epHeldMs = 0., m_epGapMs = 0., m_epLagMs = 0., m_epCatchupMs = 0., m_epFirstMoveMs = -1.;
   float m_epMaxStep = 0.f;
   std::vector<uint32_t> m_stepHist; // ball steps of the frames outside a gap, 0.5 VPU bins
   uint64_t m_stepFrames = 0;
   float m_stepMax = 0.f;
};

}
