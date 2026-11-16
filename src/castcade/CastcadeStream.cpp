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

namespace
{

constexpr uint16_t PROTOCOL_VERSION = 1;
constexpr uint8_t ROLE_PLAY = 1;
constexpr uint8_t ROLE_WATCH = 2;
constexpr uint16_t DEFAULT_PORT = 47800;

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
constexpr uint64_t STATS_USEC = 5000000;
constexpr uint64_t TIMELINE_MAX_USEC = 2000000;
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

// Appends to CASTCADE_STREAM_LOG (two instances on one PC share vpinball.log; this file is per instance) and
// to the VPX log.
class StreamLog final
{
public:
   ~StreamLog()
   {
      if (m_file)
         fclose(m_file);
   }

   void Open(const std::string& path, const char* const role)
   {
      m_role = role;
      if (!path.empty())
         m_file = _fsopen(path.c_str(), "a", _SH_DENYNO);
   }
   bool IsOpen() const { return m_file != nullptr; }

   void Info(const std::string& s) { Write("", s); PLOGI << "Castcade " << m_role << ": " << s; }
   void Warn(const std::string& s) { Write("WARNING ", s); PLOGW << "Castcade " << m_role << ": " << s; }
   void Error(const std::string& s) { Write("ERROR ", s); PLOGE << "Castcade " << m_role << ": " << s; }

private:
   void Write(const char* const level, const std::string& s)
   {
      if (!m_file)
         return;
      SYSTEMTIME t;
      GetLocalTime(&t);
      fprintf(m_file, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s pid %lu] %s%s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
         m_role.c_str(), GetCurrentProcessId(), level, s.c_str());
      fflush(m_file);
   }

   FILE* m_file = nullptr;
   std::string m_role;
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
};

class SessionImpl;
SessionImpl* g_session = nullptr;

class SessionImpl final : public Session
{
public:
   SessionImpl(Player* const player, const bool watch);
   ~SessionImpl() override;

   bool IsWatcher() const override { return m_watch; }
   bool NeverPauses() const override { return m_neverPauses; }
   void OnBallCreated(Ball* const ball) override;
   void OnBallDestroyed(Ball* const ball) override;
   void OnFrame() override;

   void Record(IDispatch* const obj, const DISPID dispid, const WORD flags, const DISPPARAMS* const params);

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

   // Watch
   void WatchFrame(const uint64_t now);
   void WatchService(Peer& p, const uint64_t now);
   bool ParseFrame(const Message& m, RxFrame& f);
   void ApplyKeyframe(RxFrame& kf);
   void StepTimeline(const uint64_t now);
   void Replay(RxFrame& f);
   IDispatch* Resolve(const uint32_t id, KindClass& cls) const;
   void Refuse(const std::string& why);
   void AddOffsetSample(const int64_t sample);

   // Both
   void SendHello(Peer& p);
   bool ParseHello(const Message& m, Hello& h) const;
   bool CheckHello(const Hello& h, std::string& why) const;
   void AccountBytes(Peer& p);
   void Note(const std::string& text, const U32 ms = NOTE_MS) { m_notes.emplace_back(text, ms); }
   void FlushNotes();
   void LogStats(const uint64_t now);
   Policy* NewPolicy(const int key, const std::string& kind, IDispatch* const sample);
   void LogSentByName(const char* const when);

   // Test tooling (CASTCADE_TEST_INPUT)
   struct TestKey
   {
      uint64_t ms;
      bool down;
      std::string key; // a binding name, or empty when code is a scan code
      int code;
      int line;
   };
   void LoadTestInput();
   void FireTestInput(const uint64_t now);

   Player* const m_player;
   const bool m_watch;
   StreamLog m_log;

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

   // Test tooling
   std::vector<TestKey> m_testKeys;
   size_t m_testNext = 0;
   uint64_t m_testStart = 0;

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

   // Both
   uint64_t m_statsStart = 0;
   uint64_t m_iFrames = 0;
   uint64_t m_iBytesOut = 0, m_iBytesIn = 0;
};

