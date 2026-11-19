// license:GPLv3+

#include "stdafx.h"
#include "CastcadeStream.h"
#include "CastcadeState.h"
#include "CastcadeWire.h"
#include "vpversion.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <iterator>
#include <memory>
#include <share.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The session. See CastcadeStream.h for the configuration and the role of each hook.
//
// Play role. Every successful script call on a table object arrives in OnScriptCall (CastcadeDispatch.h). A
// per-kind policy decides whether it matters to a watcher: property puts that change what is drawn or heard,
// and a short list of methods that start animations or sounds. Puts are deduplicated against the last value
// sent for the same (object, property, index arguments): scripts re-set unchanged values thousands of times a
// frame. The stored values are also the keyframe: a watcher's table, loaded from the same file and never
// scripted, is in the state the player's table was in before its script ran, so replaying every stored put in
// order brings it to the present. Once per frame the calls of that frame and the physics state are sent.
//
// Watch role. Frames are buffered and shown a fixed delay behind the newest one (snapshot interpolation): the
// physics state is interpolated between the two frames around the render time and the script calls of every
// frame up to the render time are replayed through the objects' own IDispatch::Invoke.

namespace castcade
{

bool g_recordCalls = false;
bool g_baselinePuts = false;
bool g_holdEvents = false;

namespace
{

constexpr uint16_t PROTOCOL_VERSION = 1;
constexpr uint8_t ROLE_PLAY = 1;
constexpr uint8_t ROLE_WATCH = 2;
constexpr uint16_t DEFAULT_PORT = 47800;
// HELLO feature bits (the byte after the build string)
constexpr uint8_t HELLO_PHYSICS_STAMPS = 1; // the player stamps each frame with its physics clock: never re-stamp its frames

// Object ids on the wire. Editables are their index in the live table's m_vedit.
constexpr uint32_t ID_TABLE = 0x7FFFFFF0u;
constexpr uint32_t ID_GLOBAL = 0x7FFFFFF1u;
constexpr uint32_t ID_BALL = 0x80000000u; // | network ball id

constexpr uint8_t CALL_PUT = 0;
constexpr uint8_t CALL_PUTREF = 1;
constexpr uint8_t CALL_METHOD = 2;
constexpr uint8_t CALLFLAG_SOUND = 1;

// Policy rule bits, per DISPID of one object kind
constexpr uint8_t RULE_PUT = 1;    // the put is sent (kinds with a put whitelist)
constexpr uint8_t RULE_NO_PUT = 2; // the put is never sent (editables send every other put)
constexpr uint8_t RULE_METHOD = 4; // the method is sent
constexpr uint8_t RULE_STICKY = 8; // the method's last call is kept for the keyframe
constexpr uint8_t RULE_SOUND = 16; // a watcher may drop it (CASTCADE_STREAM_SOUND=0)
// A DispReel has no readable value, and SetValue (or ResetToZero) determines it entirely while AddValue and SpinReel
// step it from there: per reel, the keyframe keeps the last absolute call and every step since, in order. Without
// it a watcher that joins late never sees digits set before it joined (LED score displays emulated with reels).
constexpr uint8_t RULE_REEL_SET = 32;
constexpr uint8_t RULE_REEL_STEP = 64;

enum KindClass : uint8_t
{
   KC_EDITABLE,
   KC_TABLE,
   KC_GLOBAL,
   KC_BALL,
   KC_COUNT
};

constexpr uint64_t RETRY_USEC = 2000000;
constexpr uint64_t HOLD_MAX_USEC = 10000000; // match role: how long a watcher's connection is held while this table is not playing
constexpr uint64_t STATS_USEC = 5000000;
constexpr uint64_t TIMELINE_MAX_USEC = 2000000;
constexpr int64_t CATCHUP_MIN_LAG_USEC = 20000; // a hold shorter than this resumes by jumping, as it always did
constexpr uint64_t CONNECT_FAIL_LOG_USEC = 30000000;
constexpr size_t OFFSET_WINDOW = 300;
constexpr int MAX_LOGGED = 20;
constexpr U32 NOTE_MS = 5000;
constexpr U32 NOTE_LASTING_MS = 24u * 3600u * 1000u;

struct NameRule
{
   const wchar_t* name;
   uint8_t rule;
};

const NameRule kEditableNoPut[] = { { L"Name", RULE_NO_PUT }, { L"UserValue", RULE_NO_PUT }, { L"TimerEnabled", RULE_NO_PUT }, { L"TimerInterval", RULE_NO_PUT } };
const NameRule kBumperMethods[] = { { L"PlayHit", RULE_METHOD } };
const NameRule kDispReelMethods[] = { { L"AddValue", RULE_METHOD | RULE_REEL_STEP }, { L"ResetToZero", RULE_METHOD | RULE_REEL_SET },
   { L"SpinReel", RULE_METHOD | RULE_REEL_STEP }, { L"SetValue", RULE_METHOD | RULE_REEL_SET } };
const NameRule kLightMethods[] = { { L"Duration", RULE_METHOD } };
const NameRule kLightSeqMethods[] = { { L"Play", RULE_METHOD }, { L"StopPlay", RULE_METHOD } };
const NameRule kPrimitiveMethods[] = { { L"PlayAnim", RULE_METHOD }, { L"PlayAnimEndless", RULE_METHOD | RULE_STICKY }, { L"StopAnim", RULE_METHOD | RULE_STICKY },
   { L"ShowFrame", RULE_METHOD | RULE_STICKY }, { L"ContinueAnim", RULE_METHOD } };
const NameRule kSurfaceMethods[] = { { L"PlaySlingshotHit", RULE_METHOD } };
const NameRule kTableRules[] = { { L"BallImage", RULE_PUT }, { L"BallFrontDecal", RULE_PUT }, { L"BallDecalMode", RULE_PUT }, { L"ColorGradeImage", RULE_PUT },
   { L"Image", RULE_PUT }, { L"EnvironmentImage", RULE_PUT }, { L"BackdropImage_DT", RULE_PUT }, { L"BackdropImage_FS", RULE_PUT }, { L"BackdropImage_FSS", RULE_PUT },
   { L"BackdropColor", RULE_PUT }, { L"PlayfieldMaterial", RULE_PUT }, { L"BloomStrength", RULE_PUT }, { L"LightAmbient", RULE_PUT }, { L"Light0Emission", RULE_PUT },
   { L"LightHeight", RULE_PUT }, { L"LightRange", RULE_PUT }, { L"EnvironmentEmissionScale", RULE_PUT }, { L"LightEmissionScale", RULE_PUT }, { L"AOScale", RULE_PUT },
   { L"SSRScale", RULE_PUT }, { L"BallReflection", RULE_PUT }, { L"PlayfieldReflectionStrength", RULE_PUT }, { L"BallTrail", RULE_PUT }, { L"TrailStrength", RULE_PUT },
   { L"BallPlayfieldReflectionScale", RULE_PUT }, { L"DefaultBulbIntensityScale", RULE_PUT }, { L"ReflectElementsOnPlayfield", RULE_PUT }, { L"EnableEMReels", RULE_PUT },
   { L"EnableDecals", RULE_PUT }, { L"NightDay", RULE_PUT } };
const NameRule kGlobalRules[] = { { L"DMDWidth", RULE_PUT }, { L"DMDHeight", RULE_PUT }, { L"DMDPixels", RULE_PUT }, { L"DMDColoredPixels", RULE_PUT },
   { L"DisableStaticPrerendering", RULE_PUT }, { L"MusicVolume", RULE_PUT | RULE_SOUND }, { L"PlaySound", RULE_METHOD | RULE_SOUND },
   { L"StopSound", RULE_METHOD | RULE_SOUND }, { L"PlayMusic", RULE_METHOD | RULE_SOUND }, { L"EndMusic", RULE_METHOD | RULE_SOUND },
   { L"UpdateMaterial", RULE_METHOD }, { L"MaterialColor", RULE_METHOD } };
const NameRule kBallRules[] = { { L"Color", RULE_PUT }, { L"Image", RULE_PUT }, { L"FrontDecal", RULE_PUT }, { L"DecalMode", RULE_PUT }, { L"BulbIntensityScale", RULE_PUT },
   { L"ReflectionEnabled", RULE_PUT }, { L"PlayfieldReflectionScale", RULE_PUT }, { L"ForceReflection", RULE_PUT }, { L"Visible", RULE_PUT } };

// Read-only properties used to check, once per kind, that the IDispatch* the hook reports is the one the map holds
const wchar_t* const kProbeEditable[] = { L"Name", L"Visible", L"X" };
const wchar_t* const kProbeTable[] = { L"Name", L"Width" };
const wchar_t* const kProbeGlobal[] = { L"LeftFlipperKey", L"GameTime" };
const wchar_t* const kProbeBall[] = { L"Name", L"X", L"ID" };

const char* ItemTypeName(const ItemTypeEnum t)
{
   static const char* const names[] = { "Wall", "Flipper", "Timer", "Plunger", "Textbox", "Bumper", "Trigger", "Light", "Kicker", "Decal", "Gate", "Spinner", "Ramp",
      "Table", "LightCenter", "DragPoint", "Collection", "DispReel", "LightSeq", "Primitive", "Flasher", "Rubber", "HitTarget" };
   return (unsigned)t < sizeof(names) / sizeof(names[0]) ? names[t] : "Unknown";
}

const char* const kKindClassNames[KC_COUNT] = { "editable", "table", "global", "ball" };

std::string Fmt(const char* const fmt, ...)
{
   char buf[2048];
   va_list ap;
   va_start(ap, fmt);
   const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   return n < 0 ? std::string() : std::string(buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
}

std::string Narrow(const wchar_t* const w)
{
   if (!w || !*w)
      return std::string();
   const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
   if (n <= 1)
      return std::string();
   std::string s((size_t)n - 1, '\0');
   WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
   return s;
}

// The member name of a DISPID, from the object's type info (logs only); the number when it cannot be named
std::string MemberName(IDispatch* const disp, const DISPID dispid)
{
   std::string name = std::to_string((int)dispid);
   ITypeInfo* ti = nullptr;
   if (disp && SUCCEEDED(disp->GetTypeInfo(0, LOCALE_USER_DEFAULT, &ti)) && ti)
   {
      BSTR bstr = nullptr;
      UINT n = 0;
      if (SUCCEEDED(ti->GetNames(dispid, &bstr, 1, &n)) && n == 1 && bstr)
         name = Narrow(bstr);
      SysFreeString(bstr);
      ti->Release();
   }
   return name;
}

std::string GetEnv(const char* const name)
{
   char buf[1024];
   const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
   if (n == 0 || n >= sizeof(buf))
      return std::string();
   return std::string(buf, n);
}

// For paths: any characters, any length
std::wstring GetEnvW(const wchar_t* const name)
{
   std::wstring buf(MAX_PATH, L'\0');
   DWORD n = GetEnvironmentVariableW(name, buf.data(), (DWORD)buf.size());
   if (n >= buf.size())
   {
      buf.resize(n);
      n = GetEnvironmentVariableW(name, buf.data(), (DWORD)buf.size());
   }
   if (n == 0 || n >= buf.size())
      return std::wstring();
   buf.resize(n);
   return buf;
}

// The table's file name is in the ANSI code page
std::wstring WideFromAnsi(const std::string& s)
{
   if (s.empty())
      return std::wstring();
   const int n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
   std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
   if (n > 0)
      MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), w.data(), n);
   return w;
}

// Creates a folder and every missing folder above it
bool MakeFolders(const std::wstring& dir, std::string& err)
{
   for (size_t i = 1; i <= dir.size(); ++i)
      if (i == dir.size() || dir[i] == L'\\' || dir[i] == L'/')
      {
         const std::wstring part = dir.substr(0, i);
         if (part.back() != L':') // "C:" is no folder to create
            CreateDirectoryW(part.c_str(), nullptr); // existing ones fail harmlessly; the result is checked below
      }
   const DWORD attr = GetFileAttributesW(dir.c_str());
   if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
   {
      err = Fmt("cannot create the folder '%s' (Windows error %lu)", Narrow(dir.c_str()).c_str(), (unsigned long)GetLastError());
      return false;
   }
   return true;
}

// CASTCADE_STREAM_RECORD: a file, or a folder (one that exists, a path ending in a separator, or a last part
// with no extension) in which the file is named after the table and the local time:
// <table file name without .vpx>.<yyyyMMdd-HHmmss>.castrec
bool RecordingPath(const std::wstring& spec, const std::string& tableFile, std::wstring& path, std::string& err)
{
   const DWORD need = GetFullPathNameW(spec.c_str(), 0, nullptr, nullptr);
   std::wstring full((size_t)need, L'\0');
   const DWORD got = need ? GetFullPathNameW(spec.c_str(), need, full.data(), nullptr) : 0;
   if (got == 0 || got >= need)
   {
      err = "'" + Narrow(spec.c_str()) + "' is not a usable path";
      return false;
   }
   full.resize(got);
   const DWORD attr = GetFileAttributesW(full.c_str());
   const size_t sep = full.find_last_of(L"\\/");
   const std::wstring leaf = sep == std::wstring::npos ? full : full.substr(sep + 1);
   const bool folder = (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) || leaf.empty() || leaf.find(L'.') == std::wstring::npos;
   std::wstring dir = folder ? full : full.substr(0, sep == std::wstring::npos ? 0 : sep);
   while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/'))
      dir.pop_back();
   if (!dir.empty() && !MakeFolders(dir, err))
      return false;
   if (!folder)
   {
      path = full;
      return true;
   }

   std::wstring stem = WideFromAnsi(tableFile);
   const size_t dot = stem.find_last_of(L'.');
   if (dot != std::wstring::npos && dot > 0)
      stem.resize(dot);
   for (wchar_t& c : stem)
      if (c < 32 || wcschr(L"<>:\"/\\|?*", c))
         c = L'_';
   if (stem.empty())
      stem = L"table";
   SYSTEMTIME t;
   GetLocalTime(&t);
   wchar_t stamp[32];
   swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
   const std::wstring base = dir + (dir.back() == L'\\' || dir.back() == L'/' ? L"" : L"\\") + stem + L"." + stamp;
   path = base + L".castrec";
   for (int k = 2; k < 100 && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++k)
      path = base + L"-" + std::to_wstring(k) + L".castrec";
   return true;
}

bool ParsePort(const std::string& s, uint16_t& port)
{
   if (s.empty() || s.size() > 5)
      return false;
   unsigned v = 0;
   for (const char c : s)
   {
      if (c < '0' || c > '9')
         return false;
      v = v * 10 + (unsigned)(c - '0');
   }
   if (v == 0 || v > 65535)
      return false;
   port = (uint16_t)v;
   return true;
}

// listen:<port> | <port>               listen on every interface (host empty)
// listen:<ipv4>:<port>                 listen on that interface (host = the address)
// connect:<host>:<port> | <host>:<port>   connect
bool ParseAddr(const std::string& s, bool& listen, std::string& host, uint16_t& port)
{
   std::string rest = s;
   int form = 0; // 0 bare, 1 listen:, 2 connect:
   if (rest.rfind("listen:", 0) == 0)
   {
      form = 1;
      rest = rest.substr(7);
   }
   else if (rest.rfind("connect:", 0) == 0)
   {
      form = 2;
      rest = rest.substr(8);
   }
   const size_t colon = rest.rfind(':');
   host = colon == std::string::npos ? std::string() : rest.substr(0, colon);
   if (!ParsePort(colon == std::string::npos ? rest : rest.substr(colon + 1), port) || (colon != std::string::npos && host.empty()))
      return false;
   listen = form == 1 || (form == 0 && colon == std::string::npos);
   if (form == 2 && host.empty())
      return false;
   if (listen && !host.empty())
   {
      // A bind address must be an IPv4 literal: four dot separated numbers 0..255
      int parts = 0, value = -1;
      for (const char c : host + '.')
      {
         if (c == '.')
         {
            if (value < 0 || ++parts > 4)
               return false;
            value = -1;
         }
         else if (c >= '0' && c <= '9')
         {
            value = (value < 0 ? 0 : value * 10) + (c - '0');
            if (value > 255)
               return false;
         }
         else
            return false;
      }
      if (parts != 4)
         return false;
   }
   return true;
}

// Appends to CASTCADE_STREAM_LOG (two instances on one PC share vpinball.log; this file is per instance) and to the
// VPX log. Nothing is written on the caller's thread: a line is stamped and queued, and the writer's own thread
// appends it (AsyncLineWriter). A synchronous fprintf + fflush here cost the player a frame of 62 ms during the
// owner's recording, and up to 1.2 s in the scripted tests, whenever the disk or a virus scan held the file. With a
// log file, the VPX log gets the warnings and errors only (the VPX log is written on the caller's thread by
// whoever else logs, and its lock is shared); without one, it gets every line, from the writer's thread.
// The mirror writes through VPX's file-only logger (PLOG_NO_DBG_OUT_INSTANCE_ID, as the script's own error log
// does): the default logger's DebugAppender asks the main thread for the active table (SendMessage), and the main
// thread waits for this writer at the session's end (Player::OnClose runs inside the WM_CLOSE handler), so a line
// mirrored there at exit hung both threads. Nothing the writer thread calls waits on the main thread.
class StreamLog final
{
public:
   void Open(const std::string& path, const char* const role)
   {
      m_role = role;
      const std::string prefix = "Castcade " + m_role + ": ";
      const auto mirror = [prefix](const bool all)
      {
         return [prefix, all](const std::string& line)
         {
            // "<date> <time> [<role> pid <n>] [WARNING |ERROR ]<text>"
            const size_t at = line.find("] ");
            const std::string text = at == std::string::npos ? line : line.substr(at + 2);
            if (text.rfind("ERROR ", 0) == 0)
               PLOGE_(PLOG_NO_DBG_OUT_INSTANCE_ID) << prefix << text.substr(6);
            else if (text.rfind("WARNING ", 0) == 0)
               PLOGW_(PLOG_NO_DBG_OUT_INSTANCE_ID) << prefix << text.substr(8);
            else if (all)
               PLOGI_(PLOG_NO_DBG_OUT_INSTANCE_ID) << prefix << text;
         };
      };
      if (!path.empty())
         m_out = OpenAsyncLineWriter(path, "streamlog", mirror(false));
      if (!m_out) // no file asked for, or it cannot be opened: the VPX log gets every line
         m_out = OpenAsyncLineWriter(std::string(), nullptr, mirror(true));
   }

   // Match role: the parts of one session write through one writer (one file handle, one thread, lines in order),
   // each under its own role name. A writer per watch phase would open the file on the render thread at every turn.
   void OpenShared(const StreamLog& other, const char* const role)
   {
      m_role = role;
      m_out = other.m_out;
   }

   void Info(const std::string& s) { Write("", s); }
   void Warn(const std::string& s) { Write("WARNING ", s); }
   void Error(const std::string& s) { Write("ERROR ", s); }

private:
   void Write(const char* const level, const std::string& s)
   {
      if (!m_out)
         return;
      SYSTEMTIME t;
      GetLocalTime(&t);
      char head[96];
      snprintf(head, sizeof(head), "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s pid %lu] %s", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
         m_role.c_str(), GetCurrentProcessId(), level);
      m_out->Line(head + s);
   }

