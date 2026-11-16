// license:GPLv3+

#include "stdafx.h"
#include "CastcadeTest.h"
#include "CastcadeWire.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

// Scripted key presses for automated tests, which can count on neither the focus nor a keyboard. Each event goes
// into PinInput's queue with PushQueue(.., APP_KEYBOARD), where GetInputDeviceData puts a real key press (and the
// touch input in Player::WndProc puts its virtual ones), so ProcessKeys handles it exactly as a real key: its own
// checks first (exit, pause, tweak and debugger keys), then PinInput::FireKeyEvent, which tracks the pressed state
// and fires the table's KeyDown / KeyUp events into the script.

namespace castcade
{

namespace
{

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

class Harness final : public TestHarness
{
public:
   Harness(Player* const player, const bool noPause, const std::string& inputPath)
      : m_player(player)
      , m_noPause(noPause)
   {
      m_out = OpenAsyncLineWriter(GetEnvA("CASTCADE_STREAM_LOG"), "streamlog");
      if (!inputPath.empty())
         Load(inputPath);
   }

   bool NeverPauses() const override { return m_noPause; }

   void OnFrame() override
   {
      if (m_next >= m_keys.size())
         return;
      const uint64_t now = NowUsec();
      if (m_start == 0)
         m_start = now;
      const uint64_t elapsedMs = (now - m_start) / 1000;
      while (m_next < m_keys.size() && m_keys[m_next].ms <= elapsedMs)
      {
         const Key& k = m_keys[m_next++];
         int code = k.code;
         if (!k.key.empty()) // resolved now, through the player's own key bindings
         {
            const EnumAssignKeys which = k.key == "start" ? eStartGameKey : k.key == "credit" ? eAddCreditKey : k.key == "plunger" ? ePlungerKey
                                       : k.key == "lflip" ? eLeftFlipperKey : eRightFlipperKey;
            code = (int)m_player->m_rgKeys[which];
         }
         DIDEVICEOBJECTDATA d = {};
         d.dwOfs = (DWORD)code;
         d.dwData = k.down ? 0x80 : 0;
         d.dwTimeStamp = msec();
         m_player->m_pininput.PushQueue(&d, APP_KEYBOARD);
         // Queued for the log's own thread only: a synchronous write per key press here (to castcade.log and the VPX
         // log, as this tooling did when it lived in the session) stalled the render thread for 50 to 1200 ms whenever
         // the disk or a virus scan held the file, and those stalls were the gaps in the scripted test recordings
         Line("", Fmt("test input at %llu ms (line %d, due %llu ms): %s %s, code %d", (unsigned long long)elapsedMs, k.line, (unsigned long long)k.ms,
            k.key.empty() ? "scan code" : k.key.c_str(), k.down ? "down" : "up", code));
      }
      if (m_next == m_keys.size())
         Line("", "test input finished");
   }

private:
   struct Key
   {
      uint64_t ms;
      bool down;
      std::string key; // a binding name, or empty when code is a scan code
      int code;
      int line;
   };

   void Line(const char* const level, const std::string& s)
   {
      if (!m_out)
         return;
      SYSTEMTIME t;
      GetLocalTime(&t);
      m_out->Line(Fmt("%04u-%02u-%02u %02u:%02u:%02u.%03u [test pid %lu] %s", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
         GetCurrentProcessId(), level) + s);
   }
   void Info(const std::string& s)
   {
      Line("", s);
      PLOGI << "Castcade test: " << s;
   }
   void Warn(const std::string& s)
   {
      Line("WARNING ", s);
      PLOGW << "Castcade test: " << s;
   }

   void Load(const std::string& path)
   {
      FILE* f = nullptr;
      if (fopen_s(&f, path.c_str(), "r") != 0 || !f)
      {
         Warn("test input: cannot read '" + path + "'; carrying on without scripted input");
         return;
      }
      static const char* const names[] = { "start", "credit", "plunger", "lflip", "rflip" };
      char line[512];
      int lineNo = 0;
      while (fgets(line, sizeof(line), f))
      {
         ++lineNo;
         std::string s(line);
         const size_t hash = s.find('#');
         if (hash != std::string::npos)
            s.resize(hash);
         char a[64] = {}, b[64] = {}, c[64] = {}, extra[8] = {};
         const int got = sscanf_s(s.c_str(), "%63s %63s %63s %7s", a, (unsigned)sizeof(a), b, (unsigned)sizeof(b), c, (unsigned)sizeof(c), extra, (unsigned)sizeof(extra));
         if (got <= 0)
            continue; // blank or comment
         Key k { 0, false, std::string(), 0, lineNo };
         bool ok = got == 3;
         char* end = nullptr;
         k.ms = strtoull(a, &end, 10);
         ok = ok && end != a && *end == '\0';
         if (strcmp(b, "down") == 0)
            k.down = true;
         else if (strcmp(b, "up") != 0)
            ok = false;
         if (std::find_if(std::begin(names), std::end(names), [&c](const char* n) { return strcmp(n, c) == 0; }) != std::end(names))
            k.key = c;
         else
         {
            const long code = strtol(c, &end, 10);
            if (end == c || *end != '\0' || code < 1 || code > 255)
               ok = false;
            k.code = (int)code;
         }
         if (!ok)
         {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
               s.pop_back();
            Warn(Fmt("test input line %d is not '<ms> <down|up> <start|credit|plunger|lflip|rflip|scan code>', skipped: %s", lineNo, s.c_str()));
            continue;
         }
         m_keys.push_back(k);
      }
      fclose(f);
      std::stable_sort(m_keys.begin(), m_keys.end(), [](const Key& x, const Key& y) { return x.ms < y.ms; });
      Info(Fmt("test input: %zu events from '%s', the last at %llu ms", m_keys.size(), path.c_str(), m_keys.empty() ? 0ull : (unsigned long long)m_keys.back().ms));
   }

   Player* const m_player;
   const bool m_noPause;
   std::unique_ptr<AsyncLineWriter> m_out;
   std::vector<Key> m_keys;
   size_t m_next = 0;
   uint64_t m_start = 0;
};

} // namespace

TestHarness* TestHarness::Create(Player* const player)
{
   const bool noPause = GetEnvA("CASTCADE_TEST_NOPAUSE") == "1";
   const std::string input = GetEnvA("CASTCADE_TEST_INPUT");
   if (!noPause && input.empty())
      return nullptr;
   return new Harness(player, noPause, input);
}

}