// ---------------------------------------------------------------------------------------------------------
// Setup and teardown

SessionImpl::SessionImpl(Player* const player, const bool watch)
   : m_player(player)
   , m_watch(watch)
   , m_tsc0(__rdtsc())
   , m_qpc0(NowUsec())
{
   m_log.Open(GetEnv("CASTCADE_STREAM_LOG"), watch ? "watch" : "play");

   m_addrText = GetEnv("CASTCADE_STREAM_ADDR");
   if (m_addrText.empty() && !watch)
      m_addrText = std::to_string(DEFAULT_PORT);
   if (m_addrText.empty())
   {
      m_log.Error("CASTCADE_STREAM=watch needs CASTCADE_STREAM_ADDR (<port> to listen, <host>:<port> to connect); staying idle");
      Note("Castcade: no CASTCADE_STREAM_ADDR set, nothing to watch", NOTE_LASTING_MS);
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
   }

   const std::string& file = m_player->m_ptable->m_szFileName;
   const size_t slash = file.find_last_of("\\/");
   m_tableFile = slash == std::string::npos ? file : file.substr(slash + 1);

   g_session = this; // before the identity probes, which go through OnScriptCall
   BuildObjectMap();

   m_log.Info(Fmt("session start: role %s, address %s (%s %s:%u), delay %d ms, sound %s, never pauses %s, table '%s', %u editables, hash %016llx, build %s",
      watch ? "watch" : "play", m_addrText.empty() ? "(none)" : m_addrText.c_str(), m_listen ? "listen on" : "connect to",
      m_listen ? (m_connectHost.empty() ? "0.0.0.0" : m_connectHost.c_str()) : m_connectHost.c_str(), m_port, (int)(m_delayUsec / 1000),
      m_replaySound ? "on" : "off", m_neverPauses ? "yes" : "no", m_tableFile.c_str(), m_editCount, (unsigned long long)m_hash, VP_VERSION_STRING_POINTS_FULL));

   if (watch)
      m_applier = std::make_unique<StateApplier>(m_player);
   else
      LoadTestInput();
   // A watcher's window is rarely focused (a VR watcher's almost never): it must not start paused either
   // (Player::RecomputePauseState keeps it unpaused from here on)
   if (m_neverPauses)
      m_player->m_pause = false;

   if (m_configOk)
      StartNetwork();

   m_statsStart = NowUsec();
   if (!watch)
      g_recordCalls = true; // the script starts right after this returns: its global code is recorded too
}

