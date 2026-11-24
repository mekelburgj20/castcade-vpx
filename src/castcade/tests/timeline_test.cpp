// license:GPLv3+

// Standalone test of the watcher's timeline (CastcadeTimeline.h), with no VPX and no socket: a player, a connection
// and a watcher on one simulated clock. The player stamps its frames as SessionImpl::PhysicsStamp does (the physics
// clock read through two counters), the connection delays them, and the watcher takes what has arrived once per
// frame of its own and steps the timeline, as SessionImpl::WatchFrame does. Every 5 s of simulated time it prints
// the part of a watcher's stats line that the timeline decides, so that a run can be laid beside a tester's log.
//
// I11.4.17 (2026-11-24): in two matches of 2026-11-23 a watcher showed a still table for 35 to 40 s. Its log had
// frames arriving at 118 a second, `replayed calls 0` and a growing timeline in every interval, a span printed as
// 18446744073709552 ms and a clock offset that grew by 5 s in every 5 s: the player's stamps stood still, because
// the player's game was paused (its window had lost the focus) while its frames went on. Scenario `pause` is that
// night; the others are what must not change (a steady stream, a gap in the connection) and what else a clock can do
// (jump back, stand exactly still).

#include "../CastcadeTimeline.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace castcade;

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond) \
   do \
   { \
      ++g_checks; \
      if (!(cond)) \
      { \
         ++g_fails; \
         printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      } \
   } while (0)

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t Rnd()
{
   g_rng ^= g_rng << 13;
   g_rng ^= g_rng >> 7;
   g_rng ^= g_rng << 17;
   return (uint32_t)(g_rng >> 24);
}
static double Rnd01() { return (double)(Rnd() & 0xFFFFFF) / (double)0x1000000; }

// ---------------------------------------------------------------------------------------------------------

struct TFrame
{
   uint64_t time = 0;
   uint64_t arrived = 0;
   bool replayed = false;
   uint32_t seq = 0;
   uint32_t calls = 0;
   double ball = 0.; // one ball's position along its path, VP units
   uint64_t due = 0; // simulation: when the connection delivers it
};

// The player's clocks, in 100 ns ticks as QueryPerformanceCounter counts them on the PCs of that night. NowUsec() counts
// from the counter's zero and VPX's usec() from wintimer_init(), so the two differ by a constant that is not a whole
// number of microseconds.
struct PlayerClock
{
   uint64_t usecOriginTicks = 31415926537ull; // when the player's VPX started, on the counter

   static uint64_t NowUsec(const uint64_t ticks) { return ticks / 10; }
   uint64_t Usec(const uint64_t ticks) const { return (ticks - usecOriginTicks) / 10; }
};

enum class Event
{
   Pause,    // the player's VPX is paused (Player::m_pause): no UpdatePhysics, the physics clock stands, frames go on
   Resume,   // and resumed: Player::m_noTimeCorrect moves the physics clock to now
   Freeze,   // match role, SessionImpl::Freeze: one stamp, held as a number
   Unfreeze, // and the game comes back, resumed as from a pause
   JumpBack, // the player's clock is set back by `value` seconds (nothing in the fork does this: a clock that is wrong)
   Rate,     // the player draws `value` frames a second from here
   NetStop,  // the connection delivers nothing ...
   NetGo,    // ... until here, then everything at once
};

struct Timed
{
   double at; // seconds after the watcher's keyframe; before 0: the player's history
   Event what;
   double value;
};

struct Scenario
{
   const char* name;
   const char* what;
   double seconds;
   double playerFps;
   double blipEvery; // one frame in this many is stamped late: the thread was interrupted between the two counters (0: never)
   std::vector<Timed> events;
};

struct Totals
{
   uint64_t received = 0, replayedFrames = 0, replayedCalls = 0;
   uint64_t emptyIntervals = 0; // stats intervals with frames received and no call replayed
   uint64_t maxTimeline = 0;
   double maxWaitMs = 0.; // the longest a frame waited between its arrival and its calls being replayed
   uint64_t gaps = 0, jumps = 0, buffering = 0, starved = 0;
   double sumWaitMs = 0.;
   uint64_t logLines = 0;
   bool negativeSpan = false;
};

