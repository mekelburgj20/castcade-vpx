// license:GPLv3+

#include "stdafx.h"
#include "CastcadeProbe.h"
#include "CastcadeWire.h"
#include "vpversion.h"

#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

// See CastcadeProbe.h. Threads: the render thread only copies a long frame's profile into a queue; the sampler
// thread samples, formats and hands lines to an AsyncLineWriter, whose own thread writes the file.

namespace castcade
{

namespace
{

constexpr int kSections = FrameProfiler::PROFILE_CUSTOM3 + 1; // PROFILE_MISC .. PROFILE_CUSTOM3
const char* const kSectionNames[kSections] = { "misc", "script", "physics", "collect", "submit", "flip", "sleep", "castcade", "custom2", "custom3" };
constexpr unsigned kPollMs = 2;
constexpr int kMaxDepth = 48;
constexpr int kShownDepth = 18;
constexpr size_t kStackCopy = 256 * 1024;
constexpr size_t kStackSlack = 64 * 1024;
constexpr size_t kMaxSamples = 4096;
constexpr uint64_t kSummaryUsec = 10000000;

std::string Fmt(const char* const fmt, ...)
{
   char buf[1024];
   va_list ap;
   va_start(ap, fmt);
   const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   return n < 0 ? std::string() : std::string(buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
}

std::string GetEnvA(const char* const name)
{
   char buf[1024];
   const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
   return (n == 0 || n >= sizeof(buf)) ? std::string() : std::string(buf, n);
}

std::string Stamp(const SYSTEMTIME& t)
{
   return Fmt("%04u-%02u-%02u %02u:%02u:%02u.%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

const char* ScriptEventName(const DISPID id)
{
   switch (id)
   {
   case 1000: return "KeyDown";
   case 1001: return "KeyUp";
   case 1002: return "Init";
   case 1003: return "MusicDone";
   case 1004: return "Exit";
   case 1005: return "Paused";
   case 1006: return "UnPaused";
   case 1007: return "OptionEvent";
   case 1101: return "Slingshot";
   case 1200: return "Collide";
   case 1300: return "Timer";
   case 1301: return "Spin";
   case 1302: return "Dropped";
   case 1303: return "Raised";
   case 1320: return "PlayDone";
   case 1400: return "Hit";
   case 1401: return "Unhit";
   case 1402: return "EOS";
   case 1403: return "BOS";
   case 1404: return "Animate";
   default: return nullptr;
   }
}

struct LongFrame
{
   uint64_t index = 0, start = 0, end = 0; // NowUsec
   SYSTEMTIME wall = {};
   unsigned int sections[kSections] = {};
   unsigned int physMsec = 0, physIterations = 0, physAdvance = 0;
   std::vector<std::tuple<DISPID, unsigned int, unsigned int>> events; // DISPID, calls, usec
   std::string timerLog;
};

struct IoEvent
{
   std::string what;
   uint64_t start, end;
   size_t bytes;
};

struct Sample
{
   uint64_t frame;
   int depth;
   uint64_t pc[kMaxDepth];
};

// Module of an address, and the nearest exported function below it (system DLLs name their functions this way)
class Symbolizer
{
public:
   std::string Name(const uint64_t pc)
   {
      Module* m = Find(pc);
      if (!m)
      {
         Refresh();
         m = Find(pc);
      }
      if (!m)
         return Fmt("0x%llx", (unsigned long long)pc);
      if (!m->exportsRead)
         ReadExports(*m);
      const uint32_t rva = (uint32_t)(pc - m->base);
      const auto it = std::upper_bound(m->exports.begin(), m->exports.end(), rva, [](const uint32_t r, const std::pair<uint32_t, std::string>& e) { return r < e.first; });
      if (it != m->exports.begin())
      {
         const auto& e = *(it - 1);
         if (rva - e.first < 0x8000)
            return Fmt("%s!%s+0x%x", m->name.c_str(), e.second.c_str(), rva - e.first);
      }
      return Fmt("%s+0x%x", m->name.c_str(), rva);
   }

   std::string ModuleName(const uint64_t pc)
   {
      Module* m = Find(pc);
      if (!m)
      {
         Refresh();
         m = Find(pc);
      }
      return m ? m->name : std::string("?");
   }

private:
   struct Module
   {
      uint64_t base, end;
      std::string name;
      bool exportsRead = false;
      std::vector<std::pair<uint32_t, std::string>> exports; // rva, name; sorted
   };

   Module* Find(const uint64_t pc)
   {
      for (Module& m : m_modules)
         if (pc >= m.base && pc < m.end)
            return &m;
      return nullptr;
   }

   void Refresh()
   {
      const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
      if (snap == INVALID_HANDLE_VALUE)
         return;
      MODULEENTRY32W me;
      me.dwSize = sizeof(me);
      for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
      {
         const uint64_t base = (uint64_t)me.modBaseAddr;
         if (Find(base))
            continue;
         char name[MAX_PATH];
         WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, name, sizeof(name), nullptr, nullptr);
         char* dot = strrchr(name, '.');
         if (dot && (!_stricmp(dot, ".dll") || !_stricmp(dot, ".exe")))
            *dot = '\0';
         Module m;
         m.base = base;
         m.end = base + me.modBaseSize;
         m.name = name;
         m_modules.push_back(std::move(m));
      }
      CloseHandle(snap);
   }

   // The module is pinned (its reference count raised) while its export table is read, so it cannot be unloaded
   static void ReadExports(Module& m)
   {
      m.exportsRead = true;
      HMODULE pin = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)m.base, &pin) || (uint64_t)pin != m.base)
      {
         if (pin)
            FreeLibrary(pin);
         return;
      }
      const uint64_t base = m.base;
      const IMAGE_DOS_HEADER* const dos = (const IMAGE_DOS_HEADER*)base;
      const IMAGE_NT_HEADERS* const nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
      if (dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE)
      {
         const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
         if (dir.VirtualAddress != 0 && dir.Size != 0)
         {
            const IMAGE_EXPORT_DIRECTORY* const ed = (const IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
            const DWORD* const funcs = (const DWORD*)(base + ed->AddressOfFunctions);
            const DWORD* const names = (const DWORD*)(base + ed->AddressOfNames);
            const WORD* const ords = (const WORD*)(base + ed->AddressOfNameOrdinals);
            for (DWORD i = 0; i < ed->NumberOfNames && i < 20000; ++i)
            {
               if (ords[i] >= ed->NumberOfFunctions)
                  continue;
               const DWORD rva = funcs[ords[i]];
               if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size)
                  continue; // forwarded
               m.exports.emplace_back(rva, std::string((const char*)(base + names[i])));
            }
         }
      }
      FreeLibrary(pin);
      std::sort(m.exports.begin(), m.exports.end());
   }

