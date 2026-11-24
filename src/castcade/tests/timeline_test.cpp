// license:GPLv3+

// Standalone test of the stream's clock (CastcadeTimeline.h), with no VPX and no socket: a player, a connection and a
// watcher on one simulated clock. The player stamps its frames as SessionImpl::PhysicsStamp does (the physics clock
// read through two counters), the connection delays them, and the watcher takes what has arrived once per frame of
// its own and steps the timeline, as SessionImpl::WatchFrame does. Every 5 s of simulated time it prints the part of
// a watcher's stats line that the timeline decides, so that a run can be laid beside a tester's log.
//
// I11.4.17 (2026-11-24): in two matches of 2026-11-23 a watcher showed a still table for 35 to 40 s. Its log had
// frames arriving at 118 a second, `replayed calls 0` and a growing timeline in every interval, a span printed as
// 18446744073709552 ms and a clock offset that grew by 5 s in every 5 s: the player's stamps stood still, because
// the player's game was paused (its window had lost the focus) while its frames went on. Scenario `pause` is that
// night; the others are what must not change (a steady stream, a gap in the connection) and what else a clock can do
// (stand exactly still, be set back).
//
// Each scenario is run with the watcher's rules and the player's stamps of before that date and of after it, in the
// four combinations: a match is two PCs, and either may have the older build.
//
//    timeline_test          the stats lines of the `pause` scenario before and after, one result line for every other run
//    timeline_test -v       every stats line of every run

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
   JumpBack, // the player's stamps are set back by `value` seconds (nothing in the fork does this: a clock that is wrong)
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

// Which build each end runs
struct Builds
{
   bool newWatcher; // WatchTimeline with its rules of 2026-11-24 (false: CASTCADE_STREAM_RESYNC=0, the code as it was)
   bool newPlayer;  // PlayStamp (false: the stamp as it was, NowUsec() - (usec() - physics clock))
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
   uint64_t resyncs = 0, ordered = 0, stands = 0;
   uint64_t stampsBack = 0; // frames the player stamped before the frame it had sent last

   bool SamePicture(const Totals& o) const
   {
      return received == o.received && replayedFrames == o.replayedFrames && replayedCalls == o.replayedCalls && maxTimeline == o.maxTimeline && maxWaitMs == o.maxWaitMs
         && sumWaitMs == o.sumWaitMs && gaps == o.gaps && jumps == o.jumps && buffering == o.buffering && starved == o.starved;
   }
};

class World
{
public:
   World(const Scenario& sc, const Builds builds, const bool stats)
      : m_sc(sc)
      , m_builds(builds)
      , m_stats(stats)
   {
      m_tl.Configure(100000, true, builds.newWatcher); // a match's watcher: CASTCADE_STREAM_DELAY_MS=100, smoothing on
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
      const bool gapLine = line.find("stream gap") != std::string::npos;
      if (gapLine && !m_stats)
         return;
      if (!gapLine || ++m_gapLines <= 8)
         printf("  %7.3f s  %s\n", Seconds(), line.c_str());
      else if (m_gapLines == 9)
         printf("  %7.3f s  (further `stream gap` lines are counted, not printed)\n", Seconds());
   }

private:
   double Seconds() const { return (double)((int64_t)m_now - (int64_t)m_watchEpoch) / 1e6; }
   uint64_t Stamp(const uint64_t ticks);
   void Stats(const double secs);

   const Scenario& m_sc;
   const Builds m_builds;
   const bool m_stats;
   WatchTimeline<TFrame, World> m_tl { *this };
   Totals m_tot;
   int m_gapLines = 0;

   // The watcher's NowUsec at its keyframe. The player's counter reads 279183.4 s less at the same moment, as the two
   // PCs' did that night (one had been up three days longer), so the clock offset printed is the log's.
   const uint64_t m_watchEpoch = 400000000000ull;
   const uint64_t m_playerEpochTicks = (400000000000ull - 279183400000ull) * 10;
   uint64_t m_now = 0; // the watcher's NowUsec