   std::shared_ptr<AsyncLineWriter> m_out;
   std::string m_role;
};

// The name a PlaySound / StopSound argument holds, lower-cased (PinTable matches sound names with lstrcmpi), into key.
// False for a value that is not a string and cannot be made one.
bool SoundKey(const VARIANT& in, std::wstring& key)
{
   const VARIANT* v = &in;
   for (int n = 0; V_VT(v) == (VT_VARIANT | VT_BYREF) && n < 8; ++n)
      v = V_VARIANTREF(v);
   if (V_VT(v) == VT_BSTR || V_VT(v) == (VT_BSTR | VT_BYREF))
   {
      const BSTR s = V_VT(v) == VT_BSTR ? V_BSTR(v) : (V_BSTRREF(v) ? *V_BSTRREF(v) : nullptr);
      key.assign(s ? s : L"", s ? SysStringLen(s) : 0);
   }
   else
   {
      VARIANT tmp;
      VariantInit(&tmp);
      if (FAILED(VariantChangeType(&tmp, const_cast<VARIANT*>(v), 0, VT_BSTR)))
         return false;
      key.assign(V_BSTR(&tmp) ? V_BSTR(&tmp) : L"", V_BSTR(&tmp) ? SysStringLen(V_BSTR(&tmp)) : 0);
      VariantClear(&tmp);
   }
   if (!key.empty())
      CharLowerBuffW(key.data(), (DWORD)key.size());
   return true;
}

// Which Global.StopSound calls can change what a watcher hears. On Congo they were two thirds of every call the
// stream carried: the table stops the rolling sounds of each of its ten possible balls that is not on the table,
// every frame. The rule follows VPX's sound code (PinTable::PlaySound / StopSound, AudioMusicPlayer):
//  - On a watcher a sound plays only because a replayed PlaySound started it, or because a DispReel's own animation
//    clicked its sound (DispReel::UpdateAnimation calls PinTable::PlaySound itself). A keyframe carries no PlaySound,
//    so a sound started before a watcher joined never plays there.
//  - StopSound(name) stops the sound's own buffer and the FIRST copy of it in the list of copied buffers, playing or
//    not. That list only changes inside a PlaySound (of any name: it first drops every copy that has finished), and
//    only a PlaySound starts a stopped buffer again.
// So a StopSound(name) is not sent when
//  (a) no PlaySound(name) has been sent in this session: nothing of that name can be playing on any watcher, or
//  (b) a StopSound(name) has been sent and no PlaySound of any name since: it would find the same buffers, stopped,
// except that the sound of a DispReel is always sent, and (b) is not used at all on a table where a DispReel has a
// sound (a reel's click changes the list of copies at a moment of the watcher's own). Looping sounds, sounds started
// before a watcher joined and repeated stops after a new PlaySound are all covered by (a) and (b).
// The watcher applies the same rule to what it replays, which thins recordings made before the player did.
class StopSoundFilter final
{
public:
   void Init(const std::vector<std::wstring>& reelSounds)
   {
      m_engine.clear();
      for (std::wstring s : reelSounds)
      {
         if (!s.empty())
            CharLowerBuffW(s.data(), (DWORD)s.size());
         m_engine.insert(s);
      }
      m_repeatRule = m_engine.empty();
      m_played.clear();
      m_lastStop.clear();
      m_epoch = 0;
   }

   bool RepeatRule() const { return m_repeatRule; }
   size_t ReelSounds() const { return m_engine.size(); }

   // A watcher completed its HELLO: what was sent before it joined proves nothing about what it heard, so rule (b)
   // starts again (rule (a) stays right: the watcher starts from silence and only this session's PlaySounds start a
   // sound there)
   void ForgetSent() { m_lastStop.clear(); }

   void OnPlay(const std::wstring& key)
   {
      ++m_epoch;
      m_played.insert(key);
   }

   // True when the stop must be sent; it is then counted as sent
   bool Wanted(const std::wstring& key)
   {
      if (m_engine.count(key))
         return true;
      if (!m_played.count(key))
         return false;
      if (m_repeatRule)
      {
         const auto it = m_lastStop.find(key);
         if (it != m_lastStop.end() && it->second == m_epoch)
            return false;
      }
      m_lastStop[key] = m_epoch;
      return true;
   }

private:
   std::unordered_set<std::wstring> m_engine; // DispReel sounds
   std::unordered_set<std::wstring> m_played;
   std::unordered_map<std::wstring, uint64_t> m_lastStop; // name -> m_epoch when its last stop was sent
   uint64_t m_epoch = 0; // PlaySound calls sent
   bool m_repeatRule = true;
};

struct Policy
{
   std::string kind;           // for logs
   bool putsByDefault = false; // editables: every put that is not RULE_NO_PUT
   std::unordered_map<DISPID, uint8_t> rules;
   uint32_t index = 0;         // for the per-name call counts
   IDispatch* sample = nullptr; // an object of this kind: its type info names DISPIDs in the logs
};

struct ObjInfo
{
   uint32_t id;
   const Policy* policy;
   KindClass cls;
   uint32_t epoch; // bumped by every sent method: an equal put after it is sent again (the method may have changed the state)
   const WCHAR* name; // owned by the object; for logs
};

// A put (or sticky method) as last sent: the dedupe reference and a keyframe entry
struct Stored
{
   uint64_t seq = 0;
   uint32_t obj = 0;
   DISPID dispid = 0;
   uint32_t epoch = 0;
   uint8_t kind = 0;
   uint8_t flags = 0;
   uint8_t argc = 0;
   std::vector<uint8_t> args;
};

// A received frame (or keyframe), parsed and with its call arguments decoded
struct RxCall
{
   uint32_t obj;
   DISPID dispid;
   uint8_t kind;
   uint8_t flags;
   uint8_t argc;
   uint32_t argStart;
};

struct RxFrame
{
   RxFrame() = default;
   RxFrame(const RxFrame&) = delete;
   RxFrame& operator=(const RxFrame&) = delete;
   ~RxFrame()
   {
      for (VARIANT& v : args)
         VariantClear(&v);
   }

   uint64_t time = 0;
   uint64_t arrived = 0; // NowUsec when it was received
   uint32_t seq = 0;
   size_t bytes = 0;
   size_t stateBytes = 0;
   std::shared_ptr<const StateSnapshot> state;
   std::vector<RxCall> calls;
   std::vector<VARIANT> args;
   bool replayed = false;
};

struct Peer
{
   std::unique_ptr<TcpPeer> tcp;
   std::string addr;
   bool outgoing = false;
   bool wasConnected = false;
   bool helloOk = false; // the other end's HELLO was checked and accepted
   bool closing = false; // dropped by us (protocol error, mismatch, BYE)
   bool held = false;    // play part of a match session: accepted while not playing, our HELLO not sent yet
   uint64_t heldSince = 0;
   uint64_t sentCounted = 0;
   uint64_t recvCounted = 0;
};

struct Hello
{
   uint16_t version = 0;
   uint8_t role = 0;
   uint32_t count = 0;
   uint64_t hash = 0;
   std::string table;
   std::string build;
   uint8_t features = 0; // HELLO_*; 0 from a build before the byte existed
};

// Owned VARIANTs in IDispatch::Invoke's argument order (rgvarg): for a property put, [0] is the value and the index
// arguments follow
struct VarList
{
   VarList() = default;
   VarList(const VarList&) = delete;
   VarList& operator=(const VarList&) = delete;
   VarList(VarList&& o) noexcept
      : v(std::move(o.v))
   {
      o.v.clear();
   }
   VarList& operator=(VarList&& o) noexcept
   {
      if (this != &o)
      {
         Clear();
         v = std::move(o.v);
         o.v.clear();
      }
      return *this;
   }
   ~VarList() { Clear(); }
   void Clear()
   {
      for (VARIANT& x : v)
         VariantClear(&x);
      v.clear();
   }
   std::vector<VARIANT> v;
};

// Match role: the value a property had before this table's script first put it (the table file's value)
struct Baseline
{
   uint32_t obj = 0;
   DISPID dispid = 0;
   bool readable = false; // false: the property has no getter, or its value cannot be a put's argument
   VarList args;
};

// Match role: sees what a watch phase is about to replay onto the local table, before it is replayed
struct ReplayObserver
{
   virtual ~ReplayObserver() = default;
   virtual void BeforeKeyframe(const RxFrame& kf) = 0;
   virtual void BeforeCall(const RxCall& c, const VARIANT* const args, const KindClass cls, IDispatch* const target) = 0;
};

// Match role: the watch session of one watch phase gets its address and its log from the match session
struct InnerWatch
{
   std::string host;
   uint16_t port = 0;
   ReplayObserver* observer = nullptr;
   const StreamLog* log = nullptr;
   bool staticRefresh = false; // rebuild the static pre-render after a keyframe, as a plain watcher does
};

class SessionImpl;
class MatchSession;
SessionImpl* g_session = nullptr;

class SessionImpl final : public Session
{
   friend class MatchSession;

public:
   // inner: the watch phase of a match session. matchPlay: the play part of one.
   SessionImpl(Player* const player, const bool watch, const InnerWatch* const inner = nullptr, const bool matchPlay = false);
   ~SessionImpl() override;

   bool IsWatcher() const override { return m_watch; }
   bool NeverPauses() const override { return m_neverPauses; }
   void OnBallCreated(Ball* const ball) override;
   void OnBallDestroyed(Ball* const ball) override;
   void OnFrame() override;
   void OnPhysicsStep(const unsigned long long targetUsec) override;

   void Record(IDispatch* const obj, const DISPID dispid, const WORD flags, const DISPPARAMS* const params);
   void BaselineBefore(IDispatch* const obj, const DISPID dispid, const DISPPARAMS* const params);

private:
   // Setup
   void BuildObjectMap();
   const Policy* EditablePolicy(IEditable* const pe, IDispatch* const disp);
   void BuildRules(Policy& p, IDispatch* const disp, const NameRule* const rules, const size_t n);
   void ProbeIdentity(const std::string& kind, IDispatch* const disp, const wchar_t* const* const names, const size_t n);
   void StartNetwork();

   // Play
   void RecordImpl(IDispatch* const obj, const DISPID dispid, const WORD flags, const DISPPARAMS* const params);
   void PlayFrame(const uint64_t now);
   void PlayService(Peer& p);
   void BuildKeyframe(ByteWriter& w);
   void BuildFrame(ByteWriter& w, const uint64_t time);
   void EmitFrame(const uint64_t stamp); // the calls since the last frame and the physics state now, to the watchers and the recording
   uint64_t PhysicsStamp() const;      // the physics clock on the NowUsec clock: what a frame's time field holds

   // Play: the recording (CASTCADE_STREAM_RECORD)
   void OpenRecording(const std::wstring& spec);
   void StartRecording();
   void RecordMessage(const uint8_t type, const uint8_t* const data, const size_t size);
   void CloseRecording();

   // Watch
   void WatchFrame(const uint64_t now);
   void WatchService(Peer& p, const uint64_t now);
   bool ParseFrame(const Message& m, RxFrame& f);
   void ApplyKeyframe(RxFrame& kf);
   void StepTimeline(const uint64_t now);
   void LogGap();
   std::string StepSummary() const;
   void Replay(RxFrame& f);
   IDispatch* Resolve(const uint32_t id, KindClass& cls) const;
   void Refuse(const std::string& why);
   void StopAllSoundsHere(const char* const why); // watch: PinTable::StopAllSounds, counted before and after
   void AddOffsetSample(const int64_t sample);

   // Both
   void BuildHello(ByteWriter& w) const;
   void SendHello(Peer& p);
   bool ParseHello(const Message& m, Hello& h) const;
   bool CheckHello(const Hello& h, std::string& why) const;
   void AccountBytes(Peer& p);
   void Note(const std::string& text, const U32 ms = NOTE_MS)
   {
      if (!m_quiet) // a match session writes nothing on the table: the companion has the match's display
         m_notes.emplace_back(text, ms);
   }

   // Match role, the play part
   void CaptureOwnState(); // into m_state: the physics state now, or the one held while another player's table is shown
   void Freeze();
   void Unfreeze();
   void FlushNotes();
   void LogStats(const uint64_t now);
   Policy* NewPolicy(const int key, const std::string& kind, IDispatch* const sample);
   void LogSentByName(const char* const when);

   Player* const m_player;
   const bool m_watch;
   const bool m_inner;     // the watch phase of a match session
   const bool m_matchPlay; // the play part of a match session
   const bool m_quiet;     // no notifications on the table
   ReplayObserver* const m_observer;
   const bool m_staticRefresh;
   StreamLog m_log;

   // Match role, the play part: while another player's table is shown here, this table's own state as it was when
   // the watch began is what its own watchers are sent, under a stamp that stands still (a paused player's stream)
   bool m_frozen = false;
   std::vector<uint8_t> m_frozenState;
   uint64_t m_frozenStamp = 0;
   // Match role, the play part: the table file's value of every property this table's script has put
   std::unordered_map<std::string, Baseline> m_baseline;
   std::unordered_set<uint64_t> m_baselineSeen; // (object id << 32 | DISPID) of the puts without index arguments
   bool m_inBaseline = false;
   uint64_t m_baselineUnreadable = 0;

   // Configuration
   std::string m_addrText;
   std::string m_connectHost; // connect: the host; listen: the bind address (empty: every interface)
   std::string m_connectText; // host:port, for logs and notifications
   uint16_t m_port = 0;
   bool m_listen = false;
   bool m_configOk = false;
   bool m_neverPauses = false;
   int64_t m_delayUsec = 50000;
   bool m_replaySound = true;

   // Both: Global.StopSound calls that cannot change what a watcher hears are neither sent nor replayed
   StopSoundFilter m_stopFilter;
   bool m_stopFilterOn = true; // CASTCADE_STREAM_STOPFILTER=0 turns it off (tests)
   bool m_stopAudit = false;   // watch, CASTCADE_STREAM_STOPAUDIT=1: replay every stop, and count those the filter would drop that would have stopped a playing sound
   DISPID m_dispPlaySound = DISPID_UNKNOWN, m_dispStopSound = DISPID_UNKNOWN;
   std::wstring m_soundKey;
   uint64_t m_iStopDropped = 0, m_stopDroppedTotal = 0;
   uint64_t m_iAuditEffective = 0, m_iAuditNoEffect = 0, m_auditEffectiveTotal = 0, m_auditNoEffectTotal = 0;

   // Table identity
   uint32_t m_editCount = 0;
   uint64_t m_hash = 0;
   std::string m_tableFile;

   // Network
   bool m_wsa = false;
   std::unique_ptr<TcpListener> m_listener;
   std::vector<Peer> m_peers; // play: any number; watch: at most one
   uint64_t m_nextRetry = 0;
   uint64_t m_lastConnectFailLog = 0;
   bool m_connectNoted = false;
   std::vector<Message> m_msgs;
   std::vector<std::pair<std::string, U32>> m_notes;

   // Play: object map and recording
   std::unordered_map<IDispatch*, ObjInfo> m_objects;
   std::unordered_map<int, std::unique_ptr<Policy>> m_policies; // item type, or the keys below
   std::vector<const Policy*> m_policyByIndex;
   std::unordered_map<uint64_t, uint64_t> m_sentByName; // (policy index << 32 | DISPID) -> calls sent since the session began
   uint64_t m_lastNamesLog = 0;
   static constexpr int POLICY_TABLE = 1000, POLICY_GLOBAL = 1001, POLICY_BALL = 1002;
   const Policy* m_ballPolicy = nullptr;
   std::unordered_map<const Ball*, uint32_t> m_ballIds;
   uint32_t m_nextBallId = 1;
   std::unordered_map<std::string, Stored> m_stored;
   std::unordered_map<uint32_t, std::vector<std::string>> m_reelKeys; // per DispReel: its stored SetValue / steps
   uint64_t m_seq = 0;
   ByteWriter m_args;
   std::string m_key;
   ByteWriter m_frameCalls;
   uint32_t m_frameCallCount = 0;
   uint32_t m_frameSeq = 0;
   ByteWriter m_payload;
   ByteWriter m_state;
   bool m_probing = false;
   IDispatch* m_probeSeen = nullptr;
   int m_probeOk = 0, m_probeBad = 0, m_probeUnverified = 0;
   std::unordered_set<IDispatch*> m_unknownSeen;
   int m_unknownLogged = 0;
   int m_unencodableLogged = 0;
   const uint64_t m_tsc0;
   const uint64_t m_qpc0;

   // Play: the recording. Its frames are numbered on their own (a watcher's numbering stays as without it).
   std::unique_ptr<StreamRecorder> m_recorder;
   std::string m_recordPath; // UTF-8, for logs
   bool m_recordStarted = false; // HELLO and KEYFRAME written
   uint32_t m_recordSeq = 0;
   uint64_t m_recFrames = 0, m_recTicks = 0, m_recTicksMax = 0, m_recBytesAtStats = 0; // per stats interval

   // Play: catch-up frames (OnPhysicsStep). CASTCADE_STREAM_CATCHUP=0 turns them off (tests).
   bool m_catchupFrames = true;
   double m_frameIntervalUsec = 7000.;    // the player's recent frame interval (smoothed)
   uint64_t m_lastFrameAt = 0;            // NowUsec of the last PlayFrame
   uint64_t m_lastEmitPhysics = 0;        // physics clock (usec()) of the last frame sent
   bool m_inCatchup = false;              // this UpdatePhysics call is a catch-up that sends frames
   uint64_t m_iCatchupFrames = 0, m_iCatchups = 0, m_catchupFramesTotal = 0, m_catchupsTotal = 0;
   uint64_t m_iCatchupTicks = 0;

   // Play: per frame and per interval counters
   uint32_t m_frameSeen = 0, m_frameQueued = 0;
   uint64_t m_frameTicks = 0;
   uint64_t m_seenByClass[KC_COUNT] = {}, m_queuedByClass[KC_COUNT] = {};
   uint64_t m_iSeen = 0, m_iQueued = 0, m_iTicks = 0;
   uint32_t m_iSeenMax = 0, m_iQueuedMax = 0;
   uint64_t m_iTicksMax = 0;
   uint64_t m_iDedupe = 0, m_iIgnored = 0, m_iUnencodable = 0, m_iUnknown = 0, m_iFramesSent = 0;

   // Watch
   const DISPID m_dispidPut = DISPID_PROPERTYPUT;
   std::unique_ptr<StateApplier> m_applier;
   std::vector<IDispatch*> m_editDisp;
   IDispatch* m_tableDisp = nullptr;
   IDispatch* m_globalDisp = nullptr;
   std::deque<std::unique_ptr<RxFrame>> m_timeline;
   std::deque<int64_t> m_offsetSamples;
   int64_t m_offset = 0;
   bool m_refused = false;
   bool m_keyframeApplied = false;
   bool m_bye = false;
   int m_replayFailLogged = 0;
   uint64_t m_iRecv = 0, m_iBeforeKeyframe = 0, m_iStarved = 0, m_iBuffering = 0, m_iJumps = 0;
   uint64_t m_iReplayed[KC_COUNT] = {}, m_iFailed = 0, m_iUnresolved = 0, m_iSoundSkipped = 0;
   uint32_t m_frameReplayed = 0, m_iReplayedMax = 0;
   uint64_t m_iReplayedSum = 0;

   // Watch: what happens when the stream had a gap (StepTimeline). CASTCADE_STREAM_SMOOTH=0: the shown time jumps
   // to the target when frames come back, as before; 1 (default): it catches up over a short eased interval.
   bool m_smooth = true;
   bool m_haveShown = false, m_held = false, m_catchup = false;
   int64_t m_shown = 0;         // the stream time the last frame showed
   uint64_t m_catchupStart = 0; // NowUsec
   double m_catchupLag = 0., m_catchupLen = 0.;
   // The gap being measured: how long the picture held, and the largest ball step until the catch-up is over
   uint64_t m_heldSince = 0, m_epHeldSince = 0;
   uint64_t m_lastAnyArrival = 0;     // when the last frame was received (any stamp)
   uint64_t m_holdArrivalGapMax = 0;  // the longest time without a frame received while the picture held
   bool m_stopWhenDrained = false;    // the connection ended: stop every sound again once its last frames are shown
   int64_t m_heldStamp = 0;
   bool m_epOpen = false;
   uint64_t m_epUntil = 0;
   double m_epHeldMs = 0., m_epGapMs = 0., m_epLagMs = 0., m_epCatchupMs = 0., m_epFirstMoveMs = -1.;
   float m_epMaxStep = 0.f;
   uint64_t m_iGaps = 0, m_gapsTotal = 0;
   float m_iMaxStep = 0.f;
   std::vector<double> m_testDrops;   // CASTCADE_TEST_DROP: seconds after the session began to cut the connection at
   bool m_physicsStamps = false;      // the player's HELLO says its frames carry its physics clock (HELLO_PHYSICS_STAMPS)
   bool m_lastRxCorrected = false;    // the last frame received was moved back to its physics time
   uint64_t m_iRebased = 0;           // older stream: frames taken as sent after a moved one (a re-based clock)
   bool m_haveLastRx = false;         // the last frame received: its (possibly corrected) stamp and its physics time
   uint64_t m_lastRxTime = 0;
   uint32_t m_lastRxPhys = 0;
   uint64_t m_iRestamped = 0;         // frames of an older player moved back to their physics time
   std::vector<uint32_t> m_stepHist; // ball steps of the frames outside a gap, 0.5 VPU bins
   uint64_t m_stepFrames = 0;
   float m_stepMax = 0.f;

