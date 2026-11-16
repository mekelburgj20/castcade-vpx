// license:GPLv3+

#pragma once

// Castcade state stream: byte buffers, the VARIANT codec, message framing and the TCP transport.
// Nothing in this file or in CastcadeWire.cpp knows about VPX: it builds and is tested on its own
// (src/castcade/tests). All sockets are non-blocking and every call is made from one thread.

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// CastcadeWire.cpp includes <winsock2.h> before this header; it is built without the precompiled header
#include <windows.h>
#include <oleauto.h>

namespace castcade
{

// ---------------------------------------------------------------------------------------------------------
// Byte buffers. Little endian. Header-only so that code using only these has no link dependency.

class ByteWriter final
{
public:
   void u8(const uint8_t v) { m_buf.push_back(v); }
   void u16(const uint16_t v) { raw(&v, sizeof(v)); }
   void u32(const uint32_t v) { raw(&v, sizeof(v)); }
   void u64(const uint64_t v) { raw(&v, sizeof(v)); }
   void i32(const int32_t v) { raw(&v, sizeof(v)); }
   void f32(const float v) { raw(&v, sizeof(v)); }
   void f64(const double v) { raw(&v, sizeof(v)); }
   // Unsigned LEB128
   void varu(uint32_t v)
   {
      while (v >= 0x80)
      {
         m_buf.push_back((uint8_t)(v | 0x80));
         v >>= 7;
      }
      m_buf.push_back((uint8_t)v);
   }
   void raw(const void* const p, const size_t n)
   {
      if (n == 0)
         return;
      const size_t at = m_buf.size();
      m_buf.resize(at + n);
      memcpy(m_buf.data() + at, p, n);
   }
   // varu byte count, then the bytes
   void blob(const void* const p, const size_t n)
   {
      varu((uint32_t)n);
      raw(p, n);
   }
   void str(const std::string& s) { blob(s.data(), s.size()); }
   // varu count of UTF-16 code units, then the units
   void wstr(const wchar_t* const s, const size_t count)
   {
      varu((uint32_t)count);
      raw(s, count * sizeof(wchar_t));
   }

   const uint8_t* data() const { return m_buf.data(); }
   size_t size() const { return m_buf.size(); }
   bool empty() const { return m_buf.empty(); }
   void clear() { m_buf.clear(); }
   // Overwrite 4 bytes written earlier (a count that is only known after the items were written)
   void patch_u32(const size_t offset, const uint32_t v) { memcpy(m_buf.data() + offset, &v, sizeof(v)); }
   std::vector<uint8_t>& vec() { return m_buf; }
   const std::vector<uint8_t>& vec() const { return m_buf; }

private:
   std::vector<uint8_t> m_buf;
};

// Reads never run past the end: the first read that would sets ok() to false, returns zero, and every
// later read returns zero too. Check ok() once after parsing a whole structure.
class ByteReader final
{
public:
   ByteReader(const uint8_t* const p, const size_t n) : m_p(p), m_end(p + n) { }

   bool ok() const { return m_ok; }
   size_t remaining() const { return (size_t)(m_end - m_p); }