   // The player
   PlayerClock m_clock;
   PlayStamp m_stamp;
   uint64_t m_phys = 0; // Player::m_curPhysicsFrameTime, on the usec() clock
   bool m_paused = false, m_frozen = false;
   uint64_t m_frozenStamp = 0, m_lastStamp = 0;
   int64_t m_wrongBy = 0; // JumpBack
   double m_ball = 0.;

   uint32_t m_lastReplayedSeq = 0;
   double m_ballShown = 0.;
   float m_ballStep = 0.f;
   uint64_t m_iRecv = 0, m_iFrames = 0, m_iReplayedCalls = 0;
   double m_iMaxWaitMs = 0.;
};

// SessionImpl::PhysicsStamp. The two counters are read one after the other: usec() first, NowUsec() 30 to 60 ns
// later, and now and then much later (an interrupt between the two).
uint64_t World::Stamp(const uint64_t ticks)
{
   if (m_frozen)
      return m_frozenStamp;
   uint64_t between = Rnd() % 3 == 0 ? 1 : 0; // in ticks of 100 ns
   if (m_sc.blipEvery > 0. && Rnd01() < 1. / m_sc.blipEvery)
      between = 20 + Rnd() % 4000; // 2 to 400 us
   const uint64_t wall = PlayerClock::NowUsec(ticks + between), vpxNow = m_clock.Usec(ticks);
   // Before 2026-11-24: NowUsec() - (usec() - physics clock)
   const uint64_t stamp = m_builds.newPlayer ? m_stamp.Stamp(wall, vpxNow, m_phys) : (uint64_t)((int64_t)wall - ((int64_t)vpxNow - (int64_t)m_phys));
   return (uint64_t)((int64_t)stamp + m_wrongBy);
}