   // Both
   uint64_t m_statsStart = 0;
   uint64_t m_iFrames = 0;
   uint64_t m_iBytesOut = 0, m_iBytesIn = 0;
};

// ---------------------------------------------------------------------------------------------------------
// Setup and teardown

SessionImpl::SessionImpl(Player* const player, const bool watch, const InnerWatch* const inner, const bool matchPlay)
   : m_player(player)
   , m_watch(watch)
   , m_inner(inner != nullptr)
   , m_matchPlay(matchPlay)
   , m_quiet(inner != nullptr || matchPlay)
   , m_observer(inner ? inner->observer : nullptr)
   , m_staticRefresh(inner ? inner->staticRefresh : true)
   , m_tsc0(__rdtsc())
   , m_qpc0(NowUsec())
{
   if (inner)
      m_log.OpenShared(*inner->log, "watch");
   else
      m_log.Open(GetEnv("CASTCADE_STREAM_LOG"), watch ? "watch" : "play");

   const std::wstring recordSpec = GetEnvW(L"CASTCADE_STREAM_RECORD");
   const bool record = !watch && !recordSpec.empty();
   m_addrText = inner ? "connect:" + inner->host + ":" + std::to_string(inner->port) : GetEnv("CASTCADE_STREAM_ADDR");
   // A player that records and has no address opens no socket at all: no listener on every interface by
   // default, so no Windows Firewall prompt
   if (m_addrText.empty() && !watch && !record)
      m_addrText = std::to_string(DEFAULT_PORT);
   if (m_addrText.empty())
   {
      if (watch)
      {
         m_log.Error("CASTCADE_STREAM=watch needs CASTCADE_STREAM_ADDR (<port> to listen, <host>:<port> to connect); staying idle");
         Note("Castcade: no CASTCADE_STREAM_ADDR set, nothing to watch", NOTE_LASTING_MS);
      }
   }
   else if (!ParseAddr(m_addrText, m_listen, m_connectHost, m_port))
   {
      m_log.Error(Fmt("CASTCADE_STREAM_ADDR '%s' is not listen:[<ipv4>:]<port>, connect:<host>:<port>, <port> or <host>:<port>; staying idle",
         m_addrText.c_str()));
      Note("Castcade: CASTCADE_STREAM_ADDR is not valid: " + m_addrText, NOTE_LASTING_MS);
   }
   else
   {
      m_configOk = true;
      m_connectText = m_connectHost + ":" + std::to_string(m_port);
   }
   m_neverPauses = watch || GetEnv("CASTCADE_TEST_NOPAUSE") == "1";
   if (watch)
   {
      const std::string delay = GetEnv("CASTCADE_STREAM_DELAY_MS");
      if (!delay.empty())
         m_delayUsec = (int64_t)std::clamp(atoi(delay.c_str()), 0, 5000) * 1000;
      const std::string sound = GetEnv("CASTCADE_STREAM_SOUND");
      m_replaySound = sound.empty() || atoi(sound.c_str()) != 0;
      m_stopAudit = GetEnv("CASTCADE_STREAM_STOPAUDIT") == "1";
      m_smooth = GetEnv("CASTCADE_STREAM_SMOOTH") != "0";
      const std::string drops = GetEnv("CASTCADE_TEST_DROP");
      for (size_t at = 0; at < drops.size();)
      {
         const size_t comma = drops.find(',', at);
         const double s = atof(drops.substr(at, comma == std::string::npos ? std::string::npos : comma - at).c_str());
         if (s > 0.)
            m_testDrops.push_back(s);
         at = comma == std::string::npos ? drops.size() : comma + 1;
      }
      std::sort(m_testDrops.begin(), m_testDrops.end());
   }
   else
      m_catchupFrames = GetEnv("CASTCADE_STREAM_CATCHUP") != "0";
   m_stopFilterOn = GetEnv("CASTCADE_STREAM_STOPFILTER") != "0";
   {
      std::vector<std::wstring> reels;
      ReelSoundNames(m_player, reels);
      m_stopFilter.Init(reels);
   }

   const std::string& file = m_player->m_ptable->m_szFileName;
   const size_t slash = file.find_last_of("\\/");
   m_tableFile = slash == std::string::npos ? file : file.substr(slash + 1);

   if (!m_inner)
      g_session = this; // before the identity probes, which go through OnScriptCall
   BuildObjectMap();

   m_log.Info(Fmt("session start: role %s, address %s (%s %s:%u), delay %d ms, sound %s, never pauses %s, table '%s', %u editables, hash %016llx, build %s",
      watch ? "watch" : "play", m_addrText.empty() ? "(none)" : m_addrText.c_str(), m_listen ? "listen on" : "connect to",
      m_listen ? (m_connectHost.empty() ? "0.0.0.0" : m_connectHost.c_str()) : m_connectHost.c_str(), m_port, (int)(m_delayUsec / 1000),
      m_replaySound ? "on" : "off", m_neverPauses ? "yes" : "no", m_tableFile.c_str(), m_editCount, (unsigned long long)m_hash, VP_VERSION_STRING_POINTS_FULL));
   m_log.Info(Fmt("StopSound filter %s (DispReel sounds %zu, repeated-stop rule %s)%s", m_stopFilterOn ? "on" : "off (CASTCADE_STREAM_STOPFILTER=0)",
      m_stopFilter.ReelSounds(), m_stopFilter.RepeatRule() ? "on" : "off", m_stopAudit ? "; AUDIT: every stop is replayed, the ones the filter would drop are checked" : ""));
   m_log.Info(watch ? Fmt("after a gap in the stream: %s", m_smooth ? "catch up over 80-300 ms (CASTCADE_STREAM_SMOOTH=0 jumps instead)" : "jump to the target (CASTCADE_STREAM_SMOOTH=0)")
                    : Fmt("frames stamped with the physics clock; catch-up frames after a stall %s", m_catchupFrames ? "on" : "off (CASTCADE_STREAM_CATCHUP=0)"));
   if (m_stopAudit && !m_player->m_PlaySound)
      m_log.Warn("the StopSound audit needs PlaySound=1 in the watcher's settings: without it no sound plays and every check passes");

   if (watch)
      m_applier = std::make_unique<StateApplier>(m_player);
   // A watcher's window is rarely focused (a VR watcher's almost never): it must not start paused either
   // (Player::RecomputePauseState keeps it unpaused from here on)
   if (m_neverPauses)
      m_player->m_pause = false;

   if (record)
   {
      if (m_addrText.empty())
         m_log.Info("recording only: CASTCADE_STREAM_ADDR is not set, so no socket is opened");
      OpenRecording(recordSpec);
   }
   else if (watch && !recordSpec.empty())
      m_log.Warn("CASTCADE_STREAM_RECORD is ignored on a watcher: the player records");

   if (m_configOk)
      StartNetwork();

   m_statsStart = NowUsec();
   if (!watch)
   {
      g_recordCalls = true; // the script starts right after this returns: its global code is recorded too
      g_baselinePuts = m_matchPlay;
   }
}

SessionImpl::~SessionImpl()
{
   if (!m_inner) // a match session's watch phase ends while its play part goes on
   {
      g_recordCalls = false;
      g_baselinePuts = false;
      g_session = nullptr;
   }
   // Test: CASTCADE_TEST_EXITWARN=<n> writes n warnings right before the session ends, so that the log's writer
   // thread is still mirroring them into the VPX log while this thread waits for it (the exit hang of 050a169)
   const int exitWarn = std::clamp(atoi(GetEnv("CASTCADE_TEST_EXITWARN").c_str()), 0, 100000);
   for (int i = 0; i < exitWarn; ++i)
      m_log.Warn(Fmt("test warning %d of %d, written just before the session ends (CASTCADE_TEST_EXITWARN)", i + 1, exitWarn));
   if (!m_watch)
      LogSentByName("whole session");
   m_log.Info(Fmt("StopSound filter, whole session: %llu %s%s", (unsigned long long)m_stopDroppedTotal, m_watch ? "stops skipped" : "stops not sent",
      m_stopAudit ? Fmt("; audit: %llu stops the filter would drop would have stopped a playing sound, %llu would not", (unsigned long long)m_auditEffectiveTotal,
                       (unsigned long long)m_auditNoEffectTotal).c_str()
                  : ""));
   if (m_watch)
   {
      if (m_epOpen)
         LogGap();
      m_log.Info(Fmt("whole session: %llu gaps in the stream held the picture 20 ms or more; %llu frames of an older player stamped late were put back at their "
                     "physics time (%llu taken as sent after one: a re-based clock); ball steps per frame outside the gaps: %s",
         (unsigned long long)m_gapsTotal, (unsigned long long)m_iRestamped, (unsigned long long)m_iRebased, StepSummary().c_str()));
   }
   else
      m_log.Info(Fmt("whole session: %llu catch-ups after a stall sent %llu frames of their own", (unsigned long long)m_catchupsTotal, (unsigned long long)m_catchupFramesTotal));
   if (m_ballPolicy && m_ballPolicy->sample)
      m_ballPolicy->sample->Release();

   if (m_recorder)
   {
      if (m_recordStarted)
         m_recorder->Write(MSG_BYE, nullptr, 0); // a watcher of the recording is told the player closed the table
      CloseRecording();
   }

   for (Peer& p : m_peers)
   {
      if (p.tcp->Alive())
      {
         p.tcp->Send(MSG_BYE, nullptr, 0);
         m_msgs.clear();
         p.tcp->Poll(m_msgs); // flushes
      }
      AccountBytes(p);
      p.tcp->Close();
   }
   m_peers.clear();
   m_listener.reset();
   m_timeline.clear();
   if (m_applier)
   {
      m_applier->Reset();
      m_applier.reset();
   }
   if (m_wsa)
      WireShutdown();
   m_log.Info("session end");
}

// Both ends build the same id space from the same file before any script runs: editables by their index in the
// live table's m_vedit. The table check hashes, in that order, each editable's type and name.
void SessionImpl::BuildObjectMap()
{
   PinTable* const table = m_player->m_ptable;
   m_editCount = (uint32_t)table->m_vedit.size();
   uint64_t h = 14695981039346656037ull;
   const auto mix = [&h](const void* const p, const size_t n)
   {
      const uint8_t* const b = (const uint8_t*)p;
      for (size_t i = 0; i < n; ++i)
      {
         h ^= b[i];
         h *= 1099511628211ull;
      }
   };

   if (m_watch)
      m_editDisp.assign(m_editCount, nullptr);

   for (uint32_t i = 0; i < m_editCount; ++i)
   {
      IEditable* const pe = table->m_vedit[i];
      const uint32_t type = (uint32_t)pe->GetItemType();
      IScriptable* const ps = pe->GetScriptable();
      const WCHAR* const name = ps ? ps->m_wzName : L"";
      const uint32_t len = (uint32_t)wcslen(name);
      mix(&type, sizeof(type));
      mix(&len, sizeof(len));
      mix(name, len * sizeof(WCHAR));

      if (!ps)
         continue;
      IDispatch* const disp = ps->GetDispatch();
      if (!disp)
         continue;
      if (m_watch)
         m_editDisp[i] = disp;
      else
         m_objects[disp] = ObjInfo { i, EditablePolicy(pe, disp), KC_EDITABLE, 0, ps->m_wzName };
   }
   m_hash = h;

   {
      IDispatch* const g = table->m_psgt->GetDispatch();
      LPOLESTR play = const_cast<LPOLESTR>(L"PlaySound"), stop = const_cast<LPOLESTR>(L"StopSound");
      if (FAILED(g->GetIDsOfNames(IID_NULL, &play, 1, LOCALE_USER_DEFAULT, &m_dispPlaySound)))
         m_dispPlaySound = DISPID_UNKNOWN;
      if (FAILED(g->GetIDsOfNames(IID_NULL, &stop, 1, LOCALE_USER_DEFAULT, &m_dispStopSound)))
         m_dispStopSound = DISPID_UNKNOWN;
   }

   if (m_watch)
   {
      m_tableDisp = table->GetDispatch();
      m_globalDisp = table->m_psgt->GetDispatch();
      return;
   }

   // Table object and global object: whitelists
   IDispatch* const tableDisp = table->GetDispatch();
   Policy* const tp = NewPolicy(POLICY_TABLE, "Table", tableDisp);
   BuildRules(*tp, tableDisp, kTableRules, std::size(kTableRules));
   ProbeIdentity(tp->kind, tableDisp, kProbeTable, std::size(kProbeTable));
   m_objects[tableDisp] = ObjInfo { ID_TABLE, tp, KC_TABLE, 0, table->m_wzName };

   IDispatch* const globalDisp = table->m_psgt->GetDispatch();
   Policy* const gp = NewPolicy(POLICY_GLOBAL, "Global", globalDisp);
   BuildRules(*gp, globalDisp, kGlobalRules, std::size(kGlobalRules));
   ProbeIdentity(gp->kind, globalDisp, kProbeGlobal, std::size(kProbeGlobal));
   m_objects[globalDisp] = ObjInfo { ID_GLOBAL, gp, KC_GLOBAL, 0, L"(global)" };

   m_log.Info(Fmt("object map: %zu objects, %zu kinds; dispatch identity check: %d kinds ok, %d mismatched, %d unverified", m_objects.size(), m_policies.size(),
      m_probeOk, m_probeBad, m_probeUnverified));
}

// One policy per editable type, resolved on the first object of that type
const Policy* SessionImpl::EditablePolicy(IEditable* const pe, IDispatch* const disp)
{
   const ItemTypeEnum type = pe->GetItemType();
   const auto it = m_policies.find((int)type);
   if (it != m_policies.end())
      return it->second.get();

   Policy* const p = NewPolicy((int)type, ItemTypeName(type), disp);
   if (type != eItemTimer) // nothing a timer does is drawn
   {
      p->putsByDefault = true;
      BuildRules(*p, disp, kEditableNoPut, std::size(kEditableNoPut));
      switch (type)
      {
      case eItemBumper: BuildRules(*p, disp, kBumperMethods, std::size(kBumperMethods)); break;
      case eItemDispReel: BuildRules(*p, disp, kDispReelMethods, std::size(kDispReelMethods)); break;
      case eItemLight: BuildRules(*p, disp, kLightMethods, std::size(kLightMethods)); break;
      case eItemLightSeq: BuildRules(*p, disp, kLightSeqMethods, std::size(kLightSeqMethods)); break;
      case eItemPrimitive: BuildRules(*p, disp, kPrimitiveMethods, std::size(kPrimitiveMethods)); break;
      case eItemSurface: BuildRules(*p, disp, kSurfaceMethods, std::size(kSurfaceMethods)); break;
      default: break;
      }
   }
   ProbeIdentity(p->kind, disp, kProbeEditable, std::size(kProbeEditable));
   return p;
}

Policy* SessionImpl::NewPolicy(const int key, const std::string& kind, IDispatch* const sample)
{
   auto p = std::make_unique<Policy>();
   p->kind = kind;
   p->index = (uint32_t)m_policyByIndex.size();
   p->sample = sample;
   Policy* const result = p.get();
   m_policyByIndex.push_back(result);
   m_policies[key] = std::move(p);
   return result;
}

// Which calls a frame carries, by kind and member name (the type info of a sample object names the DISPIDs)
void SessionImpl::LogSentByName(const char* const when)
{
   std::vector<std::pair<uint64_t, uint64_t>> list(m_sentByName.begin(), m_sentByName.end());
   std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
   std::string s;
   for (size_t i = 0; i < list.size() && i < 16; ++i)
   {
      const Policy* const p = m_policyByIndex[(size_t)(list[i].first >> 32)];
      const std::string name = MemberName(p->sample, (DISPID)(uint32_t)list[i].first);
      s += Fmt("%s%s.%s %llu", s.empty() ? "" : ", ", p->kind.c_str(), name.c_str(), (unsigned long long)list[i].second);
   }
   m_log.Info(Fmt("sent calls by name (%s, %zu distinct): %s", when, list.size(), s.empty() ? "none" : s.c_str()));
}

void SessionImpl::BuildRules(Policy& p, IDispatch* const disp, const NameRule* const rules, const size_t n)
{
   std::string unresolved;
   for (size_t i = 0; i < n; ++i)
   {
      LPOLESTR name = const_cast<LPOLESTR>(rules[i].name);
      DISPID id;
      if (SUCCEEDED(disp->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &id)))
         p.rules[id] |= rules[i].rule;
      else
         unresolved += (unresolved.empty() ? "" : ", ") + Narrow(rules[i].name);
   }
   if (!unresolved.empty())
      m_log.Warn(Fmt("policy %s: names that do not resolve with GetIDsOfNames: %s", p.kind.c_str(), unresolved.c_str()));
}

// The hook reports static_cast<IDispatch*>(this) of CastcadeDispatchImpl; the map holds GetDispatch(). They must be
// the same pointer or every call on that kind would be ignored as unknown. Invoke a read-only property through the
// map's pointer and see what the hook reports.
void SessionImpl::ProbeIdentity(const std::string& kind, IDispatch* const disp, const wchar_t* const* const names, const size_t n)
{
   for (size_t i = 0; i < n; ++i)
   {
      LPOLESTR name = const_cast<LPOLESTR>(names[i]);
      DISPID id;
      if (FAILED(disp->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &id)))
         continue;
      DISPPARAMS none = {};
      VARIANT result;
      VariantInit(&result);
      const bool wasRecording = g_recordCalls;
      g_recordCalls = true;
      m_probing = true;
      m_probeSeen = nullptr;
      const HRESULT hr = disp->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &none, &result, nullptr, nullptr);
      m_probing = false;
      g_recordCalls = wasRecording;
      VariantClear(&result);
      if (FAILED(hr))
         continue;
      if (m_probeSeen == disp)
         m_probeOk++;
      else
      {
         m_probeBad++;
         m_log.Error(Fmt("DISPATCH IDENTITY MISMATCH for %s: map holds %p, the hook reported %p; script calls on this kind will be ignored", kind.c_str(), disp,
            m_probeSeen));
      }
      return;
   }
   m_probeUnverified++;
   m_log.Warn(Fmt("dispatch identity of %s could not be checked (no probe property could be read)", kind.c_str()));
}

void SessionImpl::StartNetwork()
{
   if (!WireStartup())
   {
      m_log.Error("Winsock could not be started; staying idle");
      Note("Castcade: networking could not be started", NOTE_LASTING_MS);
      return;
   }
   m_wsa = true;
   if (m_listen)
   {
      // m_connectHost holds the bind address in listen mode (empty: every interface)
      const char* const bind = m_connectHost.empty() ? nullptr : m_connectHost.c_str();
      std::string err;
      m_listener = ListenTcp(m_port, err, bind);
      if (!m_listener)
      {
         m_log.Error(Fmt("cannot listen on %s:%u: %s", bind ? bind : "0.0.0.0", m_port, err.c_str()));
         Note(Fmt("Castcade: cannot listen on port %u (%s)", m_port, err.c_str()), NOTE_LASTING_MS);
         return;
      }
      m_log.Info(Fmt("listening on %s:%u", bind ? bind : "0.0.0.0 (every interface)", m_listener->Port()));
      Note(m_watch ? Fmt("Castcade: waiting for the player on port %u", m_listener->Port()) : Fmt("Castcade: streaming on port %u", m_listener->Port()));
   }
   else if (!m_watch)
      Note("Castcade: streaming to " + m_connectText);
}

// ---------------------------------------------------------------------------------------------------------
// Play role: recording

void SessionImpl::OnBallCreated(Ball* const ball)
{
   if (m_watch || !ball || !ball->m_pballex)
      return;
   IDispatch* const disp = ball->m_pballex->GetDispatch();
   if (!m_ballPolicy)
   {
      // The first ball's BallEx serves as the kind's sample for the logs: keep it alive for the session
      disp->AddRef();
      Policy* const p = NewPolicy(POLICY_BALL, "Ball", disp);
      BuildRules(*p, disp, kBallRules, std::size(kBallRules));
      ProbeIdentity(p->kind, disp, kProbeBall, std::size(kProbeBall));
      m_log.Info(Fmt("ball policy built on the first ball; dispatch identity check so far: %d ok, %d mismatched, %d unverified", m_probeOk, m_probeBad,
         m_probeUnverified));
      m_ballPolicy = p;
   }
   const uint32_t id = m_nextBallId++;
   m_ballIds[ball] = id;
   m_objects[disp] = ObjInfo { ID_BALL | id, m_ballPolicy, KC_BALL, 0, nullptr };
}