   std::vector<Module> m_modules;
};

class Probe;
Probe* g_probe = nullptr; // guarded by ProbeMutex()

std::mutex& ProbeMutex()
{
   static std::mutex m;
   return m;
}

class Probe final : public FrameProbe
{
public:
   Probe(std::unique_ptr<AsyncLineWriter> out, const unsigned thresholdMs)
      : m_out(std::move(out))
      , m_threshold((uint64_t)thresholdMs * 1000)
      , m_sampleAfter((uint64_t)thresholdMs * 600)
   {
      for (std::atomic<uint64_t>& b : m_bucket)
         b.store(0);
#if defined(_M_X64)
      m_thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
      m_stackHigh = (uint64_t)((const NT_TIB*)NtCurrentTeb())->StackBase; // the render thread's: this runs on it
      m_stackBuf.assign(kStackCopy + kStackSlack, 0);
#endif
      char exe[MAX_PATH] = {};
      GetModuleFileNameA(nullptr, exe, sizeof(exe));
      SYSTEMTIME t;
      GetLocalTime(&t);
      m_out->Line(Fmt("%s probe start: pid %lu, %s, build %s, threshold %u ms, stack sampling %s after %u ms of a frame, every %u ms; clock NowUsec %llu",
         Stamp(t).c_str(), GetCurrentProcessId(), exe, VP_VERSION_STRING_POINTS_FULL, thresholdMs, m_thread ? "on" : "off", (unsigned)(m_sampleAfter / 1000), kPollMs,
         (unsigned long long)NowUsec()));
      {
         std::lock_guard<std::mutex> lock(ProbeMutex());
         g_probe = this;
      }
      SetIoObserver(&Probe::OnIo);
      m_summaryAt = NowUsec() + kSummaryUsec;
      m_worker = std::thread([this] { Run(); });
   }