void World::Stats(const double secs)
{
   const double spanMs = m_tl.SpanMs();
   if (spanMs < 0.)
      m_tot.negativeSpan = true;
   if (m_iRecv > 0 && m_iReplayedCalls == 0)
      ++m_tot.emptyIntervals;
   if (m_stats)
      printf("  %7.3f s  stats %.1fs: connected, frames %llu, received %llu (%.1f/s); timeline %zu frames (%.0f ms), starved %llu, buffering %llu, jumps %llu; "
             "replayed calls %llu; clock offset %lld us, delay 100 ms; gaps %llu; stamps put in order %llu (the furthest back by %lld us), timeline resynchronised %llu, "
             "the player's clock %s; longest wait of a frame %.0f ms\n",
         Seconds(), secs, (unsigned long long)m_iFrames, (unsigned long long)m_iRecv, (double)m_iRecv / secs, m_tl.m_frames.size(), spanMs,
         (unsigned long long)m_tl.m_iStarved, (unsigned long long)m_tl.m_iBuffering, (unsigned long long)m_tl.m_iJumps, (unsigned long long)m_iReplayedCalls,
         (long long)m_tl.m_offset, (unsigned long long)m_tl.m_iGaps, (unsigned long long)m_tl.m_iOrdered, (long long)m_tl.m_iBackMax, (unsigned long long)m_tl.m_iResyncs,
         m_tl.m_clockStands ? "stands still" : "runs", m_iMaxWaitMs);
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
         // The player stamps a frame with every frame it draws, watched or not (SessionImpl::PlayFrame)
         const uint64_t stamp = Stamp(ticks);
         if (tPlayer >= 0.)
         {
            auto f = std::make_unique<TFrame>();
            f->time = stamp;
            f->seq = keyframe ? ++seq : 0;
            f->calls = 20;
            f->ball = m_ball;
            if (keyframe && stamp < m_lastStamp)
               ++m_tot.stampsBack;
            m_lastStamp = stamp;
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
   m_tot.resyncs = m_tl.m_resyncsTotal;
   m_tot.ordered = m_tl.m_orderedTotal;
   m_tot.stands = m_tl.m_standsTotal;
   return m_tot;
}

// ---------------------------------------------------------------------------------------------------------

static void Print(const Totals& t)
{
   printf("  result: received %llu frames, replayed %llu (%llu calls); intervals with frames and no call replayed: %llu; largest timeline %llu frames; "
          "a frame waited %.0f ms at most, %.0f ms on average; gaps logged %llu, jumps %llu, buffering %llu, starved %llu; the player's clock stood %llu times, "
          "resynchronised %llu times, %llu stamps put in order (%llu were sent before the one ahead of them)%s\n",
      (unsigned long long)t.received, (unsigned long long)t.replayedFrames, (unsigned long long)t.replayedCalls, (unsigned long long)t.emptyIntervals,
      (unsigned long long)t.maxTimeline, t.maxWaitMs, t.replayedFrames ? t.sumWaitMs / (double)t.replayedFrames : 0., (unsigned long long)t.gaps,
      (unsigned long long)t.jumps, (unsigned long long)t.buffering, (unsigned long long)t.starved, (unsigned long long)t.stands, (unsigned long long)t.resyncs,
      (unsigned long long)t.ordered, (unsigned long long)t.stampsBack, t.negativeSpan ? "; a span below zero was printed" : "");
}

// What a watcher owes whatever the player's clock does: every frame's calls are replayed, soon, and the timeline
// does not grow
static bool Live(const Totals& t, const double maxWaitMs)
{
   return t.emptyIntervals == 0 && t.maxWaitMs <= maxWaitMs && t.maxTimeline <= 400 && t.received - t.replayedFrames <= 40 /* all but the last 100 ms */ && !t.negativeSpan;
}

// The rule of last resort, alone: frames whose stamps the shown time never reaches, however they came to be held (put
// straight into the timeline here, past the rules that keep such frames out of it)
static void TestOverdue()
{
   printf("\noverdue: frames held under stamps an hour ahead of the clock, which nothing on the way in would let through\n");
   struct Host
   {
      uint64_t now = 0, replayed = 0;
      double maxWaitMs = 0.;
      void TimelineShow(TFrame&, TFrame&, float) { }
      void TimelineReplay(TFrame& f)
      {
         f.replayed = true;
         ++replayed;
         maxWaitMs = (std::max)(maxWaitMs, (double)(now - f.arrived) / 1000.);
      }
      float TimelineBallStep() const { return 0.f; }
      bool TimelineConnected() const { return true; }
      void TimelineLog(const std::string& line) { printf("  %7.3f s  %s\n", (double)(now - 1000000000ull) / 1e6, line.c_str()); }
   };
   for (const bool rules : { false, true })
   {
      Host host;
      WatchTimeline<TFrame, Host> tl(host);
      tl.Configure(100000, true, rules);
      const uint64_t t0 = 1000000000ull;
      tl.OnHello();
      tl.OnKeyframe(5000000, t0);
      uint64_t pushed = 0;
      for (uint64_t now = t0; now < t0 + 10000000; now += 11111)
      {
         host.now = now;
         auto f = std::make_unique<TFrame>();
         f->arrived = now;
         f->time = 5000000 + 3600000000ull; // (stamps that also ran on would be over 2 s apart after 2 s, which the old jump rule sees)
         tl.OnArrival(now);
         tl.m_frames.push_back(std::move(f));
         ++pushed;
         tl.Step(now);
      }
      printf("  %s: %llu frames in 10 s, %llu replayed, the longest wait %.0f ms, %zu still held, resynchronised %llu times\n", rules ? "the rules of 2026-11-24" : "the rules before",
         (unsigned long long)pushed, (unsigned long long)host.replayed, host.maxWaitMs, tl.m_frames.size(), (unsigned long long)tl.m_resyncsTotal);
      if (rules)
      {
         CHECK(host.replayed >= pushed - 200); // all but the last 2.1 s
         CHECK(host.maxWaitMs <= 2150.);
         CHECK(tl.m_resyncsTotal >= 4 && tl.m_resyncsTotal <= 5);
      }
      else
         CHECK(host.replayed == 0); // this is what the rule is for
   }
}

// PlayStamp alone: what a player sends through a pause
static void TestPlayStamp()
{
   printf("\nplayer: the stamps of a game that runs 2 s, is paused 30 s and runs again (144 frames a second)\n");
   PlayerClock clock;
   for (const bool runOn : { false, true })
   {
      PlayStamp stamp;
      g_rng = 0x9E3779B97F4A7C15ull;
      uint64_t ticks = 1208166000000ull, phys = clock.Usec(ticks) / 1000 * 1000, last = 0, lastWall = 0;
      uint64_t back = 0, n = 0, stoodMax = 0;
      int64_t stepMin = INT64_MAX, stepMax = INT64_MIN, errMax = 0;
      for (double t = 0.; t < 40.; t += 1. / 144.)
      {
         const uint64_t at = ticks + (uint64_t)(t * 1e7) + Rnd() % 10;
         const bool paused = t >= 2. && t < 32.;
         if (!paused)
         {
            const uint64_t nowUsec = clock.Usec(at);
            if (t >= 32. && t < 32. + 1. / 144.)
               phys = nowUsec; // m_noTimeCorrect
            else if (nowUsec > phys)
               phys += (nowUsec - phys) / 1000 * 1000;
         }
         const uint64_t between = Rnd() % 500 == 0 ? 20 + Rnd() % 4000 : Rnd() % 3 == 0 ? 1 : 0;
         const uint64_t wall = PlayerClock::NowUsec(at + between);
         const uint64_t s = stamp.Stamp(wall, clock.Usec(at), phys, runOn);
         if (!paused) // while the game runs, the stamp is the physics clock on the NowUsec clock, as it always was
            errMax = (std::max)(errMax, (int64_t)std::llabs((int64_t)s - ((int64_t)wall - ((int64_t)clock.Usec(at) - (int64_t)phys))));
         if (n++)
         {
            const int64_t step = (int64_t)s - (int64_t)last;
            if (step < 0)
               ++back;
            // the step of the stamp beside the step of the wall clock
            stepMin = (std::min)(stepMin, step - (int64_t)(wall - lastWall));
            stepMax = (std::max)(stepMax, step - (int64_t)(wall - lastWall));
         }
         stoodMax = (std::max)(stoodMax, stamp.StoodUsec(wall));
         last = s;
         lastWall = wall;
      }
      printf("  %s: %llu frames, %llu stamped before the one ahead of them; a stamp's step differs from the wall clock's by %lld to %lld us; the physics clock stood %.1f s at most\n",
         runOn ? "PlayStamp" : "the stamp before", (unsigned long long)n, (unsigned long long)back, (long long)stepMin, (long long)stepMax, (double)stoodMax / 1e6);
      CHECK(errMax == 0);
      CHECK(stoodMax >= 29900000 && stoodMax <= 30100000);
      if (runOn)
      {
         CHECK(back == 0);
         CHECK(stepMin > -1500 && stepMax < 1500); // within a physics step and a late reading of the wall clock, pause and resume included
      }
      else
      {
         CHECK(back > 500);         // the microsecond between the two counters, in a pause of 4320 frames
         CHECK(stepMax > 29000000); // the jump at the end of the pause
      }
   }
}

int main(const int argc, const char* const* const argv)
{
   const bool verbose = argc > 1 && strcmp(argv[1], "-v") == 0;

   const Scenario steady { "steady", "a player that plays, 144 frames a second, for 20 s", 20., 144., 500., {} };
   const Scenario gap { "gap", "the same, and the connection delivers nothing from 8.0 to 8.6 s", 20., 144., 500., { { 8.0, Event::NetStop, 0. }, { 8.6, Event::NetGo, 0. } } };
   // The first Congo match of 2026-11-23: the player's VPX lost the focus 122 s before the watcher's keyframe and got
   // it back 39 s after it; 120 frames a second while paused, 144 after
   const Scenario pause { "pause", "2026-11-23, 22:12: the player has been paused for 122 s when the watcher connects, and plays 39 s later", 60., 120., 500.,
      { { -122., Event::Pause, 0. }, { 39., Event::Resume, 0. }, { 39., Event::Rate, 144. } } };
   const Scenario pauseExact { "pause, exact", "the same pause with no frame ever stamped late: only the microsecond between the two counters", 60., 120., 0.,
      { { -122., Event::Pause, 0. }, { 39., Event::Resume, 0. }, { 39., Event::Rate, 144. } } };
   const Scenario pauseMid { "pause while watched", "a player that plays, is paused from 10 to 25 s and plays again", 40., 144., 500.,
      { { 10., Event::Pause, 0. }, { 25., Event::Resume, 0. } } };
   const Scenario frozen { "frozen", "match role: the watched table itself watches from 5 to 20 s (one stamp, held as a number)", 30., 144., 500.,
      { { 5., Event::Freeze, 0. }, { 20., Event::Unfreeze, 0. } } };
   const Scenario back { "back", "stamps that are set back by 10 s at 8 s (no player of ours does that)", 30., 144., 500., { { 8., Event::JumpBack, 10. } } };

   const Scenario* const all[] = { &steady, &gap, &pause, &pauseExact, &pauseMid, &frozen, &back };
   const struct
   {
      Builds builds;
      const char* what;
   } combos[] = { { { false, false }, "BEFORE: the watcher's rules and the player's stamps of before 2026-11-24" },
      { { true, false }, "new watcher, older player" },
      { { false, true }, "older watcher, new player" },
      { { true, true }, "new watcher, new player" } };

   for (const Scenario* const sc : all)
   {
      printf("\n%s: %s\n", sc->name, sc->what);
      Totals before;
      for (const auto& c : combos)
      {
         printf(" %s\n", c.what);
         World w(*sc, c.builds, verbose || (sc == &pause && !c.builds.newPlayer));
         const Totals t = w.Run();
         Print(t);
         const int failsBefore = g_fails;
         const bool anyNew = c.builds.newWatcher || c.builds.newPlayer;
         if (!anyNew)
         {
            before = t;
            // The fault itself, kept in view: the old code against the old stamps must go on failing here as it did
            // that night, or this test no longer tests what it is for. (When it begins is chance: at the first frame
            // stamped late that is not let through at once. 1.2 s and 8.5 s into the two matches, 12 s here.)
            if (sc == &pause)
            {
               CHECK(t.emptyIntervals >= 3 && t.maxTimeline > 2000 && t.maxWaitMs > 20000. && t.negativeSpan);
               CHECK(t.stampsBack > 500);
            }
            if (sc == &back)
               CHECK(t.maxWaitMs > 9000.);
            printf("  %s\n", Live(t, 450.) ? "live throughout" : "NOT LIVE (the fault: expected here)");
            continue;
         }
         // Stamps that are set back are the watcher's to deal with: a player's own stamps never go back, but these are
         // set back behind it, and the older watcher waits 10 s as it always would have
         if (sc == &back && !c.builds.newWatcher)
         {
            printf("  not live, and not this player's to mend (not checked)\n");
            continue;
         }
         // The delay, and after a hold the catch-up of 300 ms at most
         CHECK(Live(t, c.builds.newWatcher && sc != &gap ? 150. : 450.));
         // A stream whose clock runs is shown exactly as it always was
         if (sc == &steady || sc == &gap)
         {
            CHECK(t.SamePicture(before));
            CHECK(t.resyncs == 0 && t.ordered == 0 && t.stands == 0 && t.stampsBack == 0);
         }
         if (c.builds.newWatcher && !c.builds.newPlayer)
         {
            // What the watcher's log says of an older player's clock
            if (sc == &pause || sc == &pauseExact || sc == &pauseMid || sc == &frozen)
               CHECK(t.stands == 1 && t.resyncs == 1);
            if (sc == &back)
               CHECK(t.stands == 0 && t.resyncs == 1);
         }
         if (c.builds.newPlayer)
         {
            // A new player's stamps never go back and never stand through a pause of its own game (they do while its
            // table shows another player's: `frozen`, the one stamp of SessionImpl::Freeze)
            CHECK(sc == &back || t.stampsBack == 0);
            CHECK(t.ordered == 0 || sc == &back);
            if (c.builds.newWatcher && sc != &frozen && sc != &back)
               CHECK(t.stands == 0 && t.resyncs == 0);
         }
         printf("  %s\n", g_fails == failsBefore ? "ok" : "NOT OK");
      }
   }

   TestOverdue();
   TestPlayStamp();

   printf("\n%d checks, %d failed\n", g_checks, g_fails);
   return g_fails ? 1 : 0;
}