void SessionImpl::OnBallDestroyed(Ball* const ball)
{
   if (m_watch || !ball)
      return;
   const auto it = m_ballIds.find(ball);
   if (it == m_ballIds.end())
      return;
   const uint32_t obj = ID_BALL | it->second;
   m_ballIds.erase(it);
   if (ball->m_pballex)
      m_objects.erase(ball->m_pballex->GetDispatch());
   // The ball's stored puts must not reach a later keyframe (no watcher would have that ball)
   for (auto s = m_stored.begin(); s != m_stored.end();)
   {
      if (s->second.obj == obj)
         s = m_stored.erase(s);
      else
         ++s;
   }
}

void SessionImpl::Record(IDispatch* const obj, const DISPID dispid, const WORD flags, const DISPPARAMS* const params)
{
   if (m_probing)
   {
      m_probeSeen = obj;
      return;
   }
   if (m_inBaseline) // the session's own read of a property, not a script call
      return;
   const uint64_t t0 = __rdtsc();
   ++m_frameSeen;
   RecordImpl(obj, dispid, flags, params);
   m_frameTicks += __rdtsc() - t0;
}

// Match role. A watch phase must first bring this table to what the table file gives, for every property this
// table's script has put and the watched player's has not (a lamp lit in this game and never touched in the other
// one), and no keyframe carries such a value. So it is read here, the first time the script is about to put a
// property (object, DISPID, index arguments): what the property holds then is what the file gave it. Every later
// put of it costs one lookup of the object and one of the property.
void SessionImpl::BaselineBefore(IDispatch* const obj, const DISPID dispid, const DISPPARAMS* const params)
{
   if (!params || params->cArgs == 0 || params->cArgs > 8)
      return;
   const uint64_t t0 = __rdtsc();
   const auto it = m_objects.find(obj);
   if (it != m_objects.end() && it->second.cls != KC_BALL)
   {
      const ObjInfo& o = it->second;
      const bool plain = params->cArgs == 1;
      const uint64_t fast = ((uint64_t)o.id << 32) | (uint32_t)dispid;
      if (!plain || !m_baselineSeen.count(fast))
      {
         const Policy& p = *o.policy;
         uint8_t rule = 0;
         const auto r = p.rules.find(dispid);
         if (r != p.rules.end())
            rule = r->second;
         bool wanted = (rule & RULE_PUT) != 0 || (p.putsByDefault && !(rule & RULE_NO_PUT));
         m_key.assign(8, '\0');
         memcpy(m_key.data(), &o.id, 4);
         memcpy(m_key.data() + 4, &dispid, 4);
         if (wanted && !plain)
         {
            m_args.clear();
            for (UINT i = 1; wanted && i < params->cArgs; ++i)
               wanted = EncodeVariant(m_args, params->rgvarg[i]);
            m_key.append((const char*)m_args.data(), m_args.size());
         }
         if (wanted && !m_baseline.count(m_key))
         {
            Baseline b;
            b.obj = o.id;
            b.dispid = dispid;
            b.args.v.resize(params->cArgs);
            for (VARIANT& v : b.args.v)
               VariantInit(&v);
            bool ok = true;
            for (UINT i = 1; ok && i < params->cArgs; ++i)
               ok = SUCCEEDED(VariantCopyInd(&b.args.v[i], &params->rgvarg[i]));
            if (ok)
            {
               DISPPARAMS get = { params->cArgs > 1 ? params->rgvarg + 1 : nullptr, nullptr, params->cArgs - 1, 0 };
               m_inBaseline = true;
               b.readable = SUCCEEDED(obj->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &get, &b.args.v[0], nullptr, nullptr));
               m_inBaseline = false;
               // A value that cannot be a put's argument again (an object) is of no use here
               if (b.readable && (V_VT(&b.args.v[0]) == VT_DISPATCH || V_VT(&b.args.v[0]) == VT_UNKNOWN))
                  b.readable = false;
               if (!b.readable)
               {
                  VariantClear(&b.args.v[0]);
                  ++m_baselineUnreadable;
               }
               m_baseline.emplace(m_key, std::move(b));
            }
         }
         if (plain)
            m_baselineSeen.insert(fast);
      }
   }
   m_frameTicks += __rdtsc() - t0;
}

void SessionImpl::CaptureOwnState()
{
   m_state.clear();
   if (m_frozen)
      m_state.raw(m_frozenState.data(), m_frozenState.size());
   else
      CaptureState(m_player, m_ballIds, m_state);
}

// Match role: this table is about to show another player's. From here to Unfreeze nothing is recorded (what is
// replayed here is not this table's script), and this table's own watchers get its state as it is now.
void SessionImpl::Freeze()
{
   m_frozenStamp = PhysicsStamp();
   m_state.clear();
   CaptureState(m_player, m_ballIds, m_state);
   m_frozenState = m_state.vec();
   m_frozen = true;
   g_recordCalls = false;
   g_baselinePuts = false;
}

void SessionImpl::Unfreeze()
{
   m_frozen = false;
   g_recordCalls = true;
   g_baselinePuts = m_matchPlay;
}

// The hot path: one lookup for the object, one for the policy, encode into a reusable buffer, one lookup in
// the stored map.
void SessionImpl::RecordImpl(IDispatch* const obj, const DISPID dispid, const WORD flags, const DISPPARAMS* const params)
{
   const auto it = m_objects.find(obj);
   if (it == m_objects.end())
   {
      // Every class that reaches this hook is one the map should know (collections and the debugger object
      // are not hooked), so an unknown pointer is a bug or a ball used after DestroyBall: say so.
      ++m_iUnknown;
      if (m_unknownLogged < MAX_LOGGED && m_unknownSeen.insert(obj).second)
      {
         ++m_unknownLogged;
         IUnknown* pBall = nullptr;
         obj->QueryInterface(IID_IBall, (void**)&pBall);
         if (pBall)
            pBall->Release();
         m_log.Warn(Fmt("UNKNOWN OBJECT: script call on %p (%s), %s (dispid %d), flags %u: not in the session's object map, ignored", obj,
            pBall ? "a ball that is not registered, destroyed?" : "not a ball", MemberName(obj, dispid).c_str(), (int)dispid, (unsigned)flags));
      }
      return;
   }
   ObjInfo& o = it->second;
   ++m_seenByClass[o.cls];

   const Policy& p = *o.policy;
   const bool put = (flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) != 0;
   uint8_t rule = 0;
   if (!p.rules.empty())
   {
      const auto r = p.rules.find(dispid);
      if (r != p.rules.end())
         rule = r->second;
   }
   // VBScript calls property gets and functions with DISPATCH_METHOD | DISPATCH_PROPERTYGET, so a method is only
   // what the policy lists as one
   const bool wanted = put ? ((rule & RULE_PUT) != 0 || (p.putsByDefault && !(rule & RULE_NO_PUT))) : (rule & RULE_METHOD) != 0;
   if (!wanted)
   {
      ++m_iIgnored;
      return;
   }

   const UINT argc = params ? params->cArgs : 0;
   bool encoded = argc <= 255 && (!put || argc > 0);
   size_t valueEnd = 0;
   UINT badArg = 0;
   m_args.clear();
   for (UINT i = 0; encoded && i < argc; ++i)
   {
      if (!EncodeVariant(m_args, params->rgvarg[i]))
      {
         encoded = false;
         badArg = i;
      }
      else if (i == 0)
         valueEnd = m_args.size();
   }
   if (!encoded)
   {
      ++m_iUnencodable;
      if (m_unencodableLogged < MAX_LOGGED)
      {
         ++m_unencodableLogged;
         m_log.Warn(Fmt("call dropped, argument not encodable: %s '%s' %s (dispid %d), %u args, arg %u vt 0x%x", p.kind.c_str(), o.name ? Narrow(o.name).c_str() : "",
            MemberName(obj, dispid).c_str(), (int)dispid, argc, badArg, (argc > 0 && params && badArg < argc) ? (unsigned)params->rgvarg[badArg].vt : 0u));
      }
      return;
   }

   const uint8_t kind = put ? ((flags & DISPATCH_PROPERTYPUT) ? CALL_PUT : CALL_PUTREF) : CALL_METHOD;
   const uint8_t cflags = (rule & RULE_SOUND) ? CALLFLAG_SOUND : 0;

   // A stop that cannot change what a watcher hears is not sent (StopSoundFilter); every PlaySound is
   if (!put && o.cls == KC_GLOBAL && m_stopFilterOn && argc >= 1 && (dispid == m_dispStopSound || dispid == m_dispPlaySound)
      && SoundKey(params->rgvarg[argc - 1], m_soundKey))
   {
      if (dispid == m_dispPlaySound)
         m_stopFilter.OnPlay(m_soundKey);
      else if (!m_stopFilter.Wanted(m_soundKey))
      {
         ++m_iStopDropped;
         ++m_stopDroppedTotal;
         return;
      }
   }

   if (put || (rule & RULE_STICKY))
   {
      // Key: object, DISPID and, for a put, the index arguments (all but the value, which is rgvarg[0])
      m_key.assign(8, '\0');
      memcpy(m_key.data(), &o.id, 4);
      memcpy(m_key.data() + 4, &dispid, 4);
      if (put)
         m_key.append((const char*)m_args.data() + valueEnd, m_args.size() - valueEnd);
      const auto ins = m_stored.try_emplace(m_key);
      Stored& e = ins.first->second;
      if (!ins.second && put && e.epoch == o.epoch && e.args == m_args.vec())
      {
         ++m_iDedupe;
         return;
      }
      e.seq = ++m_seq;
      e.obj = o.id;
      e.dispid = dispid;
      e.epoch = o.epoch;
      e.kind = kind;
      e.flags = cflags;
      e.argc = (uint8_t)argc;
      e.args.assign(m_args.data(), m_args.data() + m_args.size());
   }
   else if (rule & (RULE_REEL_SET | RULE_REEL_STEP))
   {
      std::vector<std::string>& keys = m_reelKeys[o.id];
      if (rule & RULE_REEL_SET)
      {
         for (const std::string& k : keys)
            m_stored.erase(k);
         keys.clear();
      }
      const uint64_t seq = ++m_seq;
      m_key.assign(16, '\0');
      memcpy(m_key.data(), &o.id, 4);
      memcpy(m_key.data() + 4, &dispid, 4);
      memcpy(m_key.data() + 8, &seq, 8);
      Stored& e = m_stored[m_key];
      e.seq = seq;
      e.obj = o.id;
      e.dispid = dispid;
      e.epoch = o.epoch;
      e.kind = kind;
      e.flags = cflags;
      e.argc = (uint8_t)argc;
      e.args.assign(m_args.data(), m_args.data() + m_args.size());
      keys.push_back(m_key);
   }
   if (!put)
      ++o.epoch; // methods like Light.Duration change state behind a property's back

   m_frameCalls.varu(o.id);
   m_frameCalls.i32(dispid);
   m_frameCalls.u8(kind);
   m_frameCalls.u8(cflags);
   m_frameCalls.u8((uint8_t)argc);
   m_frameCalls.raw(m_args.data(), m_args.size());
   ++m_frameCallCount;
   ++m_frameQueued;
   ++m_queuedByClass[o.cls];
   ++m_sentByName[((uint64_t)p.index << 32) | (uint32_t)dispid];
}

// ---------------------------------------------------------------------------------------------------------
// Play role: network

void SessionImpl::BuildFrame(ByteWriter& w, const uint64_t time)
{
   CaptureOwnState();
   w.clear();
   w.u64(time);
   w.u32(m_frameSeq);
   w.blob(m_state.data(), m_state.size());
   w.u32(m_frameCallCount);
   w.raw(m_frameCalls.data(), m_frameCalls.size());
}

// The physics state now plus every stored put and sticky method in the order they were made
void SessionImpl::BuildKeyframe(ByteWriter& w)
{
   CaptureOwnState();
   w.clear();
   w.u64(PhysicsStamp());
   w.u32(m_frameSeq);
   w.blob(m_state.data(), m_state.size());
   std::vector<const Stored*> list;
   list.reserve(m_stored.size());
   for (const auto& kv : m_stored)
      list.push_back(&kv.second);
   std::sort(list.begin(), list.end(), [](const Stored* a, const Stored* b) { return a->seq < b->seq; });
   w.u32((uint32_t)list.size());
   for (const Stored* e : list)
   {
      w.varu(e->obj);
      w.i32(e->dispid);
      w.u8(e->kind);
      w.u8(e->flags);
      w.u8(e->argc);
      w.raw(e->args.data(), e->args.size());
   }
}

void SessionImpl::BuildHello(ByteWriter& w) const
{
   w.clear();
   w.raw("CCST", 4);
   w.u16(PROTOCOL_VERSION);
   w.u8(m_watch ? ROLE_WATCH : ROLE_PLAY);
   w.u32(m_editCount);
   w.u64(m_hash);
   w.str(m_tableFile);
   w.str(VP_VERSION_STRING_POINTS_FULL);
   // One byte after the build string. Builds before it read the HELLO's fields in order, check only that they were all
   // there and the magic, and ignore what follows (ParseHello of 8ec92cf / 57305804); the replay tool checks only
   // the message type. So the protocol version stays 1 and both directions keep working.
   w.u8(m_watch ? 0 : HELLO_PHYSICS_STAMPS);
}

void SessionImpl::SendHello(Peer& p)
{
   ByteWriter w;
   BuildHello(w);
   p.tcp->Send(MSG_HELLO, w.data(), w.size());
}

bool SessionImpl::ParseHello(const Message& m, Hello& h) const
{
   ByteReader r(m.payload.data(), m.payload.size());
   char magic[4];
   r.raw(magic, 4);
   h.version = r.u16();
   h.role = r.u8();
   h.count = r.u32();
   h.hash = r.u64();
   h.table = r.str();
   h.build = r.str();
   h.features = r.ok() && r.remaining() >= 1 ? r.u8() : 0; // absent from builds before the byte existed
   return r.ok() && memcmp(magic, "CCST", 4) == 0;
}

bool SessionImpl::CheckHello(const Hello& h, std::string& why) const
{
   if (h.version != PROTOCOL_VERSION)
   {
      why = Fmt("protocol version %u, this build speaks %u", h.version, PROTOCOL_VERSION);
      return false;
   }
   const uint8_t want = m_watch ? ROLE_PLAY : ROLE_WATCH;
   if (h.role != want)
   {
      why = Fmt("the other end has role %u, expected %u", h.role, want);
      return false;
   }
   if (h.count != m_editCount || h.hash != m_hash)
   {
      why = Fmt("table mismatch: this table '%s' has %u editables, hash %016llx; the other end's '%s' has %u editables, hash %016llx", m_tableFile.c_str(),
         m_editCount, (unsigned long long)m_hash, h.table.c_str(), h.count, (unsigned long long)h.hash);
      return false;
   }
   return true;
}

void SessionImpl::AccountBytes(Peer& p)
{
   const uint64_t sent = p.tcp->BytesSent(), recv = p.tcp->BytesReceived();
   m_iBytesOut += sent - p.sentCounted;
   m_iBytesIn += recv - p.recvCounted;
   p.sentCounted = sent;
   p.recvCounted = recv;
}

void SessionImpl::PlayService(Peer& p)
{
   m_msgs.clear();
   const bool alive = p.tcp->Poll(m_msgs);
   if (p.tcp->Connected() && !p.wasConnected)
   {
      p.wasConnected = true;
      if (p.outgoing)
         m_log.Info("connected to " + p.addr);
   }
   for (const Message& m : m_msgs)
   {
      if (p.closing)
         break;
      if (m.type == MSG_HELLO && !p.helloOk)
      {
         Hello h;
         std::string why;
         if (!ParseHello(m, h))
            why = "malformed HELLO";
         else if (CheckHello(h, why))
         {
            p.helloOk = true;
            if (h.build != VP_VERSION_STRING_POINTS_FULL)
               m_log.Warn(Fmt("watcher %s runs build %s, this is %s", p.addr.c_str(), h.build.c_str(), VP_VERSION_STRING_POINTS_FULL));
            m_log.Info(Fmt("watcher connected (%s)", p.addr.c_str()));
            // It starts from silence (it stops every sound at a HELLO): the repeated-stop rule starts again
            m_stopFilter.ForgetSent();
            Note("Castcade: watcher connected (" + p.addr + ")");
            BuildKeyframe(m_payload);
            const size_t states = m_state.size();
            if (!p.tcp->Send(MSG_KEYFRAME, m_payload.data(), m_payload.size()))
            {
               m_log.Warn(Fmt("keyframe to %s could not be sent (%zu bytes): dropping the watcher", p.addr.c_str(), m_payload.size()));
               p.closing = true;
            }
            else
               m_log.Info(Fmt("keyframe to %s: %zu bytes, state %zu bytes, %zu calls", p.addr.c_str(), m_payload.size(), states, m_stored.size()));
            continue;
         }
         m_log.Warn(Fmt("refusing %s: %s", p.addr.c_str(), why.c_str()));
         p.closing = true;
      }
      else if (m.type == MSG_BYE)
      {
         m_log.Info(Fmt("watcher %s said goodbye", p.addr.c_str()));
         p.closing = true;
      }
      else
      {
         m_log.Warn(Fmt("unexpected message type %u from %s: dropping it", m.type, p.addr.c_str()));
         p.closing = true;
      }
   }
   if (alive && !p.closing)
      return;

   AccountBytes(p);
   if (p.helloOk)
   {
      m_log.Info(Fmt("watcher left (%s)", p.addr.c_str()));
      Note("Castcade: watcher left");
   }
   else if (p.outgoing && !p.wasConnected)
   {
      const uint64_t now = NowUsec();
      if (now - m_lastConnectFailLog >= CONNECT_FAIL_LOG_USEC)
      {
         m_lastConnectFailLog = now;
         m_log.Info("cannot reach " + p.addr + " yet, retrying every 2 s");
      }
   }
   else if (!p.closing)
      m_log.Info(Fmt("connection with %s closed before its HELLO", p.addr.c_str()));
   p.tcp->Close();
   p.closing = true;
}

void SessionImpl::PlayFrame(const uint64_t now)
{
   // Per frame recording cost (the script work of this frame is complete here)
   m_iSeen += m_frameSeen;
   m_iQueued += m_frameQueued;
   m_iTicks += m_frameTicks;
   m_iSeenMax = (std::max)(m_iSeenMax, m_frameSeen);
   m_iQueuedMax = (std::max)(m_iQueuedMax, m_frameQueued);
   m_iTicksMax = (std::max)(m_iTicksMax, m_frameTicks);
   m_frameSeen = m_frameQueued = 0;
   m_frameTicks = 0;

   if (m_listener)
      while (std::unique_ptr<TcpPeer> t = m_listener->Accept())
      {
         Peer p;
         p.addr = t->RemoteAddress();
         p.tcp = std::move(t);
         if (m_frozen)
         {
            // Match role, the hand-over: the watcher of this table's next turn arrives while this table still shows
            // the other player's. Accepted and held: no HELLO, no keyframe, until this table plays again.
            p.held = true;
            p.heldSince = now;
            m_log.Info("connection from " + p.addr + " held: this table is not playing yet");
         }
         else
         {
            m_log.Info("connection from " + p.addr);
            SendHello(p);
         }
         m_peers.push_back(std::move(p));
      }
   if (m_configOk && m_wsa && !m_listen && !m_frozen && now >= m_nextRetry
      && std::none_of(m_peers.begin(), m_peers.end(), [](const Peer& p) { return p.outgoing; }))
   {
      m_nextRetry = now + RETRY_USEC;
      std::string err;
      std::unique_ptr<TcpPeer> t = ConnectTcp(m_connectHost, m_port, err);
      if (t)
      {
         Peer p;
         p.addr = m_connectText;
         p.outgoing = true;
         p.tcp = std::move(t);
         SendHello(p); // queued until the connection completes
         m_peers.push_back(std::move(p));
      }
      else if (now - m_lastConnectFailLog >= CONNECT_FAIL_LOG_USEC)
      {
         m_lastConnectFailLog = now;
         m_log.Warn("cannot connect to " + m_connectText + ": " + err);
      }
   }

   for (Peer& p : m_peers)
   {
      if (m_frozen && !p.helloOk)
      {
         // Not playing: a watcher that has not been given its keyframe yet gets none now (its HELLO, if it has sent
         // one, stays unread). Held for 10 s at most.
         if (p.heldSince == 0)
            p.heldSince = now;
         else if (now - p.heldSince >= HOLD_MAX_USEC)
         {
            m_log.Info(Fmt("connection from %s closed: held 10 s and this table is still not playing", p.addr.c_str()));
            p.tcp->Close();
            p.closing = true;
         }
         continue;
      }
      if (p.held)
      {
         p.held = false;
         m_log.Info(Fmt("connection from %s served after %.0f ms on hold", p.addr.c_str(), (double)(now - p.heldSince) / 1000.));
         SendHello(p);
      }
      p.heldSince = 0;
      PlayService(p);
   }
   m_peers.erase(std::remove_if(m_peers.begin(), m_peers.end(), [](const Peer& p) { return p.closing; }), m_peers.end());

   // The recording gets what a watcher connected now would: a HELLO and a keyframe, then this frame and the next
   if (m_recorder && !m_recordStarted)
      StartRecording();

   // The player's frame interval, smoothed: the catch-up thresholds scale with it. A stalled frame counts as 2.5
   // intervals at most, so one stall moves it by 7.5% while a slower frame rate still takes over within 30 frames.
   if (m_lastFrameAt)
      m_frameIntervalUsec = 0.95 * m_frameIntervalUsec + 0.05 * (std::min)((double)(now - m_lastFrameAt), 2.5 * m_frameIntervalUsec);
   m_lastFrameAt = now;
   m_inCatchup = false;

   EmitFrame(PhysicsStamp());
}