   ~Probe() override
   {
      SetIoObserver(nullptr);
      {
         std::lock_guard<std::mutex> lock(ProbeMutex());
         g_probe = nullptr;
      }
      {
         std::lock_guard<std::mutex> lock(m_mutex);
         m_stop = true;
      }
      m_cv.notify_all();
      if (m_worker.joinable())
         m_worker.join();
      SYSTEMTIME t;
      GetLocalTime(&t);
      m_out->Line(Fmt("%s probe end: %s", Stamp(t).c_str(), Totals().c_str()));
      m_out.reset();
      if (m_thread)
         CloseHandle(m_thread);
   }

   void OnFrameEnd(const unsigned int physMsec, const unsigned int physIterations) override
   {
      const uint64_t now = NowUsec();
      if (m_frameStart == 0)
      {
         m_frameStart = now;
         m_lastPhysMsec = physMsec;
         m_curStart.store(now, std::memory_order_relaxed);
         return;
      }
      const uint64_t len = now - m_frameStart;
      m_frames.fetch_add(1, std::memory_order_relaxed);
      for (int i = 0; i < 5; ++i)
         if (len >= kBuckets[i] * 1000ull)
            m_bucket[i].fetch_add(1, std::memory_order_relaxed);
      if (len > m_maxLen.load(std::memory_order_relaxed))
         m_maxLen.store(len, std::memory_order_relaxed);
      if (len >= m_threshold)
      {
         LongFrame f;
         f.index = m_index;
         f.start = m_frameStart;
         f.end = now;
         GetLocalTime(&f.wall);
         for (int s = 0; s < kSections; ++s)
            f.sections[s] = g_frameProfiler.Get((FrameProfiler::ProfileSection)s);
         f.physMsec = physMsec;
         f.physIterations = physIterations;
         f.physAdvance = physMsec - m_lastPhysMsec;
         g_frameProfiler.ForEachScriptEvent([&f](const DISPID id, const unsigned int calls, const unsigned int usec) { f.events.emplace_back(id, calls, usec); });
         size_t n = 0;
         const char* const log = g_frameProfiler.GetTimerLog(n);
         f.timerLog.assign(log, n);
         {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_frames_q.push_back(std::move(f));
         }
         m_cv.notify_one();
      }
      m_lastPhysMsec = physMsec;
      m_frameStart = now;
      ++m_index;
      // The sampler reads these two: a sample taken while the render thread is suspended pairs them consistently
      m_curIndex.store(m_index, std::memory_order_relaxed);
      m_curStart.store(now, std::memory_order_relaxed);
   }

private:
   static constexpr unsigned kBuckets[5] = { 25, 50, 100, 200, 500 };

   static void OnIo(const char* const what, const uint64_t start, const uint64_t end, const size_t bytes)
   {
      std::lock_guard<std::mutex> lock(ProbeMutex());
      if (!g_probe)
         return;
      {
         std::lock_guard<std::mutex> l2(g_probe->m_mutex);
         g_probe->m_io.push_back(IoEvent { what ? what : "?", start, end, bytes });
      }
      g_probe->m_cv.notify_one();
   }