SessionImpl::~SessionImpl()
{
   g_recordCalls = false;
   g_session = nullptr;
   if (!m_watch)
      LogSentByName("whole session");
   if (m_ballPolicy && m_ballPolicy->sample)
      m_ballPolicy->sample->Release();

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
   const uint64_t t0 = __rdtsc();
   ++m_frameSeen;
   RecordImpl(obj, dispid, flags, params);
   m_frameTicks += __rdtsc() - t0;
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
   m_state.clear();
   CaptureState(m_player, m_ballIds, m_state);
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
   m_state.clear();
   CaptureState(m_player, m_ballIds, m_state);
   w.clear();
   w.u64(NowUsec());
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

void SessionImpl::SendHello(Peer& p)
{
   ByteWriter w;
   w.raw("CCST", 4);
   w.u16(PROTOCOL_VERSION);
   w.u8(m_watch ? ROLE_WATCH : ROLE_PLAY);
   w.u32(m_editCount);
   w.u64(m_hash);
   w.str(m_tableFile);
   w.str(VP_VERSION_STRING_POINTS_FULL);
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

   if (m_testStart == 0)
      m_testStart = now;
   if (m_testNext < m_testKeys.size())
      FireTestInput(now);

   if (m_listener)
      while (std::unique_ptr<TcpPeer> t = m_listener->Accept())
      {
         Peer p;
         p.addr = t->RemoteAddress();
         p.tcp = std::move(t);
         m_log.Info("connection from " + p.addr);
         SendHello(p);
         m_peers.push_back(std::move(p));
      }
   if (m_configOk && m_wsa && !m_listen && now >= m_nextRetry
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
      PlayService(p);
   m_peers.erase(std::remove_if(m_peers.begin(), m_peers.end(), [](const Peer& p) { return p.closing; }), m_peers.end());

   if (std::any_of(m_peers.begin(), m_peers.end(), [](const Peer& p) { return p.helloOk; }))
   {
      ++m_frameSeq;
      BuildFrame(m_payload, NowUsec());
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
   m_frameCalls.clear();
   m_frameCallCount = 0;
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
   m_applier->Apply(*kf.state, *kf.state, 1.f);
   const uint64_t failedBefore = m_iFailed + m_iUnresolved;
   const uint32_t replayedBefore = m_frameReplayed;
   Replay(kf);
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
            m_log.Info(Fmt("watching '%s' from %s", h.table.c_str(), p.addr.c_str()));
            Note("Castcade: watching " + h.table);
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
   p.tcp->Close();
   p.closing = true;
   m_keyframeApplied = false; // the next connection starts with a keyframe; the last picture stays meanwhile
}

void SessionImpl::StepTimeline(const uint64_t now)
{
   if (m_timeline.empty())
      return;
   int64_t render = (int64_t)now - m_offset - m_delayUsec;

   // A stalled watcher (or a stale clock offset) lets the timeline grow: jump to the newest minus the delay
   const int64_t newest = (int64_t)m_timeline.back()->time;
   if (newest - (int64_t)m_timeline.front()->time > (int64_t)TIMELINE_MAX_USEC && newest - m_delayUsec > render)
   {
      render = newest - m_delayUsec;
      if (m_iJumps++ < 3)
         m_log.Info(Fmt("timeline over 2 s (%zu frames): jumping to the newest frame minus the delay", m_timeline.size()));
   }

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
   // Calls after the state, in arrival order: a ball put finds its ball, lights change in step with the balls
   for (size_t i = 0; i <= ia; ++i)
      if (!m_timeline[i]->replayed)
         Replay(*m_timeline[i]);
   for (size_t i = 0; i < ia; ++i)
      m_timeline.pop_front();
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
      if (!m_peers.empty())
      {
         WatchService(m_peers.front(), now);
         if (m_peers.front().closing)
            m_peers.clear();
      }
   }

   m_frameReplayed = 0;
   StepTimeline(now);
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
                     "dedupe-dropped %llu, policy-ignored %llu, unencodable %llu, unknown-object %llu; OnScriptCall us/frame avg %.1f max %.1f; stored %zu",
         secs, watchers, (unsigned long long)m_iFrames, (unsigned long long)m_iFramesSent, (double)m_iFramesSent / secs, (double)m_iBytesOut / secs,
         (double)m_iBytesIn / secs, (double)m_iSeen / frames, m_iSeenMax, (unsigned long long)m_seenByClass[KC_EDITABLE],
         (unsigned long long)m_seenByClass[KC_TABLE], (unsigned long long)m_seenByClass[KC_GLOBAL], (unsigned long long)m_seenByClass[KC_BALL],
         (double)m_iQueued / frames, m_iQueuedMax, (unsigned long long)m_queuedByClass[KC_EDITABLE], (unsigned long long)m_queuedByClass[KC_TABLE],
         (unsigned long long)m_queuedByClass[KC_GLOBAL], (unsigned long long)m_queuedByClass[KC_BALL], (unsigned long long)m_iDedupe,
         (unsigned long long)m_iIgnored, (unsigned long long)m_iUnencodable, (unsigned long long)m_iUnknown, usAvg, usMax, m_stored.size()));
      m_iSeen = m_iQueued = m_iTicks = 0;
      m_iSeenMax = m_iQueuedMax = 0;
      m_iTicksMax = 0;
      m_iDedupe = m_iIgnored = m_iUnencodable = m_iUnknown = m_iFramesSent = 0;
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
                     "sound skipped %llu; frames before keyframe %llu; clock offset %lld us, delay %lld ms",
         secs, m_refused ? "refused" : (connected ? "connected" : "not connected"), (unsigned long long)m_iFrames, (unsigned long long)m_iRecv,
         (double)m_iRecv / secs, (double)m_iBytesIn / secs, (double)m_iBytesOut / secs, m_timeline.size(), span, (unsigned long long)m_iStarved,
         (unsigned long long)m_iBuffering, (unsigned long long)m_iJumps,
         (unsigned long long)(m_iReplayed[KC_EDITABLE] + m_iReplayed[KC_TABLE] + m_iReplayed[KC_GLOBAL] + m_iReplayed[KC_BALL]),
         (unsigned long long)m_iReplayed[KC_EDITABLE], (unsigned long long)m_iReplayed[KC_TABLE], (unsigned long long)m_iReplayed[KC_GLOBAL],
         (unsigned long long)m_iReplayed[KC_BALL], (double)m_iReplayedSum / frames, m_iReplayedMax, (unsigned long long)m_iFailed,
         (unsigned long long)m_iUnresolved, (unsigned long long)m_iSoundSkipped, (unsigned long long)m_iBeforeKeyframe, (long long)m_offset,
         (long long)(m_delayUsec / 1000)));
      m_iRecv = m_iBeforeKeyframe = m_iStarved = m_iBuffering = m_iJumps = 0;
      m_iFailed = m_iUnresolved = m_iSoundSkipped = 0;
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
// Test tooling: CASTCADE_TEST_INPUT (play role; off unless the variable is set).
//
// Scripted key presses for automated tests, which can count on neither the focus nor a keyboard. Each event goes
// into PinInput's queue with PushQueue(.., APP_KEYBOARD), where GetInputDeviceData puts a real key press (and the
// touch input in Player::WndProc puts its virtual ones), so ProcessKeys handles it exactly as a real key: its own
// checks first (exit, pause, tweak and debugger keys), then PinInput::FireKeyEvent, which tracks the pressed state
// and fires the table's KeyDown / KeyUp events into the script.