uint64_t SessionImpl::PhysicsStamp() const
{
   if (m_frozen) // match role, another player's table is shown here: this game stands still, and so does its stamp
      return m_frozenStamp;
   // usec() and NowUsec() both count QueryPerformanceCounter, from different origins
   const int64_t behind = (int64_t)usec() - (int64_t)PhysicsClockUsec(m_player);
   return (uint64_t)((int64_t)NowUsec() - behind);
}

void SessionImpl::EmitFrame(const uint64_t stamp)
{
   const bool toWatchers = std::any_of(m_peers.begin(), m_peers.end(), [](const Peer& p) { return p.helloOk; });
   if (toWatchers || m_recorder)
   {
      const uint64_t t0 = __rdtsc();
      if (toWatchers)
         ++m_frameSeq;
      BuildFrame(m_payload, stamp);
      uint64_t recTicks = toWatchers ? 0 : __rdtsc() - t0; // built for the recording alone: its cost
      if (toWatchers)
      {
         for (Peer& p : m_peers)
            if (p.helloOk && !p.tcp->Send(MSG_FRAME, m_payload.data(), m_payload.size()))
            {
               AccountBytes(p);
               m_log.Warn(Fmt("watcher %s dropped: the connection closed or it fell more than 8 MiB behind", p.addr.c_str()));
               Note("Castcade: watcher left");
               p.closing = true;
            }
         m_peers.erase(std::remove_if(m_peers.begin(), m_peers.end(), [](const Peer& p) { return p.closing; }), m_peers.end());
         ++m_iFramesSent;
      }
      if (m_recorder)
      {
         // The same payload under the recording's own frame number (after the watchers have been sent theirs)
         const uint64_t t1 = __rdtsc();
         m_payload.patch_u32(8, ++m_recordSeq);
         RecordMessage(MSG_FRAME, m_payload.data(), m_payload.size());
         recTicks += __rdtsc() - t1;
         ++m_recFrames;
         m_recTicks += recTicks;
         m_recTicksMax = (std::max)(m_recTicksMax, recTicks);
      }
   }
   m_frameCalls.clear();
   m_frameCallCount = 0;
   m_lastEmitPhysics = PhysicsClockUsec(m_player);
}

// After a stall of the render thread (a script that waited on a file, the GPU driver, ...) the next UpdatePhysics
// simulates the whole stall in one go, and a frame sent after it would carry the end state only: a watcher could
// only jump to it, or draw a straight line through whatever the ball hit meanwhile. So while UpdatePhysics is
// catching up by more than 2.5 frame intervals (and at least 25 ms), every frame interval of simulated time is sent
// as a frame of its own: the state then and the script calls made up to then, stamped with the physics clock. It
// costs a CaptureState and a send per interval of the catch-up (about 0.1 ms each; 90 of them for a 650 ms stall at
// 144 fps), only during a catch-up; normal frames are unchanged.
void SessionImpl::OnPhysicsStep(const unsigned long long targetUsec)
{
   if (m_watch || m_frozen || !m_catchupFrames || m_lastFrameAt == 0 || (!m_recorder && m_peers.empty()))
      return;
   const uint64_t phys = PhysicsClockUsec(m_player);
   const double interval = std::clamp(m_frameIntervalUsec, 3000., 17000.);
   if (!m_inCatchup)
   {
      // Decided at the step that starts the catch-up: how far this UpdatePhysics has left to go
      if ((double)((int64_t)targetUsec - (int64_t)phys) < (std::max)(25000., 2.5 * interval))
         return;
      m_inCatchup = true;
      ++m_iCatchups;
      ++m_catchupsTotal;
   }
   if ((double)(phys - m_lastEmitPhysics) < interval || (int64_t)targetUsec - (int64_t)phys < (int64_t)(interval * 0.5))
      return; // not yet a frame interval since the last frame; or the frame that follows the catch-up is due
   const uint64_t t0 = __rdtsc();
   EmitFrame(PhysicsStamp());
   m_iCatchupTicks += __rdtsc() - t0;
   ++m_iCatchupFrames;
   ++m_catchupFramesTotal;
}

// ---------------------------------------------------------------------------------------------------------
// Play role: the recording

void SessionImpl::OpenRecording(const std::wstring& spec)
{
   std::wstring path;
   std::string err;
   if (RecordingPath(spec, m_tableFile, path, err))
   {
      RecordingInfo info;
      info.protocol = PROTOCOL_VERSION;
      info.partCount = m_editCount;
      info.hash = m_hash;
      info.table = Narrow(WideFromAnsi(m_tableFile).c_str());
      info.build = VP_VERSION_STRING_POINTS_FULL;
      m_recorder = CreateRecorder(path, info, err);
      m_recordPath = Narrow(path.c_str());
      if (m_recorder)
      {
         const size_t slash = path.find_last_of(L"\\/");
         m_log.Info("recording to " + m_recordPath);
         Note("Castcade: recording to " + Narrow(path.substr(slash == std::wstring::npos ? 0 : slash + 1).c_str()));
         return;
      }
      err = "'" + m_recordPath + "': " + err;
   }
   m_log.Error("cannot record (CASTCADE_STREAM_RECORD): " + err + "; playing on without a recording");
   Note("Castcade: cannot record: " + err, NOTE_LASTING_MS);
}

void SessionImpl::StartRecording()
{
   m_recordStarted = true;
   ByteWriter hello;
   BuildHello(hello);
   RecordMessage(MSG_HELLO, hello.data(), hello.size());
   if (!m_recorder)
      return;
   BuildKeyframe(m_payload);
   m_payload.patch_u32(8, m_recordSeq);
   const size_t states = m_state.size();
   RecordMessage(MSG_KEYFRAME, m_payload.data(), m_payload.size());
   m_log.Info(Fmt("recording: keyframe %zu bytes, state %zu bytes, %zu calls", m_payload.size(), states, m_stored.size()));
}

void SessionImpl::RecordMessage(const uint8_t type, const uint8_t* const data, const size_t size)
{
   if (m_recorder->Write(type, data, size))
      return;
   m_recorder->Close();
   const std::string why = m_recorder->Error();
   m_recorder.reset();
   m_log.Error(Fmt("recording stopped after %u frames: %s (%s)", m_recordSeq, why.c_str(), m_recordPath.c_str()));
   Note("Castcade: recording stopped: " + why, NOTE_LASTING_MS);
}

void SessionImpl::CloseRecording()
{
   const uint64_t frames = m_recordSeq, bytes = m_recorder->FileBytes(), usec = m_recorder->DurationUsec(), chunks = m_recorder->Chunks();
   m_recorder->Close();
   const std::string err = m_recorder->Error();
   m_recorder.reset();
   if (chunks == 0)
   {
      m_log.Info("recording: no frame was rendered, nothing recorded (" + m_recordPath + " removed)");
      return;
   }
   const double minutes = (double)usec / 60e6;
   m_log.Info(Fmt("recording closed: %s: %llu frames, %llu messages, %.1f s, %.2f MB, %.2f MB per minute%s%s", m_recordPath.c_str(), (unsigned long long)frames,
      (unsigned long long)chunks, (double)usec / 1e6, (double)bytes / 1e6, minutes > 0. ? (double)bytes / 1e6 / minutes : 0., err.empty() ? "" : "; ",
      err.c_str()));
}

// ---------------------------------------------------------------------------------------------------------
// Watch role

bool SessionImpl::ParseFrame(const Message& m, RxFrame& f)
{
   ByteReader r(m.payload.data(), m.payload.size());
   f.bytes = m.payload.size();
   f.time = r.u64();
   f.seq = r.u32();
   size_t n = 0;
   const uint8_t* const state = r.blob(n);
   if (!r.ok())
      return false;
   f.stateBytes = n;
   ByteReader sr(state, n);
   f.state = ParseState(sr);
   if (!f.state)
      return false;
   const uint32_t count = r.u32();
   if (!r.ok() || count > r.remaining() / 8) // a call is at least 8 bytes
      return false;
   f.calls.reserve(count);
   for (uint32_t i = 0; i < count; ++i)
   {
      RxCall c;
      c.obj = r.varu();
      c.dispid = r.i32();
      c.kind = r.u8();
      c.flags = r.u8();
      c.argc = r.u8();
      c.argStart = (uint32_t)f.args.size();
      if (!r.ok() || c.kind > CALL_METHOD || (c.kind != CALL_METHOD && c.argc == 0))
         return false;
      for (uint8_t a = 0; a < c.argc; ++a)
      {
         f.args.emplace_back();
         VariantInit(&f.args.back());
         if (!DecodeVariant(r, f.args.back()))
            return false;
      }
      f.calls.push_back(c);
   }
   return r.ok();
}

IDispatch* SessionImpl::Resolve(const uint32_t id, KindClass& cls) const
{
   if (id & ID_BALL)
   {
      cls = KC_BALL;
      Ball* const b = m_applier->FindBall(id & ~ID_BALL);
      return (b && b->m_pballex) ? b->m_pballex->GetDispatch() : nullptr;
   }
   if (id == ID_TABLE)
   {
      cls = KC_TABLE;
      return m_tableDisp;
   }
   if (id == ID_GLOBAL)
   {
      cls = KC_GLOBAL;
      return m_globalDisp;
   }
   cls = KC_EDITABLE;
   return id < m_editDisp.size() ? m_editDisp[id] : nullptr;
}

// Replays the frame's calls through the objects' own Invoke, as the player's script made them. g_recordCalls is
// false on a watcher, so nothing here is recorded again. A failure is counted and never stops the stream.
void SessionImpl::Replay(RxFrame& f)
{
   f.replayed = true;
   for (const RxCall& c : f.calls)
   {
      if ((c.flags & CALLFLAG_SOUND) && !m_replaySound)
      {
         ++m_iSoundSkipped;
         continue;
      }
      // The player's StopSoundFilter, applied again: it thins streams recorded before the player applied it, and
      // drops nothing from a stream the player already filtered (same rule, same calls, same order)
      if (c.obj == ID_GLOBAL && c.kind == CALL_METHOD && m_stopFilterOn && c.argc >= 1 && (c.dispid == m_dispStopSound || c.dispid == m_dispPlaySound)
         && SoundKey(f.args[c.argStart + c.argc - 1], m_soundKey))
      {
         if (c.dispid == m_dispPlaySound)
            m_stopFilter.OnPlay(m_soundKey);
         else if (!m_stopFilter.Wanted(m_soundKey))
         {
            if (!m_stopAudit)
            {
               ++m_iStopDropped;
               ++m_stopDroppedTotal;
               continue;
            }
            // Audit: replayed anyway, after checking whether it would have stopped anything that plays here now
            if (StopSoundWouldStop(m_player, m_soundKey))
            {
               ++m_iAuditEffective;
               ++m_auditEffectiveTotal;
               if (m_auditEffectiveTotal <= MAX_LOGGED)
                  m_log.Warn("StopSound audit: the filter would have dropped a stop of '" + Narrow(m_soundKey.c_str()) + "' while that sound was playing here");
            }
            else
            {
               ++m_iAuditNoEffect;
               ++m_auditNoEffectTotal;
            }
         }
      }
      KindClass cls;
      IDispatch* const target = Resolve(c.obj, cls);
      if (!target)
      {
         ++m_iUnresolved;
         if (m_replayFailLogged < MAX_LOGGED)
         {
            ++m_replayFailLogged;
            m_log.Warn(Fmt("replay: object %08x (%s) not found, dispid %d", c.obj, kKindClassNames[cls], (int)c.dispid));
         }
         continue;
      }
      // Match role: the value about to be replaced is read first, so that this table's own can be put back
      if (m_observer)
         m_observer->BeforeCall(c, c.argc ? &f.args[c.argStart] : nullptr, cls, target);
      DISPPARAMS dp;
      dp.rgvarg = c.argc ? &f.args[c.argStart] : nullptr;
      dp.cArgs = c.argc;
      WORD wflags;
      if (c.kind == CALL_METHOD)
      {
         dp.cNamedArgs = 0;
         dp.rgdispidNamedArgs = nullptr;
         wflags = DISPATCH_METHOD;
      }
      else
      {
         dp.cNamedArgs = 1;
         dp.rgdispidNamedArgs = const_cast<DISPID*>(&m_dispidPut);
         wflags = c.kind == CALL_PUTREF ? DISPATCH_PROPERTYPUTREF : DISPATCH_PROPERTYPUT;
      }
      VARIANT result;
      VariantInit(&result);
      EXCEPINFO ei = {};
      UINT argErr = 0;
      const HRESULT hr = target->Invoke(c.dispid, IID_NULL, LOCALE_USER_DEFAULT, wflags, &dp, c.kind == CALL_METHOD ? &result : nullptr, &ei, &argErr);
      VariantClear(&result);
      if (ei.bstrSource)
         SysFreeString(ei.bstrSource);
      if (ei.bstrDescription)
         SysFreeString(ei.bstrDescription);
      if (ei.bstrHelpFile)
         SysFreeString(ei.bstrHelpFile);
      if (FAILED(hr))
      {
         ++m_iFailed;
         if (m_replayFailLogged < MAX_LOGGED)
         {
            ++m_replayFailLogged;
            m_log.Warn(Fmt("replay failed: object %08x (%s), %s (dispid %d), kind %u, %u args, HRESULT 0x%08lx", c.obj, kKindClassNames[cls],
               MemberName(target, c.dispid).c_str(), (int)c.dispid, c.kind, c.argc, (unsigned long)hr));
         }
         continue;
      }
      ++m_iReplayed[cls];
      ++m_frameReplayed;
   }
}

void SessionImpl::ApplyKeyframe(RxFrame& kf)
{
   m_applier->Reset();
   m_timeline.clear();
   m_haveShown = m_held = m_catchup = m_epOpen = false;
   // Match role: a keyframe builds on the table as its file gives it, which a table that has played its own game
   // is not: the match session brings it there first (and hides the local balls)
   if (m_observer)
      m_observer->BeforeKeyframe(kf);
   m_applier->Apply(*kf.state, *kf.state, 1.f);
   const uint64_t failedBefore = m_iFailed + m_iUnresolved;
   const uint32_t replayedBefore = m_frameReplayed;
   Replay(kf);
   if (m_staticRefresh)
      MarkStaticPrepassDirty(m_player); // the desktop static pre-render is rebuilt from the replicated state
   m_keyframeApplied = true;
   m_log.Info(Fmt("keyframe applied: %zu bytes, state %zu bytes, %zu calls (%u replayed, %llu failed or unresolved)", kf.bytes, kf.stateBytes, kf.calls.size(),
      m_frameReplayed - replayedBefore, (unsigned long long)(m_iFailed + m_iUnresolved - failedBefore)));
}

void SessionImpl::AddOffsetSample(const int64_t sample)
{
   m_offsetSamples.push_back(sample);
   if (m_offsetSamples.size() > OFFSET_WINDOW)
      m_offsetSamples.pop_front();
   m_offset = *std::min_element(m_offsetSamples.begin(), m_offsetSamples.end());
}

void SessionImpl::Refuse(const std::string& why)
{
   m_log.Error("refusing the stream: " + why);
   Note("Castcade: this is not the same table file as the player's", NOTE_LASTING_MS);
   m_refused = true;
   m_listener.reset();
}

// PinTable::StopAllSounds: every sound's own buffer (a WAV's DirectSound buffer, or its BASS stream) and, through
// AudioMusicPlayer::StopCopiedWavs, every copied buffer. The watcher's StopSoundFilter starts again with it.
void SessionImpl::StopAllSoundsHere(const char* const why)
{
   if (!m_watch || !m_player->m_ptable)
      return;
   // Match role with the stream's sound off (what a match runs with: the companion carries the sound): nothing the
   // stream does plays here, so every sound that plays is this table's own, and stays
   if (m_inner && !m_replaySound)
      return;
   const int before = CountPlayingSounds(m_player);
   m_player->m_ptable->StopAllSounds();
   const int after = CountPlayingSounds(m_player);
   std::vector<std::wstring> reels;
   ReelSoundNames(m_player, reels);
   m_stopFilter.Init(reels);
   m_log.Info(Fmt("%s: every sound stopped (%d were playing, %d play now)", why, before, after));
}