   std::string Totals() const
   {
      const uint64_t frames = m_frames.load();
      return Fmt("frames %llu, >=25 ms %llu, >=50 ms %llu, >=100 ms %llu, >=200 ms %llu, >=500 ms %llu, longest %.1f ms", (unsigned long long)frames,
         (unsigned long long)m_bucket[0].load(), (unsigned long long)m_bucket[1].load(), (unsigned long long)m_bucket[2].load(), (unsigned long long)m_bucket[3].load(),
         (unsigned long long)m_bucket[4].load(), (double)m_maxLen.load() / 1000.);
   }

   void Run()
   {
      std::deque<LongFrame> frames;
      std::deque<IoEvent> io;
      std::unique_lock<std::mutex> lock(m_mutex);
      for (;;)
      {
         m_cv.wait_for(lock, std::chrono::milliseconds(kPollMs), [this] { return m_stop || !m_frames_q.empty() || !m_io.empty(); });
         frames.swap(m_frames_q);
         io.swap(m_io);
         const bool stop = m_stop;
         lock.unlock();

         if (!stop)
            MaybeSample();
         for (const LongFrame& f : frames)
            Report(f);
         frames.clear();
         for (const IoEvent& e : io)
            m_out->Line(Fmt("io %s %.6f-%.6f s (%.2f ms) %zu B", e.what.c_str(), (double)e.start / 1e6, (double)e.end / 1e6, (double)(e.end - e.start) / 1000., e.bytes));
         io.clear();
         const uint64_t now = NowUsec();
         if (now >= m_summaryAt)
         {
            m_summaryAt = now + kSummaryUsec;
            SYSTEMTIME t;
            GetLocalTime(&t);
            m_out->Line(Fmt("%s summary: %s", Stamp(t).c_str(), Totals().c_str()));
         }

         lock.lock();
         if (stop && m_frames_q.empty() && m_io.empty())
            break;
      }
   }

   void MaybeSample()
   {
#if defined(_M_X64)
      if (!m_thread)
         return;
      const uint64_t start = m_curStart.load(std::memory_order_relaxed);
      if (start == 0 || NowUsec() - start < m_sampleAfter)
         return;
      if (SuspendThread(m_thread) == (DWORD)-1)
         return;
      alignas(16) CONTEXT ctx;
      memset(&ctx, 0, sizeof(ctx));
      ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      // GetThreadContext returns once the thread is really suspended: everything read from here on is consistent
      if (!GetThreadContext(m_thread, &ctx))
      {
         ResumeThread(m_thread);
         return;
      }
      const uint64_t frame = m_curIndex.load(std::memory_order_relaxed);
      const uint64_t rsp = ctx.Rsp;
      size_t n = 0;
      const uint64_t stackLow = m_stackHigh > (64ull << 20) ? m_stackHigh - (64ull << 20) : 0; // the main thread's stack may sit low
      if (rsp >= stackLow && rsp < m_stackHigh)
      {
         n = std::min<size_t>((size_t)(m_stackHigh - rsp), kStackCopy);
         memcpy(m_stackBuf.data(), (const void*)rsp, n); // no allocation and no lock while the render thread is suspended
      }
      ResumeThread(m_thread);
      memset(m_stackBuf.data() + n, 0, kStackSlack);

      Sample s;
      s.frame = frame;
      s.depth = Unwind(ctx, rsp, n, s.pc);
      if (s.depth > 0)
      {
         if (m_samples.size() >= kMaxSamples)
            m_samples.pop_front();
         m_samples.push_back(s);
      }
#endif
   }

#if defined(_M_X64)
   // Unwinds the copy of the stack: every register value that points into the original stack is moved into the copy
   int Unwind(CONTEXT ctx, const uint64_t rsp, const size_t n, uint64_t* const pc) const
   {
      const uint64_t lo = rsp, hi = rsp + n;
      const uint64_t blo = (uint64_t)m_stackBuf.data(), bhi = blo + n;
      const auto tr = [&](DWORD64& r)
      {
         if (r >= lo && r < hi)
            r = r - lo + blo;
      };
      const auto trAll = [&]()
      {
         tr(ctx.Rsp);
         tr(ctx.Rbp);
         tr(ctx.Rbx);
         tr(ctx.Rsi);
         tr(ctx.Rdi);
         tr(ctx.R12);
         tr(ctx.R13);
         tr(ctx.R14);
         tr(ctx.R15);
      };
      if (n == 0)
      {
         pc[0] = ctx.Rip;
         return 1;
      }
      trAll();
      int depth = 0;
      while (depth < kMaxDepth)
      {
         pc[depth++] = ctx.Rip;
         if (ctx.Rsp < blo || ctx.Rsp + 8 > bhi)
            break;
         const DWORD64 prevRsp = ctx.Rsp;
         DWORD64 imageBase = 0;
         PRUNTIME_FUNCTION const fe = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
         if (!fe)
         {
            ctx.Rip = *(const DWORD64*)ctx.Rsp; // a leaf function: the return address is on top
            ctx.Rsp += 8;
         }
         else
         {
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fe, &ctx, &handlerData, &establisher, nullptr);
         }
         trAll();
         if (ctx.Rip == 0 || ctx.Rsp <= prevRsp || ctx.Rsp > bhi)
            break;
      }
      return depth;
   }
#endif