void SessionImpl::LoadTestInput()
{
   const std::string path = GetEnv("CASTCADE_TEST_INPUT");
   if (path.empty())
      return;
   FILE* f = nullptr;
   if (fopen_s(&f, path.c_str(), "r") != 0 || !f)
   {
      m_log.Error("test input: cannot read '" + path + "'; carrying on without scripted input");
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
      TestKey k { 0, false, std::string(), 0, lineNo };
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
         m_log.Warn(Fmt("test input line %d is not '<ms> <down|up> <start|credit|plunger|lflip|rflip|scan code>', skipped: %s", lineNo, s.c_str()));
         continue;
      }
      m_testKeys.push_back(k);
   }
   fclose(f);
   std::stable_sort(m_testKeys.begin(), m_testKeys.end(), [](const TestKey& x, const TestKey& y) { return x.ms < y.ms; });
   m_log.Info(Fmt("test input: %zu events from '%s', the last at %llu ms", m_testKeys.size(), path.c_str(),
      m_testKeys.empty() ? 0ull : (unsigned long long)m_testKeys.back().ms));
}

void SessionImpl::FireTestInput(const uint64_t now)
{
   const uint64_t elapsedMs = (now - m_testStart) / 1000;
   while (m_testNext < m_testKeys.size() && m_testKeys[m_testNext].ms <= elapsedMs)
   {
      const TestKey& k = m_testKeys[m_testNext++];
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
      m_log.Info(Fmt("test input at %llu ms (line %d, due %llu ms): %s %s, code %d", (unsigned long long)elapsedMs, k.line, (unsigned long long)k.ms,
         k.key.empty() ? "scan code" : k.key.c_str(), k.down ? "down" : "up", code));
   }
   if (m_testNext == m_testKeys.size())
      m_log.Info("test input finished");
}

} // namespace

Session* Session::Create(Player* const player)
{
   const std::string mode = GetEnv("CASTCADE_STREAM");
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

}