void SessionImpl::WatchService(Peer& p, const uint64_t now)
{
   m_msgs.clear();
   const bool alive = p.tcp->Poll(m_msgs);
   if (p.tcp->Connected() && !p.wasConnected)
   {
      p.wasConnected = true;
      if (p.outgoing)
         m_log.Info("connected to " + p.addr);
   }
   for (const Message& m : m_msgs)
   {
      if (p.closing)
         break;
      if (m.type == MSG_HELLO && !p.helloOk)
      {
         Hello h;
         std::string why;
         if (!ParseHello(m, h))
         {
            m_log.Warn("malformed HELLO from " + p.addr);
            p.closing = true;
         }
         else if (!CheckHello(h, why))
         {
            if (h.version == PROTOCOL_VERSION && h.role == ROLE_PLAY)
               Refuse(why); // the tables differ: never retry
            else
               m_log.Warn(Fmt("refusing %s: %s", p.addr.c_str(), why.c_str()));
            p.closing = true;
         }
         else
         {
            p.helloOk = true;
            m_bye = false;
            m_offsetSamples.clear();
            if (h.build != VP_VERSION_STRING_POINTS_FULL)
               m_log.Warn(Fmt("the player runs build %s, this is %s", h.build.c_str(), VP_VERSION_STRING_POINTS_FULL));
            m_physicsStamps = (h.features & HELLO_PHYSICS_STAMPS) != 0;
            m_haveLastRx = m_lastRxCorrected = false;
            m_log.Info(Fmt("watching '%s' from %s; its frames are stamped %s", h.table.c_str(), p.addr.c_str(),
               m_physicsStamps ? "with its physics clock (never corrected)" : "when sent (an older player or recording: late stamps are corrected)"));
            Note("Castcade: watching " + h.table);
            // A new player, or a new session of the same one, starts from silence: the player's StopSound rules only
            // have to be right for sounds started on this connection
            StopAllSoundsHere("a new player's HELLO");
            m_stopWhenDrained = false;
         }
      }
      else if ((m.type == MSG_KEYFRAME || m.type == MSG_FRAME) && p.helloOk)
      {
         if (m.type == MSG_FRAME && !m_keyframeApplied)
         {
            ++m_iBeforeKeyframe;
            continue;
         }
         auto f = std::make_unique<RxFrame>();
         if (!ParseFrame(m, *f))
         {
            m_log.Warn(Fmt("malformed %s (%zu bytes) from %s: dropping the connection", m.type == MSG_FRAME ? "FRAME" : "KEYFRAME", m.payload.size(), p.addr.c_str()));
            p.closing = true;
            break;
         }
         // A player before the physics-clock stamps (recordings made before 2026-11-16 included) stamped a frame
         // when it was sent: after a stall in the frame's script timers, the frame that follows carries the physics
         // of before the stall under a stamp of after it, and the stall's motion is crammed into the next frame
         // interval. Such a frame is moved back to where its physics time puts it. Frames of a current player, and
         // every frame without a stall, are stamped within a few ms of their physics time and are never moved.
         // A current player says so in its HELLO (HELLO_PHYSICS_STAMPS) and its frames are never looked at: its stamp
         // legitimately jumps ahead of the snapshot's physics time when it resumes from a pause (Player::m_noTimeCorrect
         // moves the physics clock and its start together). For an older stream, a frame that needs moving right after
         // a moved frame while its own physics time advanced by a normal frame interval only (nothing was caught up)
         // is a re-based clock, not a stall: its stamp is taken as sent and the next frames compare with it.
         // Part of smoothing: CASTCADE_STREAM_SMOOTH=0 shows an older stream exactly as the watcher always did.
         f->arrived = now;
         if (m_held && m_lastAnyArrival)
            m_holdArrivalGapMax = (std::max)(m_holdArrivalGapMax, now - m_lastAnyArrival);
         m_lastAnyArrival = now;
         const uint32_t phys = SnapshotPhysicsMsec(*f->state);
         bool corrected = false;
         if (m_smooth && !m_physicsStamps && m.type == MSG_FRAME && m_haveLastRx && phys >= m_lastRxPhys && f->time > m_lastRxTime)
         {
            const uint64_t byPhysics = m_lastRxTime + (uint64_t)(phys - m_lastRxPhys) * 1000;
            if (f->time > byPhysics + 40000)
            {
               if (m_lastRxCorrected && phys - m_lastRxPhys <= 40)
                  ++m_iRebased;
               else
               {
                  f->time = byPhysics;
                  corrected = true;
                  ++m_iRestamped;
               }
            }
         }
         m_lastRxCorrected = corrected;
         m_lastRxTime = f->time;
         m_lastRxPhys = phys;
         m_haveLastRx = true;
         AddOffsetSample((int64_t)now - (int64_t)f->time);
         if (m.type == MSG_KEYFRAME)
            ApplyKeyframe(*f);
         else
         {
            ++m_iRecv;
            m_timeline.push_back(std::move(f));
         }
      }
      else if (m.type == MSG_BYE)
      {
         m_bye = true;
         m_log.Info("the player closed the table");
         Note("Castcade: the player closed the table");
         p.closing = true;
      }
      else
      {
         m_log.Warn(Fmt("unexpected message type %u from %s: dropping the connection", m.type, p.addr.c_str()));
         p.closing = true;
      }
   }
   if (alive && !p.closing)
      return;

   AccountBytes(p);
   if (p.helloOk && !m_bye && !m_refused)
   {
      m_log.Warn("connection lost (" + p.addr + ")" + (m_listen ? ", waiting for the player" : ", retrying"));
      Note(m_listen ? "Castcade: connection lost, waiting for the player" : "Castcade: connection lost, retrying");
   }
   else if (p.outgoing && !p.wasConnected)
   {
      if (now - m_lastConnectFailLog >= CONNECT_FAIL_LOG_USEC)
      {
         m_lastConnectFailLog = now;
         m_log.Info("cannot reach " + p.addr + " yet, retrying every 2 s");
      }
   }
   else if (!p.helloOk && !m_refused && !p.closing)
      m_log.Info(Fmt("connection with %s closed before its HELLO", p.addr.c_str()));
   // Whatever the player stops from now on, this watcher does not hear it: nothing it started may go on playing (a
   // looping sound would loop until the next stop the player's rules let through)
   if (p.helloOk)
   {
      StopAllSoundsHere(m_bye ? "the player closed the table" : "the connection ended");
      m_stopWhenDrained = true; // the frames already received are still shown (the delay), and may start sounds
   }
   p.tcp->Close();
   p.closing = true;
   m_keyframeApplied = false; // the next connection starts with a keyframe; the last picture stays meanwhile
}

// When the player stalls, its frames stop for the length of the stall and then come all at once (a catch-up sends
// the states it simulated meanwhile, CastcadeStream OnPhysicsStep). The watcher shows a fixed delay behind the
// newest frame, so it holds the last picture for the stall minus the delay whatever it does (a guess forward would be
// wrong at the first thing the ball hits). What smoothing changes is the moment frames come back: without it the
// shown time jumps to the target and the ball leaps over everything it did during the hold; with it the shown time
// carries on from where it held and catches up with the target over 80 to 300 ms (half the lag), eased at both
// ends, so the missed motion is played quickly instead of skipped. With no hold of more than CATCHUP_MIN_LAG_USEC the
// shown time is the target, exactly as without smoothing; calls are replayed once each and in order either way.
void SessionImpl::StepTimeline(const uint64_t now)
{
   if (m_timeline.empty())
      return;
   const int64_t target = (int64_t)now - m_offset - m_delayUsec;
   int64_t render = target;

   // A stalled watcher (or a stale clock offset) lets the timeline grow: jump to the newest minus the delay
   const int64_t newest = (int64_t)m_timeline.back()->time;
   bool jumped = false;
   if (newest - (int64_t)m_timeline.front()->time > (int64_t)TIMELINE_MAX_USEC && newest - m_delayUsec > render)
   {
      render = newest - m_delayUsec;
      jumped = true;
      m_catchup = false;
      if (m_iJumps++ < 3)
         m_log.Info(Fmt("timeline over 2 s (%zu frames): jumping to the newest frame minus the delay", m_timeline.size()));
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
      // A gap is frames that stopped arriving: the longest time without one while the picture held (WatchService
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
   for (size_t i = 0; i < m_timeline.size(); ++i)
   {
      if ((int64_t)m_timeline[i]->time <= render)
         ia = i;
      else
         break;
   }
   if (ia == SIZE_MAX)
   {
      ++m_iBuffering; // the first frames after a keyframe are still younger than the delay
      return;
   }
   RxFrame& a = *m_timeline[ia];
   RxFrame* const b = ia + 1 < m_timeline.size() ? m_timeline[ia + 1].get() : nullptr;
   if (b && b->time > a.time)
   {
      const float t = std::clamp((float)((double)(render - (int64_t)a.time) / (double)(b->time - a.time)), 0.f, 1.f);
      m_applier->Apply(*a.state, *b->state, t);
   }
   else
   {
      m_applier->Apply(*a.state, *a.state, 0.f);
      if (!b && !m_peers.empty() && m_peers.front().helloOk)
         ++m_iStarved;
   }
   // How far the picture moved in this frame: the gap being measured, or the reference of normal play
   const float step = m_applier->LastMaxBallStep();
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
      if (!m_timeline[i]->replayed)
         Replay(*m_timeline[i]);
   for (size_t i = 0; i < ia; ++i)
      m_timeline.pop_front();
}

void SessionImpl::LogGap()
{
   m_epOpen = false;
   ++m_iGaps;
   ++m_gapsTotal;
   const std::string how = m_epCatchupMs > 0. ? Fmt("caught up in %.0f ms", m_epCatchupMs) : m_smooth ? std::string("no catch-up (lag under 20 ms)") : std::string("jumped (smoothing off)");
   const std::string move = m_epFirstMoveMs >= 0. ? Fmt("a ball moved again %.0f ms after the hold began", m_epFirstMoveMs) : std::string("no ball moved");
   m_log.Info(Fmt("stream gap: frames stopped arriving for %.0f ms, picture held %.0f ms, %.0f ms behind when frames came back, %s; %s; largest ball step %.1f VPU in one frame",
      m_epGapMs, m_epHeldMs, m_epLagMs, how.c_str(), move.c_str(), m_epMaxStep));
}

std::string SessionImpl::StepSummary() const
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
   return Fmt("frames %llu, median %.1f, p99 %.1f, p99.9 %.1f, max %.1f VPU", (unsigned long long)m_stepFrames, pct(0.5), pct(0.99), pct(0.999), m_stepMax);
}

void SessionImpl::WatchFrame(const uint64_t now)
{
   if (!m_refused && m_wsa)
   {
      if (m_listener)
         while (std::unique_ptr<TcpPeer> t = m_listener->Accept())
         {
            // One player at a time: a new connection replaces the current one
            if (!m_peers.empty())
            {
               m_log.Info(Fmt("connection from %s replaces %s", t->RemoteAddress().c_str(), m_peers.front().addr.c_str()));
               AccountBytes(m_peers.front());
               m_peers.clear();
               m_keyframeApplied = false;
            }
            Peer p;
            p.addr = t->RemoteAddress();
            p.tcp = std::move(t);
            m_log.Info("connection from " + p.addr);
            SendHello(p);
            m_peers.push_back(std::move(p));
         }
      if (!m_listen && m_peers.empty() && now >= m_nextRetry)
      {
         m_nextRetry = now + RETRY_USEC;
         if (!m_connectNoted)
         {
            m_connectNoted = true;
            m_log.Info("connecting to " + m_connectText);
            Note("Castcade: connecting to " + m_connectText);
         }
         std::string err;
         std::unique_ptr<TcpPeer> t = ConnectTcp(m_connectHost, m_port, err);
         if (t)
         {
            Peer p;
            p.addr = m_connectText;
            p.outgoing = true;
            p.tcp = std::move(t);
            SendHello(p);
            m_peers.push_back(std::move(p));
         }
         else if (now - m_lastConnectFailLog >= CONNECT_FAIL_LOG_USEC)
         {
            m_lastConnectFailLog = now;
            m_log.Warn("cannot connect to " + m_connectText + ": " + err);
         }
      }
      // Test: CASTCADE_TEST_DROP=<s>[,<s>...] cuts the connection that many seconds after the session began, as a
      // dropped line would; the watcher reconnects after its usual 2 s retry interval (listening: when the player does)
      if (!m_testDrops.empty() && now - m_qpc0 >= (uint64_t)(m_testDrops.front() * 1e6))
      {
         m_testDrops.erase(m_testDrops.begin());
         if (!m_peers.empty() && m_peers.front().tcp->Alive())
         {
            m_log.Info("test drop: cutting the connection now (CASTCADE_TEST_DROP)");
            m_peers.front().tcp->Close();
            m_nextRetry = now + RETRY_USEC;
         }
      }
      if (!m_peers.empty())
      {
         WatchService(m_peers.front(), now);
         if (m_peers.front().closing)
            m_peers.clear();
      }
   }

   m_frameReplayed = 0;
   StepTimeline(now);
   // Once the frames received before the connection ended have all been shown, whatever they started stops too
   if (m_stopWhenDrained && (m_peers.empty() || !m_peers.front().helloOk)
      && std::none_of(m_timeline.begin(), m_timeline.end(), [](const std::unique_ptr<RxFrame>& f) { return !f->replayed; }))
   {
      m_stopWhenDrained = false;
      StopAllSoundsHere("the frames received before the connection ended have been shown");
   }
   m_iReplayedSum += m_frameReplayed;
   m_iReplayedMax = (std::max)(m_iReplayedMax, m_frameReplayed);
}

// ---------------------------------------------------------------------------------------------------------
// Both roles

void SessionImpl::OnFrame()
{
   const uint64_t now = NowUsec();
   ++m_iFrames;
   if (m_watch)
      WatchFrame(now);
   else
      PlayFrame(now);
   FlushNotes();
   if (now - m_statsStart >= STATS_USEC)
      LogStats(NowUsec());
}

// m_liveUI only exists once the script has started (Player::Init), so notifications wait for frame time
void SessionImpl::FlushNotes()
{
   if (m_notes.empty() || !m_player->m_liveUI)
      return;
   for (const auto& n : m_notes)
      m_player->m_liveUI->PushNotification(n.first, n.second);
   m_notes.clear();
}

void SessionImpl::LogStats(const uint64_t now)
{
   for (Peer& p : m_peers)
      AccountBytes(p);
   const double secs = (double)(now - m_statsStart) / 1e6;
   const double frames = m_iFrames ? (double)m_iFrames : 1.;
   if (!m_watch)
   {
      // OnScriptCall cost: rdtsc ticks, converted with the TSC rate measured against QPC since the session began
      const double tscPerUsec = (double)(__rdtsc() - m_tsc0) / (double)((now - m_qpc0) ? (now - m_qpc0) : 1);
      const double usAvg = (double)m_iTicks / frames / tscPerUsec, usMax = (double)m_iTicksMax / tscPerUsec;
      const size_t watchers = (size_t)std::count_if(m_peers.begin(), m_peers.end(), [](const Peer& p) { return p.helloOk; });
      m_log.Info(Fmt("stats %.1fs: watchers %zu, frames %llu, sent %llu (%.1f/s), out %.0f B/s, in %.0f B/s; script calls/frame avg %.1f max %u, by kind "
                     "editable %llu table %llu global %llu ball %llu; sent calls/frame avg %.2f max %u, by kind editable %llu table %llu global %llu ball %llu; "
                     "dedupe-dropped %llu, policy-ignored %llu, unencodable %llu, unknown-object %llu, StopSound not sent %llu; OnScriptCall us/frame avg %.1f max %.1f; stored %zu; "
                     "catch-ups %llu sending %llu frames (%.0f us in all)",
         secs, watchers, (unsigned long long)m_iFrames, (unsigned long long)m_iFramesSent, (double)m_iFramesSent / secs, (double)m_iBytesOut / secs,
         (double)m_iBytesIn / secs, (double)m_iSeen / frames, m_iSeenMax, (unsigned long long)m_seenByClass[KC_EDITABLE],
         (unsigned long long)m_seenByClass[KC_TABLE], (unsigned long long)m_seenByClass[KC_GLOBAL], (unsigned long long)m_seenByClass[KC_BALL],
         (double)m_iQueued / frames, m_iQueuedMax, (unsigned long long)m_queuedByClass[KC_EDITABLE], (unsigned long long)m_queuedByClass[KC_TABLE],
         (unsigned long long)m_queuedByClass[KC_GLOBAL], (unsigned long long)m_queuedByClass[KC_BALL], (unsigned long long)m_iDedupe,
         (unsigned long long)m_iIgnored, (unsigned long long)m_iUnencodable, (unsigned long long)m_iUnknown, (unsigned long long)m_iStopDropped, usAvg, usMax,
         m_stored.size(), (unsigned long long)m_iCatchups, (unsigned long long)m_iCatchupFrames, (double)m_iCatchupTicks / tscPerUsec));
      m_iCatchups = m_iCatchupFrames = m_iCatchupTicks = 0;
      if (m_recorder)
      {
         const uint64_t bytes = m_recorder->FileBytes();
         const double recFrames = m_recFrames ? (double)m_recFrames : 1.;
         m_log.Info(Fmt("recording %.1fs: frames %llu (%llu in all), file %.2f MB, %.1f KB/s; cost on the render thread us/frame avg %.1f max %.1f",
            secs, (unsigned long long)m_recFrames, (unsigned long long)m_recordSeq, (double)bytes / 1e6, (double)(bytes - m_recBytesAtStats) / 1e3 / secs,
            (double)m_recTicks / recFrames / tscPerUsec, (double)m_recTicksMax / tscPerUsec));
         m_recBytesAtStats = bytes;
      }
      m_recFrames = m_recTicks = m_recTicksMax = 0;
      m_iSeen = m_iQueued = m_iTicks = 0;
      m_iSeenMax = m_iQueuedMax = 0;
      m_iTicksMax = 0;
      m_iDedupe = m_iIgnored = m_iUnencodable = m_iUnknown = m_iFramesSent = m_iStopDropped = 0;
      for (int i = 0; i < KC_COUNT; ++i)
         m_seenByClass[i] = m_queuedByClass[i] = 0;
      if (now - m_lastNamesLog >= 30000000)
      {
         m_lastNamesLog = now;
         LogSentByName("since the session began");
      }
   }
   else
   {
      const bool connected = !m_peers.empty() && m_peers.front().helloOk;
      const double span = m_timeline.size() > 1 ? (double)(m_timeline.back()->time - m_timeline.front()->time) / 1000. : 0.;
      m_log.Info(Fmt("stats %.1fs: %s, frames %llu, received %llu (%.1f/s), in %.0f B/s, out %.0f B/s; timeline %zu frames (%.0f ms), starved %llu, buffering %llu, "
                     "jumps %llu; replayed calls %llu (editable %llu table %llu global %llu ball %llu), per frame avg %.2f max %u, failed %llu, unresolved %llu, "
                     "sound skipped %llu, stops skipped %llu%s; frames before keyframe %llu; clock offset %lld us, delay %lld ms; gaps %llu, largest ball step %.1f VPU; "
                     "frames restamped so far %llu, re-based %llu",
         secs, m_refused ? "refused" : (connected ? "connected" : "not connected"), (unsigned long long)m_iFrames, (unsigned long long)m_iRecv,
         (double)m_iRecv / secs, (double)m_iBytesIn / secs, (double)m_iBytesOut / secs, m_timeline.size(), span, (unsigned long long)m_iStarved,
         (unsigned long long)m_iBuffering, (unsigned long long)m_iJumps,
         (unsigned long long)(m_iReplayed[KC_EDITABLE] + m_iReplayed[KC_TABLE] + m_iReplayed[KC_GLOBAL] + m_iReplayed[KC_BALL]),
         (unsigned long long)m_iReplayed[KC_EDITABLE], (unsigned long long)m_iReplayed[KC_TABLE], (unsigned long long)m_iReplayed[KC_GLOBAL],
         (unsigned long long)m_iReplayed[KC_BALL], (double)m_iReplayedSum / frames, m_iReplayedMax, (unsigned long long)m_iFailed,
         (unsigned long long)m_iUnresolved, (unsigned long long)m_iSoundSkipped, (unsigned long long)m_iStopDropped,
         m_stopAudit ? Fmt(" (audit: %llu would have stopped a playing sound, %llu would not)", (unsigned long long)m_iAuditEffective, (unsigned long long)m_iAuditNoEffect).c_str() : "",
         (unsigned long long)m_iBeforeKeyframe, (long long)m_offset, (long long)(m_delayUsec / 1000), (unsigned long long)m_iGaps, m_iMaxStep,
         (unsigned long long)m_iRestamped, (unsigned long long)m_iRebased));
      m_iGaps = 0;
      m_iMaxStep = 0.f;
      m_iRecv = m_iBeforeKeyframe = m_iStarved = m_iBuffering = m_iJumps = 0;
      m_iFailed = m_iUnresolved = m_iSoundSkipped = m_iStopDropped = m_iAuditEffective = m_iAuditNoEffect = 0;
      m_iReplayedSum = 0;
      m_iReplayedMax = 0;
      for (int i = 0; i < KC_COUNT; ++i)
         m_iReplayed[i] = 0;
   }
   m_iFrames = 0;
   m_iBytesOut = m_iBytesIn = 0;
   m_statsStart = now;
}