class World
{
public:
   explicit World(const Scenario& sc, const bool quiet)
      : m_sc(sc)
      , m_quiet(quiet)
   {
      m_tl.Configure(100000, true); // a match's watcher: CASTCADE_STREAM_DELAY_MS=100, smoothing on
   }

   Totals Run();

   // What the timeline asks of the session
   void TimelineShow(TFrame& a, TFrame& b, const float t)
   {
      const double pos = a.ball + (b.ball - a.ball) * (double)t;
      m_ballStep = (float)std::fabs(pos - m_ballShown);
      m_ballShown = pos;
   }
   void TimelineReplay(TFrame& f)
   {
      CHECK(!f.replayed);
      CHECK(f.seq == m_lastReplayedSeq + 1); // every frame's calls, once, in the order the player made them
      m_lastReplayedSeq = f.seq;
      f.replayed = true;
      ++m_tot.replayedFrames;
      m_tot.replayedCalls += f.calls;
      m_iReplayedCalls += f.calls;
      const double waitMs = (double)(m_now - f.arrived) / 1000.;
      m_tot.maxWaitMs = (std::max)(m_tot.maxWaitMs, waitMs);
      m_iMaxWaitMs = (std::max)(m_iMaxWaitMs, waitMs);
      m_tot.sumWaitMs += waitMs;
   }
   float TimelineBallStep() const { return m_ballStep; }
   bool TimelineConnected() const { return true; }
   void TimelineLog(const std::string& line)
   {
      ++m_tot.logLines;
      if (!m_quiet && (m_tot.logLines <= 12 || line.find("stream gap") == std::string::npos))
         printf("  %7.3f s  %s\n", Seconds(), line.c_str());
      else if (!m_quiet && m_tot.logLines == 13)
         printf("  %7.3f s  (further `stream gap` lines are counted, not printed)\n", Seconds());
   }

private:
   double Seconds() const { return (double)((int64_t)m_now - (int64_t)m_watchEpoch) / 1e6; }
   uint64_t Stamp(const uint64_t ticks);
   void Stats(const double secs);

   const Scenario& m_sc;
   const bool m_quiet;
   WatchTimeline<TFrame, World> m_tl { *this };
   Totals m_tot;

   // The watcher's NowUsec at its keyframe. The player's counter reads 279183.4 s less at the same moment, as the two
   // PCs' did that night (one had been up three days longer), so the clock offset printed is the log's.
   const uint64_t m_watchEpoch = 400000000000ull;
   const uint64_t m_playerEpochTicks = (400000000000ull - 279183400000ull) * 10;
   uint64_t m_now = 0; // the watcher's NowUsec

   // The player
   PlayerClock m_clock;
   uint64_t m_phys = 0; // Player::m_curPhysicsFrameTime, on the usec() clock
   bool m_paused = false, m_frozen = false;
   uint64_t m_frozenStamp = 0;
   int64_t m_wrongBy = 0; // JumpBack
   double m_ball = 0.;

   uint32_t m_lastReplayedSeq = 0;
   double m_ballShown = 0.;
   float m_ballStep = 0.f;
   uint64_t m_iRecv = 0, m_iFrames = 0, m_iReplayedCalls = 0;
   double m_iMaxWaitMs = 0.;
};

// SessionImpl::PhysicsStamp: NowUsec() - (usec() - physics clock), the two counters read one after the other
uint64_t World::Stamp(const uint64_t ticks)
{
   if (m_frozen)
      return m_frozenStamp;
   uint64_t between = Rnd() % 3 == 0 ? 1 : 0; // 30 to 60 ns between the two reads, in ticks of 100 ns
   if (m_sc.blipEvery > 0. && Rnd01() < 1. / m_sc.blipEvery)
      between = 20 + Rnd() % 4000; // an interrupt between them: 2 to 400 us
   const int64_t behind = (int64_t)m_clock.Usec(ticks) - (int64_t)m_phys;
   return (uint64_t)((int64_t)PlayerClock::NowUsec(ticks + between) - behind + m_wrongBy);
}

