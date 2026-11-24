// license:GPLv3+

#pragma once

// Castcade state stream, watch role: the timeline. The frames received and not shown yet, and the clock that picks
// what the next picture shows (snapshot interpolation: a fixed delay behind the newest frame, on the player's clock).
// It knows nothing of VPX, so that it can be run alone: the session (CastcadeStream.cpp) gives it the frames and is
// called back to show a state and to replay a frame's script calls; tests/timeline_test.cpp feeds it frame times
// shaped like a log's and prints the stats lines a watcher would have written (I11.4.17, 2026-11-24: the code was
// SessionImpl::StepTimeline until then).
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

inline std::string TimelineFmt(const char* const fmt, ...)
{
   char buf[1024];
   va_list ap;
   va_start(ap, fmt);
   const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   return n < 0 ? std::string() : std::string(buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
}

template <class Frame, class Host> class WatchTimeline final
{
public:
   explicit WatchTimeline(Host& host)
      : m_host(host)
   {
   }

   // Session start: CASTCADE_STREAM_DELAY_MS and CASTCADE_STREAM_SMOOTH
   void Configure(const int64_t delayUsec, const bool smooth)
   {
      m_delayUsec = delayUsec;
      m_smooth = smooth;
   }

   // A new player's HELLO: its clock is another one
   void OnHello() { m_offsetSamples.clear(); }

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
   }

   // A frame, received now
   void Push(std::unique_ptr<Frame> f, const uint64_t now)
   {
      AddOffsetSample((int64_t)now - (int64_t)f->time);
      m_frames.push_back(std::move(f));
   }

   // Every frame received has been shown and its calls replayed
   bool Drained() const
   {
      return std::none_of(m_frames.begin(), m_frames.end(), [](const std::unique_ptr<Frame>& f) { return !f->replayed; });
   }

   // The stats line: newest stamp minus oldest, of the frames held
   double SpanMs() const { return m_frames.size() > 1 ? (double)(m_frames.back()->time - m_frames.front()->time) / 1000. : 0.; }

   void ResetInterval()
   {
      m_iGaps = 0;
      m_iMaxStep = 0.f;
      m_iStarved = m_iBuffering = m_iJumps = 0;
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
         if (!m_catchup && m_held && newest > m_shown && target - m_shown > CATCHUP_MIN_LAG_USEC)
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
   std::deque<int64_t> m_offsetSamples;
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