// ---------------------------------------------------------------------------------------------------------
// Match role: a play session that can be sent to watch another player and called back (CastcadeStream.h).
//
// Playing is the play role, unchanged (m_play). A watch phase puts a watch session (m_watch, today's watch role
// with its address given by the "watch" command) on top of the local table, which has played its own game:
//
//   Entering. The table's Paused event fires (a ROM table pauses VPinMAME in it), then no event reaches the script
//   any more (g_holdEvents: timers, hits, keys, animation events), Player skips the physics and the timers as it
//   does for a watcher (IsWatcher), and nothing is recorded. The local table is not touched until the other
//   player's keyframe has arrived: a watch that never connects costs the player nothing but the pause.
//
//   The keyframe. A keyframe builds on the table as its file gives it. So first: the local balls are hidden (not
//   destroyed, the script holds them), the state no property returns (TableRuntime) goes to what it was before
//   the script started, and every property this table's script has put that the keyframe does not put goes to its
//   file value (SessionImpl::BaselineBefore read it). Then the keyframe as on any watcher.
//
//   The undo list. Before any put reaches a local object in a watch phase (those of the step above included), the
//   first time for that (object, property, index arguments), its value is read into m_undo. Where this table's own
//   script last put that same value, the script's own argument is kept instead of the getter's (a getter may
//   convert units; the put of the script's argument is the computation that made the state). A property with no
//   getter (the DMD's pixels) is put back from the play part's stored calls. Materials changed by UpdateMaterial or
//   MaterialColor are copied whole before the first change.
//
//   Leaving. The watch session ends (its balls are destroyed). The movers are put back first, so that a put's own
//   side effect sees this table's angles; then the undo list, newest first; then the TableRuntime and the physics
//   state saved at the start, the balls, the ball id counter, the music. The game clock goes back to where it stood
//   (EndWatchPhase), recording resumes, events flow again, keys that changed while watching reach the script, the
//   UnPaused event fires, and "state playing" goes out.
//
// Every way out of a watch phase is LeaveWatch: the "play" command, a refused stream, no stream within 4 s of a
// "watch", a stream that was shown and then delivers no frame for 3 s (open: "stalled"; closed: "lost"), the player
// closing its table, a lost control connection, an unknown command, this table closing.
//
// What the companion counts on (castcade repo, docs/design/state-stream-in-match.md §3, rules 1 to 12): hello and
// "state playing" are sent as soon as the connection exists, before anything is read; hello's last field is the port
// this table's stream listener is bound to; every "watch", a re-aim included, is answered within 5 s by "state
// watching" or "state failed <reason>"; a "watch" while watching always drops the stream and connects again, even
// to the same address, and no "state playing" is sent between the two; after a "state failed" this table is back by
// itself and says "state playing"; a "play" while playing is answered "state playing"; a "watch" right behind a
// "play" works; each token of a line is printable ASCII without spaces. And the hand-over: a watcher that
// connects to this table's stream while this table is not playing is accepted and held, with nothing sent, until it
// plays (10 s at most): the play part does that (SessionImpl::PlayFrame, m_frozen).

constexpr uint64_t WATCH_FIRST_USEC = 4000000; // from a "watch" to its keyframe: the companion waits 5 s for the answer
constexpr uint64_t WATCH_STALL_USEC = 3000000; // a stream that was shown: without a frame, or without a connection, this long

// One token of a control line: printable ASCII, no space, at most 128 characters
std::string Token(const std::string& s)
{
   std::string t = s.substr(0, 128);
   for (char& c : t)
      if (c <= ' ' || c > '~')
         c = '_';
   return t.empty() ? std::string("_") : t;
}

bool IsNumber(const VARTYPE vt)
{
   switch (vt)
   {
   case VT_I1: case VT_I2: case VT_I4: case VT_I8: case VT_INT:
   case VT_UI1: case VT_UI2: case VT_UI4: case VT_UI8: case VT_UINT:
   case VT_R4: case VT_R8: case VT_CY: case VT_DATE: return true;
   default: return false;
   }
}

// Whether two property values are the same: numbers within the tolerance (relative, absolute below 1), booleans and
// strings exactly. Anything else is never the same.
bool SameValue(const VARIANT& a, const VARIANT& b, const double tolerance)
{
   if (IsNumber(V_VT(&a)) && IsNumber(V_VT(&b)))
   {
      VARIANT x, y;
      VariantInit(&x);
      VariantInit(&y);
      if (FAILED(VariantChangeType(&x, const_cast<VARIANT*>(&a), 0, VT_R8)) || FAILED(VariantChangeType(&y, const_cast<VARIANT*>(&b), 0, VT_R8)))
         return false;
      const double p = V_R8(&x), q = V_R8(&y);
      return fabs(p - q) <= tolerance * (std::max)(1.0, (std::max)(fabs(p), fabs(q)));
   }
   if (V_VT(&a) == VT_BOOL && V_VT(&b) == VT_BOOL)
      return (V_BOOL(&a) != 0) == (V_BOOL(&b) != 0);
   if (V_VT(&a) == VT_BSTR && V_VT(&b) == VT_BSTR)
   {
      const UINT n = SysStringLen(V_BSTR(&a));
      return n == SysStringLen(V_BSTR(&b)) && (n == 0 || memcmp(V_BSTR(&a), V_BSTR(&b), n * sizeof(OLECHAR)) == 0);
   }
   return false;
}

std::string VarText(const VARIANT& v)
{
   if (V_VT(&v) & VT_ARRAY)
      return "(array)";
   VARIANT s;
   VariantInit(&s);
   if (FAILED(VariantChangeType(&s, const_cast<VARIANT*>(&v), 0, VT_BSTR)))
      return Fmt("(vt %u)", (unsigned)V_VT(&v));
   const std::string text = V_VT(&v) == VT_BSTR ? "'" + Narrow(V_BSTR(&s)) + "'" : Narrow(V_BSTR(&s));
   VariantClear(&s);
   return text;
}

class MatchSession final : public Session, private ReplayObserver
{
public:
   explicit MatchSession(Player* const player);
   ~MatchSession() override;

   bool IsWatcher() const override { return m_watching; }
   bool NeverPauses() const override { return m_watching || m_play.NeverPauses(); }
   // While watching, the balls that come and go are the watch session's
   void OnBallCreated(Ball* const ball) override
   {
      if (!m_watching)
         m_play.OnBallCreated(ball);
   }
   void OnBallDestroyed(Ball* const ball) override
   {
      if (!m_watching)
         m_play.OnBallDestroyed(ball);
   }
   void OnFrame() override;
   void OnPhysicsStep(const unsigned long long targetUsec) override
   {
      if (!m_watching)
         m_play.OnPhysicsStep(targetUsec);
   }
   bool OnKeyEvent(const int dispid, const int keycode) override;

private:
   struct Undo
   {
      std::string key;
      uint32_t obj = 0;
      DISPID dispid = 0;
      uint8_t kind = CALL_PUT;
      bool readable = false;  // args.v[0] holds the value to put back
      bool fromStore = false; // ... and it is this table's own stored argument
      VarList args;
   };
   struct CheckProp
   {
      uint32_t obj;
      IDispatch* disp;
      MEMBERID id;
   };

   // ReplayObserver
   void BeforeKeyframe(const RxFrame& kf) override;
   void BeforeCall(const RxCall& c, const VARIANT* const args, const KindClass cls, IDispatch* const target) override;

   void ServiceControl(const uint64_t now);
   void Command(const std::string& line, const uint64_t now);
   void Send(const std::string& line);
   void EnterWatch(const std::string& host, const uint16_t port, const uint64_t now);
   void StartInner(const std::string& host, const uint16_t port);
   void CheckWatch(const uint64_t now);
   void LeaveWatch(const char* const why, const char* const failed);

   IDispatch* ResolveOwn(const uint32_t id) const;
   bool MakeKey(const uint32_t obj, const DISPID dispid, const VARIANT* const args, const size_t argc, std::string& key);
   void Capture(const std::string& key, const uint32_t obj, const DISPID dispid, const uint8_t kind, const VARIANT* const args, const size_t argc, IDispatch* const target);
   HRESULT Put(IDispatch* const target, const DISPID dispid, const uint8_t kind, const VARIANT* const args, const size_t argc) const;
   bool PutFileValue(const std::string& key, const uint32_t obj, const DISPID dispid, const VarList& desired);
   std::string CallName(const uint32_t obj, const DISPID dispid) const;

   // CASTCADE_TEST_MATCHCHECK
   void BuildCheckProps();
   void Fingerprint(std::vector<std::vector<uint8_t>>& out) const;
   void CompareFingerprint();

   Player* const m_player;
   SessionImpl m_play;
   StreamLog m_log;
   std::unique_ptr<SessionImpl> m_watch;
   const DISPID m_dispidPut = DISPID_PROPERTYPUT;

   // Configuration
   std::string m_ctlHost;
   uint16_t m_ctlPort = 0;
   bool m_ctlOk = false;
   bool m_wsa = false;
   bool m_staticRefresh = false;
   bool m_check = false;
   bool m_testNoUndo = false; // CASTCADE_TEST_MATCHNOUNDO=1: leave the undo list and the runtime state unapplied
   uint64_t m_ballLogUsec = 0, m_nextBallLog = 0;

   // The control connection
   std::unique_ptr<LinePeer> m_ctl;
   bool m_ctlUp = false; // connected, hello sent
   uint64_t m_ctlNextRetry = 0;
   uint64_t m_ctlLastFailLog = 0;
   std::vector<std::string> m_lines;

   // The local table's objects by stream id (as a watcher's map)
   std::vector<IDispatch*> m_ownDisp;
   std::vector<const WCHAR*> m_ownName;
   IDispatch* m_tableDisp = nullptr;
   IDispatch* m_globalDisp = nullptr;
   DISPID m_dispUpdateMaterial = DISPID_UNKNOWN, m_dispMaterialColor = DISPID_UNKNOWN;
   std::shared_ptr<const TableRuntime> m_fileRuntime; // before the script started

   // The watch phase
   bool m_watching = false;
   bool m_disturbed = false;   // a keyframe has been applied: the local table shows another player's
   bool m_saidWatching = false;
   int m_upFrames = 0;
   std::string m_watchAddr;
   uint64_t m_watchSince = 0;
   uint64_t m_watchAsked = 0; // the last "watch" command
   uint64_t m_noStreamSince = 0;
   uint64_t m_phases = 0;
   std::shared_ptr<const OwnPhysics> m_ownPhysics;
   std::shared_ptr<const TableRuntime> m_ownRuntime;
   std::shared_ptr<OwnPlayer> m_own;
   std::vector<Undo> m_undo;
   std::unordered_map<std::string, size_t> m_undoIndex;
   std::unordered_map<std::wstring, std::shared_ptr<const SavedMaterial>> m_materials;
   std::unordered_set<std::string> m_kfKeys;
   ByteWriter m_enc;
   std::string m_keyBuf;
   int m_warned = 0;

   // Keys: what the script was last told, and what happened while it was not told
   std::unordered_map<int, bool> m_keysTold, m_keysNow;

   // CASTCADE_TEST_MATCHCHECK
   std::vector<CheckProp> m_checkProps;
   bool m_checkBuilt = false;
   std::vector<std::vector<uint8_t>> m_checkBefore;
};

MatchSession::MatchSession(Player* const player)
   : m_player(player)
   , m_play(player, false, nullptr, true)
{
   m_log.OpenShared(m_play.m_log, "match");
   m_fileRuntime = SaveTableRuntime(player);

   PinTable* const table = player->m_ptable;
   m_ownDisp.assign(table->m_vedit.size(), nullptr);
   m_ownName.assign(table->m_vedit.size(), nullptr);
   for (size_t i = 0; i < table->m_vedit.size(); ++i)
      if (IScriptable* const ps = table->m_vedit[i]->GetScriptable())
      {
         m_ownDisp[i] = ps->GetDispatch();
         m_ownName[i] = ps->m_wzName;
      }
   m_tableDisp = table->GetDispatch();
   m_globalDisp = table->m_psgt->GetDispatch();
   {
      LPOLESTR a = const_cast<LPOLESTR>(L"UpdateMaterial"), b = const_cast<LPOLESTR>(L"MaterialColor");
      if (FAILED(m_globalDisp->GetIDsOfNames(IID_NULL, &a, 1, LOCALE_USER_DEFAULT, &m_dispUpdateMaterial)))
         m_dispUpdateMaterial = DISPID_UNKNOWN;
      if (FAILED(m_globalDisp->GetIDsOfNames(IID_NULL, &b, 1, LOCALE_USER_DEFAULT, &m_dispMaterialColor)))
         m_dispMaterialColor = DISPID_UNKNOWN;
   }

   m_staticRefresh = GetEnv("CASTCADE_STREAM_STATICREFRESH") == "1";
   m_check = GetEnv("CASTCADE_TEST_MATCHCHECK") == "1";
   m_testNoUndo = GetEnv("CASTCADE_TEST_MATCHNOUNDO") == "1";
   if (m_testNoUndo)
      m_log.Warn("TEST: CASTCADE_TEST_MATCHNOUNDO=1, properties and runtime state are NOT put back after a watch phase (shows that the checks see it)");
   m_ballLogUsec = (uint64_t)std::clamp(atoi(GetEnv("CASTCADE_TEST_BALLLOG").c_str()), 0, 60000) * 1000;

   // connect:<loopback host>:<port>, nothing else: the companion listens on this PC
   const std::string ctl = GetEnv("CASTCADE_STREAM_CONTROL");
   bool listen = true;
   if (ctl.rfind("connect:", 0) == 0 && ParseAddr(ctl, listen, m_ctlHost, m_ctlPort) && !listen
      && (m_ctlHost == "localhost" || m_ctlHost.rfind("127.", 0) == 0))
   {
      m_wsa = WireStartup();
      m_ctlOk = m_wsa;
   }
   if (m_ctlOk)
      m_log.Info(Fmt("role match: control connection to %s:%u (retried every 2 s); playing until told to watch; static pre-render after a keyframe %s%s",
         m_ctlHost.c_str(), m_ctlPort, m_staticRefresh ? "rebuilt (CASTCADE_STREAM_STATICREFRESH=1)" : "kept", m_check ? "; TEST: every property compared after each watch phase" : ""));
   else
      m_log.Error(ctl.empty() ? std::string("role match without CASTCADE_STREAM_CONTROL (connect:127.0.0.1:<port>): this table plays and can never be told to watch")
                              : "CASTCADE_STREAM_CONTROL '" + ctl + "' is not connect:127.0.0.1:<port> (this PC only): this table plays and can never be told to watch");
}

MatchSession::~MatchSession()
{
   // The table closes: its own state back first, so that its script ends on its own game (the Exit event follows)
   if (m_watching)
      LeaveWatch("this table closes", nullptr);
   g_holdEvents = false;
   if (m_ctl)
   {
      m_lines.clear();
      m_ctl->Poll(m_lines); // flushes
      m_ctl->Close();
   }
   if (m_wsa)
      WireShutdown();
   m_log.Info(Fmt("role match: %llu watch phases in this session", (unsigned long long)m_phases));
}

void MatchSession::OnFrame()
{
   const uint64_t now = NowUsec();
   if (m_watching)
   {
      // Player calls a watcher before the frame's script timers: a "play" puts this table back before they run, and
      // Player then calls again after them, as it calls a player
      ServiceControl(now);
      if (!m_watching)
         return;
      m_watch->OnFrame();
      CheckWatch(now);
      if (!m_watching)
         return;
      m_play.OnFrame(); // this table's own watchers: its state as it was, under a stamp that stands still
      return;
   }
   m_play.OnFrame(); // first: the calls of this frame belong to this table's stream, whatever the control connection says next
   if (m_ballLogUsec && now >= m_nextBallLog)
   {
      m_nextBallLog = now + m_ballLogUsec;
      m_log.Info("balls " + DescribeLiveBalls(m_player));
   }
   ServiceControl(now);
}

void MatchSession::Send(const std::string& line)
{
   const bool sent = m_ctl && m_ctl->Connected() && m_ctl->Send(line);
   m_log.Info((sent ? "control > " : "control (not connected) > ") + line);
}

void MatchSession::ServiceControl(const uint64_t now)
{
   if (!m_ctlOk)
      return;
   if (!m_ctl && now >= m_ctlNextRetry)
   {
      m_ctlNextRetry = now + RETRY_USEC;
      m_ctlUp = false;
      std::string err;
      m_ctl = ConnectLines(m_ctlHost, m_ctlPort, err);
      if (!m_ctl && now - m_ctlLastFailLog >= CONNECT_FAIL_LOG_USEC)
      {
         m_ctlLastFailLog = now;
         m_log.Warn(Fmt("control: cannot connect to %s:%u: %s", m_ctlHost.c_str(), m_ctlPort, err.c_str()));
      }
   }
   if (!m_ctl)
      return;
   m_lines.clear();
   const bool alive = m_ctl->Poll(m_lines);
   if (!m_ctlUp && m_ctl->Connected())
   {
      m_ctlUp = true;
      m_log.Info(Fmt("control: connected to %s:%u", m_ctlHost.c_str(), m_ctlPort));
      // hello <protocol> <build> <editables> <table hash> <stream port>: the table identity is the stream's own
      // (BuildHello); the port is the one this table's stream listener is bound to (0: it has none), which is what
      // the companion offers the other players
      Send(Fmt("hello 1 %s %u %016llx %u", Token(VP_VERSION_STRING_POINTS_FULL).c_str(), m_play.m_editCount, (unsigned long long)m_play.m_hash,
         m_play.m_listener ? (unsigned)m_play.m_listener->Port() : 0u));
      Send("state playing"); // true whenever a control connection begins: without one this table plays
   }
   // A line may end the connection's use (LeaveWatch sends on it), never the connection itself
   const std::vector<std::string> lines = std::move(m_lines);
   m_lines.clear();
   for (const std::string& line : lines)
      Command(line, now);
   if (alive)
      return;
   if (m_ctlUp)
      m_log.Warn("control: connection lost, retrying every 2 s");
   else if (now - m_ctlLastFailLog >= CONNECT_FAIL_LOG_USEC)
   {
      m_ctlLastFailLog = now;
      m_log.Info(Fmt("control: nothing listens on %s:%u yet, retrying every 2 s", m_ctlHost.c_str(), m_ctlPort));
   }
   m_ctl.reset();
   m_ctlUp = false;
   m_ctlNextRetry = now + RETRY_USEC;
   if (m_watching)
      LeaveWatch("the control connection is lost", nullptr);
}

void MatchSession::Command(const std::string& line, const uint64_t now)
{
   m_log.Info("control < " + line);
   if (line == "play")
   {
      if (m_watching)
         LeaveWatch("play", nullptr);
      else
         Send("state playing");
      return;
   }
   if (line.rfind("watch ", 0) == 0)
   {
      const std::string addr = line.substr(6);
      const size_t colon = addr.rfind(':');
      uint16_t port = 0;
      if (colon != std::string::npos && colon > 0 && addr.find(' ') == std::string::npos && ParsePort(addr.substr(colon + 1), port))
      {
         EnterWatch(addr.substr(0, colon), port, now);
         return;
      }
      m_log.Warn("control: '" + line + "' is not watch <host>:<port>");
      if (m_watching)
         LeaveWatch("a watch command without an address", "badaddress");
      else
      {
         Send("state failed badaddress");
         Send("state playing");
      }
      return;
   }
   m_log.Warn("control: unknown command '" + line + "': playing");
   if (m_watching)
      LeaveWatch("an unknown command", nullptr);
   else
      Send("state playing");
}

bool MatchSession::OnKeyEvent(const int dispid, const int keycode)
{
   if (dispid != DISPID_GameEvents_KeyDown && dispid != DISPID_GameEvents_KeyUp)
      return false;
   const bool down = dispid == DISPID_GameEvents_KeyDown;
   if (m_watching)
   {
      m_keysNow[keycode] = down; // not this player's turn: the script is told at the end, if it still matters
      return true;
   }
   m_keysTold[keycode] = down;
   return false;
}

void MatchSession::StartInner(const std::string& host, const uint16_t port)
{
   InnerWatch iw;
   iw.host = host;
   iw.port = port;
   iw.observer = this;
   iw.log = &m_log;
   iw.staticRefresh = m_staticRefresh;
   m_watch = std::make_unique<SessionImpl>(m_player, true, &iw);
   m_saidWatching = false;
   m_upFrames = 0;
}

void MatchSession::EnterWatch(const std::string& host, const uint16_t port, const uint64_t now)
{
   const std::string addr = host + ":" + std::to_string(port);
   if (m_watching)
   {
      // The next player is up: the same watch phase, another stream, whose keyframe starts from the file state
      // again. The address may be the one before (the companion's tunnel has one listener for every player and
      // re-aims behind it): the connection is dropped and made again all the same, and the new stream is announced
      // with a "state watching" of its own. No "state playing" in between: this table stays away.
      m_log.Info("re-aiming the watch from " + m_watchAddr + " to " + addr);
      m_watch.reset();
      m_watchAddr = addr;
      m_watchAsked = now;
      m_noStreamSince = now;
      StartInner(host, port);
      return;
   }

   const uint64_t t0 = NowUsec();
   // The pause a real pause is: the script hears Paused (a ROM table stops VPinMAME in it), the music pauses
   m_player->PauseMusic();
   g_holdEvents = true;
   m_play.Freeze();
   m_ownPhysics = SaveOwnPhysics(m_player);
   m_ownRuntime = SaveTableRuntime(m_player);
   m_own = BeginWatchPhase(m_player);
   if (m_check)
   {
      if (!m_checkBuilt)
         BuildCheckProps();
      Fingerprint(m_checkBefore);
   }
   m_keysNow = m_keysTold;
   m_watching = true;
   m_disturbed = false;
   m_watchSince = now;
   m_watchAsked = now;
   m_noStreamSince = now;
   m_watchAddr = addr;
   ++m_phases;
   StartInner(host, port);
   m_log.Info(Fmt("entering watch %llu of %s: own balls %s; own game paused in %.2f ms", (unsigned long long)m_phases, addr.c_str(), DescribeOwnBalls(*m_own).c_str(),
      (double)(NowUsec() - t0) / 1000.));
}