void World::Stats(const double secs)
{
   const double spanMs = m_tl.SpanMs();
   if (spanMs > 1e9)
      m_tot.negativeSpan = true;
   if (m_iRecv > 0 && m_iReplayedCalls == 0)
      ++m_tot.emptyIntervals;
   if (!m_quiet)
      printf("  %7.3f s  stats %.1fs: connected, frames %llu, received %llu (%.1f/s); timeline %zu frames (%.0f ms), starved %llu, buffering %llu, jumps %llu; "
             "replayed calls %llu; clock offset %lld us, delay 100 ms; gaps %llu; longest wait of a frame %.0f ms\n",
         Seconds(), secs, (unsigned long long)m_iFrames, (unsigned long long)m_iRecv, (double)m_iRecv / secs, m_tl.m_frames.size(), spanMs,
         (unsigned long long)m_tl.m_iStarved, (unsigned long long)m_tl.m_iBuffering, (unsigned long long)m_tl.m_iJumps, (unsigned long long)m_iReplayedCalls,
         (long long)m_tl.m_offset, (unsigned long long)m_tl.m_iGaps, m_iMaxWaitMs);
   m_tot.gaps += m_tl.m_iGaps;
   m_tot.jumps += m_tl.m_iJumps;
   m_tot.buffering += m_tl.m_iBuffering;
   m_tot.starved += m_tl.m_iStarved;
   m_tl.ResetInterval();
   m_iRecv = m_iFrames = m_iReplayedCalls = 0;
   m_iMaxWaitMs = 0.;
}