   uint8_t u8() { uint8_t v = 0; raw(&v, sizeof(v)); return v; }
   uint16_t u16() { uint16_t v = 0; raw(&v, sizeof(v)); return v; }
   uint32_t u32() { uint32_t v = 0; raw(&v, sizeof(v)); return v; }
   uint64_t u64() { uint64_t v = 0; raw(&v, sizeof(v)); return v; }
   int32_t i32() { int32_t v = 0; raw(&v, sizeof(v)); return v; }
   float f32() { float v = 0.f; raw(&v, sizeof(v)); return v; }
   double f64() { double v = 0.; raw(&v, sizeof(v)); return v; }
   uint32_t varu()
   {
      uint32_t v = 0;
      for (int shift = 0; shift < 35; shift += 7)
      {
         const uint8_t b = u8();
         v |= (uint32_t)(b & 0x7F) << shift;
         if (!(b & 0x80))
            return v;
      }
      m_ok = false;
      return 0;
   }
   bool raw(void* const dst, const size_t n)
   {
      if (!m_ok || n > remaining())
      {
         m_ok = false;
         if (n)
            memset(dst, 0, n);
         return false;
      }
      memcpy(dst, m_p, n);
      m_p += n;
      return true;
   }
   // Pointer to the next n bytes inside the source buffer (no copy), or nullptr
   const uint8_t* span(const size_t n)
   {
      if (!m_ok || n > remaining())
      {
         m_ok = false;
         return nullptr;
      }
      const uint8_t* const p = m_p;
      m_p += n;
      return p;
   }
   // Counterpart of ByteWriter::blob: sets size, returns the bytes (nullptr on failure)
   const uint8_t* blob(size_t& size)
   {
      size = varu();
      const uint8_t* const p = span(size);
      if (!p)
         size = 0;
      return p;
   }
   std::string str()
   {
      size_t n;
      const uint8_t* const p = blob(n);
      return p ? std::string((const char*)p, n) : std::string();
   }
   std::wstring wstr()
   {
      const size_t count = varu();
      const uint8_t* const p = span(count * sizeof(wchar_t));
      if (!p)
         return std::wstring();
      std::wstring s(count, L'\0');
      memcpy(s.data(), p, count * sizeof(wchar_t));
      return s;
   }

private:
   const uint8_t* m_p;
   const uint8_t* const m_end;
   bool m_ok = true;
};

// ---------------------------------------------------------------------------------------------------------
// VARIANT codec: the argument values VBScript passes to IDispatch::Invoke.
//
// The encoding is canonical: two values that are equal encode to the same bytes, so callers can compare
// encodings to detect an unchanged value.
//
// EncodeVariant dereferences VT_BYREF (including VT_VARIANT | VT_BYREF, repeatedly). Supported: VT_EMPTY,
// VT_NULL, VT_ERROR (the scode; VBScript passes DISP_E_PARAMNOTFOUND for an omitted optional argument),
// VT_BOOL, the integer types up to 64 bits, VT_R4, VT_R8, VT_CY, VT_DATE, VT_BSTR (a null BSTR is the empty
// string), and one-dimensional SAFEARRAYs of VARIANT or of a numeric type (the DMD pixel arrays). Arrays of
// integers that all fit a byte are stored one byte per element. It returns false, having written nothing,
// for anything else (objects, multi-dimensional arrays, records): the caller drops the whole call.
bool EncodeVariant(ByteWriter& w, const VARIANT& v);

// Decodes one value into out, which the caller has VariantInit'ed and later VariantClear's. Integers come
// back as VT_I4 where they fit (VT_I8 otherwise), reals as VT_R8 (VT_R4 if encoded from VT_R4), arrays as a
// SAFEARRAY of VARIANT with the original lower bound and VT_I4 / VT_R8 elements. Returns false on malformed
// input (out is then left VT_EMPTY).
bool DecodeVariant(ByteReader& r, VARIANT& out);

// ---------------------------------------------------------------------------------------------------------
// Messages.
//
// On the wire: u32 length of everything after this field, u8 type, u8 flags, payload.
// flags bit 0: the payload is u32 raw length followed by a raw deflate stream (miniz). Send() deflates
// payloads of 512 bytes or more when that makes them smaller. A length above 64 MiB closes the connection.

enum MessageType : uint8_t
{
   MSG_HELLO = 1,
   MSG_KEYFRAME = 2,
   MSG_FRAME = 3,
   MSG_BYE = 4,
};

struct Message
{
   uint8_t type = 0;
   std::vector<uint8_t> payload; // always inflated
};

// ---------------------------------------------------------------------------------------------------------
// Transport.

// Process-wide Winsock start / stop, reference counted. Listen and ConnectTcp fail before WireStartup.
bool WireStartup();
void WireShutdown();

// Monotonic microseconds (QueryPerformanceCounter)
uint64_t NowUsec();

// One TCP connection (TCP_NODELAY, non-blocking).
class TcpPeer
{
public:
   virtual ~TcpPeer() = default;

   // Frames and queues one message, then writes as much of the queue as the socket takes now. Returns false
   // if the connection is closed, or if more than 8 MiB is queued unsent (the peer is too slow: the
   // connection is then closed).
   virtual bool Send(uint8_t type, const uint8_t* payload, size_t size) = 0;

   // Completes a pending connect, flushes queued output, reads everything available and appends each
   // complete message to out. Returns false once the connection is closed or failed; messages read before
   // that are still delivered. Never blocks.
   virtual bool Poll(std::vector<Message>& out) = 0;

   // False while a connect is still in progress and after the connection closed
   virtual bool Connected() const = 0;
   // True from creation until the connection closed or failed (so: connecting or connected)
   virtual bool Alive() const = 0;

   virtual void Close() = 0;

   // "host:port" of the other end
   virtual std::string RemoteAddress() const = 0;

   virtual uint64_t BytesSent() const = 0; // bytes handed to the socket, after framing and deflate
   virtual uint64_t BytesReceived() const = 0;
};

class TcpListener
{
public:
   virtual ~TcpListener() = default;
   // Next waiting connection, or nullptr. Never blocks.
   virtual std::unique_ptr<TcpPeer> Accept() = 0;
   virtual uint16_t Port() const = 0;
};

// Listens on every IPv4 interface, or only on bindAddress (an IPv4 literal such as "127.0.0.1": a listener
// that is only reachable from this PC does not raise the Windows Firewall prompt, which tests rely on).
// Returns nullptr and sets err on failure (port in use, ...).
std::unique_ptr<TcpListener> ListenTcp(uint16_t port, std::string& err, const char* bindAddress = nullptr);

// Starts a connection to host:port (an IPv4 literal or a name; the name lookup itself may block for a
// moment). The returned peer is Alive() and not yet Connected(); Poll() completes or fails it. Returns
// nullptr and sets err if the address cannot be resolved or the socket cannot be created.
std::unique_ptr<TcpPeer> ConnectTcp(const std::string& host, uint16_t port, std::string& err);

}