   // Runs of frames in one module are shown once, as the topmost frame of the run with the run's length
   std::string Stack(const Sample& s)
   {
      std::string out;
      int shown = 0;
      for (int i = 0; i < s.depth && shown < kShownDepth;)
      {
         const std::string mod = m_sym.ModuleName(s.pc[i]);
         int j = i + 1;
         while (j < s.depth && m_sym.ModuleName(s.pc[j]) == mod)
            ++j;
         if (!out.empty())
            out += " < ";
         out += m_sym.Name(i == 0 ? s.pc[i] : s.pc[i] - 1); // a return address points after its call
         if (j - i > 1)
            out += Fmt("(x%d)", j - i);
         ++shown;
         i = j;
      }
      return out;
   }

   void Report(const LongFrame& f)
   {
      const double len = (double)(f.end - f.start) / 1000.;
      unsigned int sum = 0;
      for (int s = 0; s < kSections; ++s)
         sum += f.sections[s];
      const double other = len - (double)sum / 1000.;
      std::string line = Fmt("%s frame %llu: %.1f ms, at %.6f-%.6f s; physics clock +%u ms (%u steps)", Stamp(f.wall).c_str(), (unsigned long long)f.index, len,
         (double)f.start / 1e6, (double)f.end / 1e6, f.physAdvance, f.physIterations);
      line += " |";
      for (int s = 0; s < kSections; ++s)
         if (f.sections[s] >= 100 || s <= FrameProfiler::PROFILE_SLEEP)
            line += Fmt(" %s %.1f", kSectionNames[s], (double)f.sections[s] / 1000.);
      line += Fmt(" other %.1f", other);

      // Script: events by time, timers by time
      if (f.sections[FrameProfiler::PROFILE_SCRIPT] >= 1000)
      {
         std::vector<std::tuple<DISPID, unsigned int, unsigned int>> ev = f.events;
         std::sort(ev.begin(), ev.end(), [](const auto& a, const auto& b) { return std::get<2>(a) > std::get<2>(b); });
         line += " | script events:";
         for (size_t i = 0; i < ev.size() && i < 4; ++i)
         {
            const char* const name = ScriptEventName(std::get<0>(ev[i]));
            line += name ? Fmt(" %s x%u %.1f", name, std::get<1>(ev[i]), (double)std::get<2>(ev[i]) / 1000.)
                         : Fmt(" dispid %d x%u %.1f", (int)std::get<0>(ev[i]), std::get<1>(ev[i]), (double)std::get<2>(ev[i]) / 1000.);
         }
         std::map<std::string, std::pair<unsigned, unsigned>> timers;
         size_t pos = 0;
         while (pos < f.timerLog.size())
         {
            const size_t nul = f.timerLog.find('\0', pos);
            if (nul == std::string::npos)
               break;
            const std::string name = f.timerLog.substr(pos, nul - pos);
            pos = nul + 1;
            unsigned int usec = 0;
            if (pos + 4 <= f.timerLog.size())
            {
               memcpy(&usec, f.timerLog.data() + pos, 4);
               pos += 4;
            }
            auto& t = timers[name];
            t.first++;
            t.second += usec;
         }
         if (!timers.empty())
         {
            std::vector<std::pair<std::string, std::pair<unsigned, unsigned>>> tl(timers.begin(), timers.end());
            std::sort(tl.begin(), tl.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
            line += " | timers:";
            for (size_t i = 0; i < tl.size() && i < 5; ++i)
               line += Fmt(" %s x%u %.1f", tl[i].first.c_str(), tl[i].second.first, (double)tl[i].second.second / 1000.);
         }
      }

      // Stack samples of this frame, grouped by their shown form
      std::map<std::string, int> stacks;
      int count = 0;
      for (const Sample& s : m_samples)
         if (s.frame == f.index)
         {
            ++stacks[Stack(s)];
            ++count;
         }
      line += Fmt(" | %d stack samples", count);
      m_out->Line(line);
      if (count)
      {
         std::vector<std::pair<int, std::string>> top;
         for (const auto& kv : stacks)
            top.emplace_back(kv.second, kv.first);
         std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
         for (size_t i = 0; i < top.size() && i < 4; ++i)
            m_out->Line(Fmt("   frame %llu stack %d/%d: %s", (unsigned long long)f.index, top[i].first, count, top[i].second.c_str()));
      }
      // Samples of frames already reported are no longer needed
      while (!m_samples.empty() && m_samples.front().frame <= f.index)
         m_samples.pop_front();
   }

   std::unique_ptr<AsyncLineWriter> m_out;
   const uint64_t m_threshold, m_sampleAfter;

   // Render thread only
   uint64_t m_frameStart = 0;
   uint64_t m_index = 0;
   unsigned int m_lastPhysMsec = 0;

   // Shared
   std::atomic<uint64_t> m_curIndex { 0 }, m_curStart { 0 };
   std::atomic<uint64_t> m_frames { 0 }, m_maxLen { 0 };
   std::atomic<uint64_t> m_bucket[5];
   std::mutex m_mutex;
   std::condition_variable m_cv;
   std::deque<LongFrame> m_frames_q; // guarded by m_mutex
   std::deque<IoEvent> m_io;         // guarded by m_mutex
   bool m_stop = false;              // guarded by m_mutex

   // Sampler thread only
   std::thread m_worker;
   HANDLE m_thread = nullptr;
   uint64_t m_stackHigh = 0;
   std::vector<uint8_t> m_stackBuf;
   std::deque<Sample> m_samples;
   Symbolizer m_sym;
   uint64_t m_summaryAt = 0;
};

} // namespace

FrameProbe* FrameProbe::Create()
{
   const std::string path = GetEnvA("CASTCADE_FRAMELOG");
   if (path.empty())
      return nullptr;
   std::unique_ptr<AsyncLineWriter> out = OpenAsyncLineWriter(path);
   if (!out)
   {
      PLOGE << "Castcade frame probe: cannot open CASTCADE_FRAMELOG '" << path << "'";
      return nullptr;
   }
   const std::string ms = GetEnvA("CASTCADE_FRAMELOG_MS");
   const int threshold = ms.empty() ? 25 : std::clamp(atoi(ms.c_str()), 5, 10000);
   PLOGI << "Castcade frame probe: logging frames of " << threshold << " ms or more to " << path;
   return new Probe(std::move(out), (unsigned)threshold);
}

}