Totals World::Run()
{
   g_rng = 0x9E3779B97F4A7C15ull; // every run draws the same numbers
   std::vector<Timed> events = m_sc.events;
   std::stable_sort(events.begin(), events.end(), [](const Timed& a, const Timed& b) { return a.at < b.at; });
   size_t nextEvent = 0;

   // The two PCs of that night: 73 ms round trip, and now and then a frame (and those behind it) 25 ms late: the
   // watcher's log has frames that "stopped arriving for 22 ms" to 34 ms several times a second
   const double latency = 0.037, jitter = 0.004, hiccup = 0.025;
   const double watcherFrame = 1. / 90.;
   double fps = m_sc.playerFps;
   double netStopUntil = -1.;
   bool netStopped = false;
   std::deque<std::unique_ptr<TFrame>> wire;
   uint64_t lastDue = 0;
   uint32_t seq = 0;
   bool keyframe = false;

   // The player's counter t seconds after the watcher's keyframe (t may be before it), at any tenth of a microsecond
   const auto ticksAt = [this](const double t) { return m_playerEpochTicks + (uint64_t)((t + 1000.) * 1e7) - 10000000000ull + (Rnd() % 10); };
   // The player's history before the watcher connects decides only where its physics clock stands
   double first = 0.;
   for (const Timed& e : events)
      first = (std::min)(first, e.at);
   m_phys = m_clock.Usec(ticksAt(first)) / 1000 * 1000;

   double tPlayer = first, tWatcher = 0., tStats = 5.;
   const double end = m_sc.seconds;
   while (tWatcher < end)
   {
      // The player's frames up to the watcher's next one
      while (tPlayer <= tWatcher)
      {
         while (nextEvent < events.size() && events[nextEvent].at <= tPlayer)
         {
            const Timed& e = events[nextEvent++];
            const uint64_t ticks = ticksAt(tPlayer);
            switch (e.what)
            {
            case Event::Pause: m_paused = true; break;
            case Event::Resume:
               m_paused = false;
               m_phys = m_clock.Usec(ticks); // m_noTimeCorrect: m_curPhysicsFrameTime = initial_time_usec
               break;
            case Event::Freeze:
               m_frozenStamp = Stamp(ticks);
               m_frozen = m_paused = true;
               break;
            case Event::Unfreeze:
               m_frozen = m_paused = false;
               m_phys = m_clock.Usec(ticks);
               break;
            case Event::JumpBack: m_wrongBy -= (int64_t)(e.value * 1e6); break;
            case Event::Rate: fps = e.value; break;
            case Event::NetStop: netStopped = true; break;
            case Event::NetGo:
               netStopped = false;
               netStopUntil = tPlayer;
               break;
            }
         }
         const uint64_t ticks = ticksAt(tPlayer);
         if (!m_paused)
         {
            // UpdatePhysics: whole steps of 1 ms up to now
            const uint64_t nowUsec = m_clock.Usec(ticks);
            if (nowUsec > m_phys)
               m_phys += (nowUsec - m_phys) / 1000 * 1000;
            m_ball += 1500. / fps; // a ball at 1.5 VP units per ms of play
         }
         if (tPlayer >= 0.)
         {
            auto f = std::make_unique<TFrame>();
            f->time = Stamp(ticks);
            f->seq = keyframe ? ++seq : 0;
            f->calls = 20;
            f->ball = m_ball;
            double due = tPlayer + latency + jitter * Rnd01() + (Rnd() % 150 == 0 ? hiccup : 0.);
            if (netStopped)
               due = 1e18; // decided at NetGo
            else if (due < netStopUntil + latency)
               due = netStopUntil + latency;
            f->due = due >= 1e17 ? UINT64_MAX : m_watchEpoch + (uint64_t)(due * 1e6);
            if (f->due != UINT64_MAX)
            {
               f->due = (std::max)(f->due, lastDue); // one TCP connection: in the order sent
               lastDue = f->due;
            }
            if (!keyframe)
            {
               // The keyframe is what the first frame would have been
               keyframe = true;
               m_now = m_watchEpoch;
               m_tl.OnHello();
               m_tl.OnArrival(m_now);
               m_tl.OnKeyframe(f->time, m_now);
               m_ballShown = f->ball;
            }
            else
               wire.push_back(std::move(f));
         }
         tPlayer += 1. / fps;
      }
      // Frames held back by a stopped connection are delivered when it goes again
      if (!netStopped)
         for (auto& f : wire)
            if (f->due == UINT64_MAX)
            {
               f->due = (std::max)(m_watchEpoch + (uint64_t)((netStopUntil + latency) * 1e6), lastDue);
               lastDue = f->due;
            }

      // The watcher's frame: SessionImpl::WatchFrame
      m_now = m_watchEpoch + (uint64_t)(tWatcher * 1e6);
      ++m_iFrames;
      while (!wire.empty() && wire.front()->due <= m_now)
      {
         std::unique_ptr<TFrame> f = std::move(wire.front());
         wire.pop_front();
         f->arrived = m_now;
         m_tl.OnArrival(m_now);
         ++m_iRecv;
         ++m_tot.received;
         m_tl.Push(std::move(f), m_now);
      }
      m_tl.Step(m_now);
      m_tot.maxTimeline = (std::max)(m_tot.maxTimeline, (uint64_t)m_tl.m_frames.size());

      tWatcher += watcherFrame;
      if (tWatcher >= tStats)
      {
         Stats(5.);
         tStats += 5.;
      }
   }
   if (m_tl.m_epOpen)
      m_tl.LogGap();
   m_tot.gaps = m_tl.m_gapsTotal;
   return m_tot;
}

// ---------------------------------------------------------------------------------------------------------