void MatchSession::CheckWatch(const uint64_t now)
{
   SessionImpl& w = *m_watch;
   if (w.m_refused)
   {
      LeaveWatch("the stream was refused (not the same table)", "refused");
      return;
   }
   if (w.m_bye)
   {
      LeaveWatch("the watched player closed its table", "closed");
      return;
   }
   const bool up = !w.m_peers.empty() && w.m_peers.front().helloOk && w.m_keyframeApplied;
   if (up)
   {
      m_noStreamSince = 0;
      // The keyframe was applied in an earlier frame: its picture has been drawn
      if (!m_saidWatching && ++m_upFrames >= 2)
      {
         m_saidWatching = true;
         Send("state watching");
      }
      // A stream that is open and silent: a player that plays, pauses or watches itself sends a frame with every
      // frame it draws, so 3 s without one is a stream that hangs somewhere on the way. Better the companion's 2D
      // picture than a table that stands still.
      // (the watch session stamps an arrival with its own, later, reading of the clock: never subtract the other way)
      if (w.m_lastAnyArrival && now > w.m_lastAnyArrival && now - w.m_lastAnyArrival >= WATCH_STALL_USEC)
         LeaveWatch("the stream delivered no frame for 3 s", "stalled");
      return;
   }
   m_upFrames = 0;
   if (m_noStreamSince == 0)
      m_noStreamSince = now;
   if (!m_saidWatching)
   {
      // Not shown yet (never connected, not answered, held by a table that is not playing, no keyframe)
      if (now - m_watchAsked >= WATCH_FIRST_USEC)
         LeaveWatch("no stream within 4 s of the watch command", "timeout");
   }
   else if (now - m_noStreamSince >= WATCH_STALL_USEC)
      LeaveWatch("the stream ended and did not come back within 3 s", "lost");
}

IDispatch* MatchSession::ResolveOwn(const uint32_t id) const
{
   if (id == ID_TABLE)
      return m_tableDisp;
   if (id == ID_GLOBAL)
      return m_globalDisp;
   return id < m_ownDisp.size() ? m_ownDisp[id] : nullptr;
}

std::string MatchSession::CallName(const uint32_t obj, const DISPID dispid) const
{
   IDispatch* const d = ResolveOwn(obj);
   const std::string who = obj == ID_TABLE ? "Table" : obj == ID_GLOBAL ? "Global" : (obj < m_ownName.size() && m_ownName[obj]) ? Narrow(m_ownName[obj]) : Fmt("object %u", obj);
   return who + "." + MemberName(d, dispid);
}

// The key the play part stores a put under (RecordImpl): object, DISPID, the encoded index arguments
bool MatchSession::MakeKey(const uint32_t obj, const DISPID dispid, const VARIANT* const args, const size_t argc, std::string& key)
{
   key.assign(8, '\0');
   memcpy(key.data(), &obj, 4);
   memcpy(key.data() + 4, &dispid, 4);
   if (argc > 1)
   {
      m_enc.clear();
      for (size_t i = 1; i < argc; ++i)
         if (!EncodeVariant(m_enc, args[i]))
            return false;
      key.append((const char*)m_enc.data(), m_enc.size());
   }
   return true;
}

HRESULT MatchSession::Put(IDispatch* const target, const DISPID dispid, const uint8_t kind, const VARIANT* const args, const size_t argc) const
{
   DISPPARAMS dp;
   dp.rgvarg = const_cast<VARIANT*>(args);
   dp.cArgs = (UINT)argc;
   dp.cNamedArgs = 1;
   dp.rgdispidNamedArgs = const_cast<DISPID*>(&m_dispidPut);
   return target->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, kind == CALL_PUTREF ? DISPATCH_PROPERTYPUTREF : DISPATCH_PROPERTYPUT, &dp, nullptr, nullptr, nullptr);
}

// First touch of a property in this watch phase: its value now, which is this table's own
void MatchSession::Capture(const std::string& key, const uint32_t obj, const DISPID dispid, const uint8_t kind, const VARIANT* const args, const size_t argc, IDispatch* const target)
{
   Undo u;
   u.key = key;
   u.obj = obj;
   u.dispid = dispid;
   u.kind = kind;
   u.args.v.resize(argc);
   for (VARIANT& v : u.args.v)
      VariantInit(&v);
   bool ok = true;
   for (size_t i = 1; ok && i < argc; ++i)
      ok = SUCCEEDED(VariantCopyInd(&u.args.v[i], const_cast<VARIANT*>(&args[i])));
   if (ok && kind == CALL_PUT)
   {
      DISPPARAMS get = { argc > 1 ? u.args.v.data() + 1 : nullptr, nullptr, (UINT)(argc - 1), 0 };
      u.readable = SUCCEEDED(target->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &get, &u.args.v[0], nullptr, nullptr));
      if (u.readable && (V_VT(&u.args.v[0]) == VT_DISPATCH || V_VT(&u.args.v[0]) == VT_UNKNOWN))
         u.readable = false;
      if (!u.readable)
         VariantClear(&u.args.v[0]);
   }
   // This table's own last put of the property: the value to put back when there is nothing to read, and the better
   // one when it is what was read
   const auto st = m_play.m_stored.find(key);
   if (ok && st != m_play.m_stored.end() && st->second.kind == kind && st->second.argc == argc)
   {
      VARIANT sv;
      VariantInit(&sv);
      ByteReader r(st->second.args.data(), st->second.args.size());
      if (DecodeVariant(r, sv))
      {
         if (!u.readable || SameValue(u.args.v[0], sv, 1e-6))
         {
            VariantClear(&u.args.v[0]);
            u.args.v[0] = sv; // ownership moves
            u.readable = true;
            u.fromStore = true;
         }
         else
            VariantClear(&sv);
      }
   }
   m_undoIndex.emplace(key, m_undo.size());
   m_undo.push_back(std::move(u));
}

void MatchSession::BeforeCall(const RxCall& c, const VARIANT* const args, const KindClass cls, IDispatch* const target)
{
   if (cls == KC_BALL) // the watch session's own balls: gone with it
      return;
   if (c.kind == CALL_METHOD)
   {
      // What the other methods change is in the TableRuntime and the physics state saved when the phase began
      if (c.obj == ID_GLOBAL && c.argc >= 1 && (c.dispid == m_dispUpdateMaterial || c.dispid == m_dispMaterialColor))
      {
         std::wstring name;
         const VARIANT& n = args[c.argc - 1]; // a decoded argument: a plain BSTR
         if (V_VT(&n) == VT_BSTR && V_BSTR(&n))
            name.assign(V_BSTR(&n), SysStringLen(V_BSTR(&n)));
         if (!name.empty() && !m_materials.count(name))
            if (std::shared_ptr<const SavedMaterial> s = SaveMaterial(m_player, name))
               m_materials.emplace(name, std::move(s));
      }
      return;
   }
   if (!MakeKey(c.obj, c.dispid, args, c.argc, m_keyBuf) || m_undoIndex.count(m_keyBuf))
      return;
   Capture(m_keyBuf, c.obj, c.dispid, c.kind, args, c.argc, target);
}

// The keyframe's pre-pass for one property: to the value the table file gave it. False when the put failed.
bool MatchSession::PutFileValue(const std::string& key, const uint32_t obj, const DISPID dispid, const VarList& desired)
{
   IDispatch* const target = ResolveOwn(obj);
   if (!target)
      return false;
   const bool first = !m_undoIndex.count(key);
   if (first)
   {
      Capture(key, obj, dispid, CALL_PUT, desired.v.data(), desired.v.size(), target);
      const Undo& u = m_undo.back();
      if (u.readable && !u.fromStore && SameValue(u.args.v[0], desired.v[0], 0.))
         return true; // it holds the file's value already
   }
   return SUCCEEDED(Put(target, dispid, CALL_PUT, desired.v.data(), desired.v.size()));
}

void MatchSession::BeforeKeyframe(const RxFrame& kf)
{
   const uint64_t t0 = NowUsec();
   if (!m_disturbed)
   {
      m_disturbed = true;
      HideOwnTable(m_player, *m_own);
   }
   // What no property returns: as before any script ran (the stamps of blinkers and reels start from now)
   RestoreTableRuntime(m_player, *m_fileRuntime, true);

   m_kfKeys.clear();
   for (const RxCall& c : kf.calls)
      if (c.kind != CALL_METHOD && !(c.obj & ID_BALL) && MakeKey(c.obj, c.dispid, &kf.args[c.argStart], c.argc, m_keyBuf))
         m_kfKeys.insert(m_keyBuf);

   // Every property this table's script has put, or an earlier stream of this phase has, that this keyframe does not
   // put: to the table file's value
   size_t puts = 0, failed = 0, unreadable = 0;
   for (const auto& kv : m_play.m_baseline)
   {
      if (m_kfKeys.count(kv.first))
         continue;
      if (!kv.second.readable)
      {
         ++unreadable;
         continue;
      }
      ++puts;
      if (!PutFileValue(kv.first, kv.second.obj, kv.second.dispid, kv.second.args))
      {
         ++failed;
         if (m_warned < MAX_LOGGED)
         {
            ++m_warned;
            m_log.Warn("keyframe pre-pass: could not put the file's value of " + CallName(kv.second.obj, kv.second.dispid));
         }
      }
   }
   for (size_t i = 0; i < m_undo.size(); ++i)
   {
      const Undo& u = m_undo[i];
      if (m_kfKeys.count(u.key) || m_play.m_baseline.count(u.key) || !u.readable || u.kind != CALL_PUT)
         continue; // (a property this table's script never put: what was read at its first touch is the file's value)
      IDispatch* const target = ResolveOwn(u.obj);
      ++puts;
      if (!target || FAILED(Put(target, u.dispid, u.kind, u.args.v.data(), u.args.v.size())))
         ++failed;
   }
   m_log.Info(Fmt("keyframe pre-pass: local balls hidden, runtime state to the file's, %zu of %zu own properties not in the keyframe put to the file's value (%zu failed, %zu "
                  "with no readable value left as they are); %zu in the undo list; %.2f ms",
      puts, m_play.m_baseline.size(), failed, unreadable, m_undo.size(), (double)(NowUsec() - t0) / 1000.));
}

void MatchSession::LeaveWatch(const char* const why, const char* const failed)
{
   const uint64_t t0 = NowUsec();
   if (m_watch)
   {
      m_watch->StopAllSoundsHere("the watch ends"); // nothing with the stream's sound off
      m_watch.reset();                             // its balls are destroyed
   }

   size_t undone = 0, fromStore = 0, putFailed = 0, lost = 0;
   const size_t materials = m_materials.size();
   if (m_disturbed)
   {
      // The movers first: a put's own side effect (a gate told to open or close) must see this table's angles
      RestoreOwnPhysics(m_player, *m_ownPhysics);
      for (size_t i = m_undo.size(); i-- > 0;)
      {
         const Undo& u = m_undo[i];
         IDispatch* const target = ResolveOwn(u.obj);
         if (m_testNoUndo)
            continue;
         if (!u.readable || !target)
         {
            // No getter and never put by this table's script: nothing says what it was
            ++lost;
            if (m_warned < MAX_LOGGED)
            {
               ++m_warned;
               m_log.Warn("cannot put back " + CallName(u.obj, u.dispid) + ": it has no readable value and this table's script never set it");
            }
            continue;
         }
         if (FAILED(Put(target, u.dispid, u.kind, u.args.v.data(), u.args.v.size())))
         {
            ++putFailed;
            if (m_warned < MAX_LOGGED)
            {
               ++m_warned;
               m_log.Warn("putting back " + CallName(u.obj, u.dispid) + " = " + VarText(u.args.v[0]) + " failed");
            }
            continue;
         }
         ++undone;
         fromStore += u.fromStore ? 1 : 0;
      }
      for (const auto& kv : m_materials)
         RestoreMaterial(m_player, *kv.second);
      if (!m_testNoUndo)
         RestoreTableRuntime(m_player, *m_ownRuntime, false);
      RestoreOwnPhysics(m_player, *m_ownPhysics);
      if (m_staticRefresh)
         MarkStaticPrepassDirty(m_player);
   }
   // The balls shown again, the ball id counter, the music, and the game clock back to where this game stood
   const unsigned int clockBack = EndWatchPhase(m_player, *m_own);

   // The same bytes a watcher of this table was sent when the phase began?
   ByteWriter after;
   CaptureState(m_player, m_play.m_ballIds, after);
   const bool samePhysics = after.vec() == m_play.m_frozenState;
   if (m_check)
      CompareFingerprint();

   const size_t undoSize = m_undo.size();
   const bool disturbed = m_disturbed;
   m_undo.clear();
   m_undoIndex.clear();
   m_materials.clear();
   m_ownPhysics.reset();
   m_ownRuntime.reset();
   m_disturbed = false;
   m_saidWatching = false;

   m_play.Unfreeze();
   m_watching = false;
   m_player->RecomputePauseState();
   g_holdEvents = false;
   m_player->UnpauseMusic(); // UnPaused: a ROM table lets VPinMAME run again
   // Keys that changed while the script was not told: it hears the difference now (a flipper button let go)
   const std::unordered_map<int, bool> now = std::move(m_keysNow);
   m_keysNow.clear();
   size_t keys = 0;
   for (const auto& kv : now)
   {
      const auto told = m_keysTold.find(kv.first);
      if ((told != m_keysTold.end() && told->second) != kv.second)
      {
         ++keys;
         m_keysTold[kv.first] = kv.second;
         m_player->m_ptable->FireKeyEvent(kv.second ? DISPID_GameEvents_KeyDown : DISPID_GameEvents_KeyUp, kv.first);
      }
   }

   m_log.Info(Fmt("back to playing (%s) after %.2f s of watch %llu: %s; %zu of %zu properties put back (%zu with this table's own stored value, %zu failed, %zu lost), "
                  "%zu materials; own physics state %s (balls %s); game clock set back %u ms; %zu key events passed on; %.2f ms",
      why, (double)(t0 - m_watchSince) / 1e6, (unsigned long long)m_phases, disturbed ? "the table had shown the other player's" : "the table was never touched", undone,
      undoSize, fromStore, putFailed, lost, materials, samePhysics ? "identical" : "DIFFERENT", DescribeOwnBalls(*m_own).c_str(), clockBack, keys,
      (double)(NowUsec() - t0) / 1000.));
   if (!samePhysics)
      m_log.Error("the local physics state after the watch is not the one before it");
   m_own.reset();

   if (failed)
      Send(std::string("state failed ") + failed);
   Send("state playing");
}

// CASTCADE_TEST_MATCHCHECK=1: every property of every part and of the table that can be read without arguments, read
// when a watch phase begins and again when it has ended. Thousands of calls: a test's check, not a match's.
void MatchSession::BuildCheckProps()
{
   m_checkBuilt = true;
   const auto add = [this](const uint32_t obj, IDispatch* const disp)
   {
      ITypeInfo* ti = nullptr;
      if (!disp || FAILED(disp->GetTypeInfo(0, LOCALE_USER_DEFAULT, &ti)) || !ti)
         return;
      TYPEATTR* ta = nullptr;
      if (SUCCEEDED(ti->GetTypeAttr(&ta)) && ta)
      {
         for (UINT f = 0; f < ta->cFuncs; ++f)
         {
            FUNCDESC* fd = nullptr;
            if (FAILED(ti->GetFuncDesc(f, &fd)) || !fd)
               continue;
            const bool noArgs = fd->cParams == 0 || (fd->cParams == 1 && (fd->lprgelemdescParam[0].paramdesc.wParamFlags & PARAMFLAG_FRETVAL));
            // Not the properties that return an object: nothing to compare, and Kicker.LastCapturedBall answers with a
            // message box when there is none
            // (the type info is the dual interface's: the value comes back through an [out, retval] pointer parameter;
            // a dispinterface's would give it as the return type)
            VARTYPE ret = fd->elemdescFunc.tdesc.vt;
            if (fd->cParams == 1)
            {
               const TYPEDESC& td = fd->lprgelemdescParam[0].tdesc;
               ret = (td.vt == VT_PTR && td.lptdesc) ? td.lptdesc->vt : td.vt;
            }
            const bool object = ret == VT_PTR || ret == VT_DISPATCH || ret == VT_UNKNOWN;
            if (fd->invkind == INVOKE_PROPERTYGET && noArgs && !object && !(fd->wFuncFlags & (FUNCFLAG_FRESTRICTED | FUNCFLAG_FHIDDEN)))
               m_checkProps.push_back(CheckProp { obj, disp, fd->memid });
            ti->ReleaseFuncDesc(fd);
         }
         ti->ReleaseTypeAttr(ta);
      }
      ti->Release();
   };
   for (size_t i = 0; i < m_ownDisp.size(); ++i)
      add((uint32_t)i, m_ownDisp[i]);
   add(ID_TABLE, m_tableDisp);
}

void MatchSession::Fingerprint(std::vector<std::vector<uint8_t>>& out) const
{
   out.resize(m_checkProps.size());
   ByteWriter w;
   for (size_t i = 0; i < m_checkProps.size(); ++i)
   {
      const CheckProp& p = m_checkProps[i];
      VARIANT v;
      VariantInit(&v);
      DISPPARAMS none = {};
      w.clear();
      const HRESULT hr = p.disp->Invoke(p.id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &none, &v, nullptr, nullptr);
      if (FAILED(hr))
      {
         w.u8(0xFF);
         w.u32((uint32_t)hr);
      }
      else if (!EncodeVariant(w, v)) // an object: only that it is one
      {
         w.clear();
         w.u8(0xFE);
         w.u16(V_VT(&v));
      }
      VariantClear(&v);
      out[i] = w.vec();
   }
}

void MatchSession::CompareFingerprint()
{
   std::vector<std::vector<uint8_t>> after;
   Fingerprint(after);
   size_t differ = 0, approx = 0;
   for (size_t i = 0; i < after.size() && i < m_checkBefore.size(); ++i)
   {
      if (after[i] == m_checkBefore[i])
         continue;
      VARIANT a, b;
      VariantInit(&a);
      VariantInit(&b);
      ByteReader ra(m_checkBefore[i].data(), m_checkBefore[i].size()), rb(after[i].data(), after[i].size());
      const bool decoded = !m_checkBefore[i].empty() && !after[i].empty() && m_checkBefore[i][0] < 0xFE && after[i][0] < 0xFE && DecodeVariant(ra, a) && DecodeVariant(rb, b);
      if (decoded && SameValue(a, b, 1e-4))
         ++approx; // a float through a getter and a setter
      else
      {
         ++differ;
         if (differ <= 40)
            m_log.Warn("match check: DIFFERENT " + CallName(m_checkProps[i].obj, m_checkProps[i].id) + ": before " + (decoded ? VarText(a) : std::string("(unreadable)")) + ", after "
               + (decoded ? VarText(b) : std::string("(unreadable)")));
      }
      VariantClear(&a);
      VariantClear(&b);
   }
   m_log.Info(Fmt("match check: %zu properties compared, %zu differ, %zu equal within 0.01 %% only", after.size(), differ, approx));
   m_checkBefore.clear();
}

} // namespace

Session* Session::Create(Player* const player)
{
   const std::string mode = GetEnv("CASTCADE_STREAM");
   if (mode == "match")
      return new MatchSession(player);
   if (mode != "play" && mode != "watch")
      return nullptr;
   return new SessionImpl(player, mode == "watch");
}

void Session::MarkStaticPrepassDirty(Player* const player)
{
   player->m_isStaticPrepassDirty = true;
}

void OnScriptCall(IDispatch* obj, DISPID dispid, WORD flags, const DISPPARAMS* params)
{
   if (g_session)
      g_session->Record(obj, dispid, flags, params);
}

void OnScriptPutBefore(IDispatch* obj, DISPID dispid, const DISPPARAMS* params)
{
   if (g_session)
      g_session->BaselineBefore(obj, dispid, params);
}

}