static void Print(const char* const label, const Totals& t)
{
   printf("  %s: received %llu frames, replayed %llu (%llu calls); intervals with frames and no call replayed: %llu; largest timeline %llu frames; "
          "a frame waited %.0f ms at most, %.0f ms on average; gaps logged %llu, jumps %llu, buffering %llu, starved %llu%s\n",
      label, (unsigned long long)t.received, (unsigned long long)t.replayedFrames, (unsigned long long)t.replayedCalls, (unsigned long long)t.emptyIntervals,
      (unsigned long long)t.maxTimeline, t.maxWaitMs, t.replayedFrames ? t.sumWaitMs / (double)t.replayedFrames : 0., (unsigned long long)t.gaps,
      (unsigned long long)t.jumps, (unsigned long long)t.buffering, (unsigned long long)t.starved, t.negativeSpan ? "; a span was printed as a number below zero" : "");
}

// What a watcher owes whatever the player's clock does: every frame's calls are replayed, and none waits longer than
// the delay plus what a gap in the connection explains
static void CheckLive(const Totals& t, const double maxWaitMs)
{
   CHECK(t.emptyIntervals == 0);
   CHECK(t.maxWaitMs <= maxWaitMs);
   CHECK(t.maxTimeline <= 400);
   CHECK(t.received - t.replayedFrames <= 40); // all but the last 100 ms
   CHECK(!t.negativeSpan);
}

int main(const int argc, const char* const* const argv)
{
   const bool quiet = argc > 1 && strcmp(argv[1], "-q") == 0;

   const Scenario steady { "steady", "a player that plays, 144 frames a second, for 20 s", 20., 144., 500., {} };
   const Scenario gap { "gap", "the same, and the connection delivers nothing from 8.0 to 8.6 s", 20., 144., 500., { { 8.0, Event::NetStop, 0. }, { 8.6, Event::NetGo, 0. } } };
   // The first Congo match of 2026-11-23: the player's VPX lost the focus 122 s before the watcher's keyframe and got
   // it back 39 s after it; 120 frames a second while paused, 144 after
   const Scenario pause { "pause", "2026-11-23, 22:12: the player has been paused for 122 s when the watcher connects, and plays 39 s later", 60., 120., 500.,
      { { -122., Event::Pause, 0. }, { 39., Event::Resume, 0. }, { 39., Event::Rate, 144. } } };
   const Scenario pauseNoBlip { "pause, exact", "the same pause with no frame ever stamped late: only the microsecond between the two counters", 60., 120., 0.,
      { { -122., Event::Pause, 0. }, { 39., Event::Resume, 0. }, { 39., Event::Rate, 144. } } };
   const Scenario pauseMid { "pause while watched", "a player that plays, is paused from 10 to 25 s and plays again", 40., 144., 500.,
      { { 10., Event::Pause, 0. }, { 25., Event::Resume, 0. } } };
   const Scenario frozen { "frozen", "match role: the watched table itself watches from 5 to 20 s (one stamp, held as a number)", 30., 144., 500.,
      { { 5., Event::Freeze, 0. }, { 20., Event::Unfreeze, 0. } } };
   const Scenario back { "back", "a clock that is set back by 10 s at 8 s", 30., 144., 500., { { 8., Event::JumpBack, 10. } } };

   const Scenario* const all[] = { &steady, &gap, &pause, &pauseNoBlip, &pauseMid, &frozen, &back };
   for (const Scenario* const sc : all)
   {
      printf("\n%s: %s\n", sc->name, sc->what);
      World w(*sc, quiet);
      const Totals t = w.Run();
      Print("result", t);
      const int failsBefore = g_fails;
      // The delay, and after a hold the catch-up of 300 ms at most
      CheckLive(t, 450.);
      if (sc == &steady)
      {
         CHECK(t.gaps == 0 && t.jumps == 0);
         CHECK(t.buffering <= 12); // the first 100 ms after the keyframe
         CHECK(t.sumWaitMs / (double)t.replayedFrames > 90. && t.sumWaitMs / (double)t.replayedFrames < 125.);
      }
      if (sc == &gap)
         CHECK(t.gaps == 1);
      printf("  %s\n", g_fails == failsBefore ? "ok" : "NOT OK");
   }

   printf("\n%d checks, %d failed\n", g_checks, g_fails);
   return g_fails ? 1 : 0;
}
