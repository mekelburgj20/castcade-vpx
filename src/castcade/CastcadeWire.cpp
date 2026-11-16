// license:GPLv3+

// Built without the precompiled header (see the CMake lists): this file must not depend on VPX.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "CastcadeWire.h"

#include "miniz/miniz.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iterator>
#include <mutex>
#include <share.h>
#include <thread>

namespace castcade
{

// ---------------------------------------------------------------------------------------------------------
// VARIANT codec

namespace
{

enum Tag : uint8_t
{
   TAG_EMPTY = 0,
   TAG_NULL = 1,
   TAG_ERROR = 2,
   TAG_FALSE = 3,
   TAG_TRUE = 4,
   TAG_I32 = 5,
   TAG_I64 = 6,
   TAG_R4 = 7,
   TAG_R8 = 8,
   TAG_CY = 9,
   TAG_DATE = 10,
   TAG_BSTR = 11,
   TAG_ARRAY = 12,
};

enum ArrayFormat : uint8_t
{
   FMT_U8 = 0,
   FMT_I32 = 1,
   FMT_R8 = 2,
   FMT_VARIANTS = 3,
};

constexpr int MAX_DEPTH = 8;
constexpr size_t MAX_ARRAY_ELEMENTS = 64u * 1024u * 1024u;

// 0: not a number we handle, 1: integer (in i), 2: real (in d). pv points at the value itself.
int ReadNumRaw(const VARTYPE base, const void* const pv, int64_t& i, double& d)
{
   switch (base)
   {
   case VT_I1: i = *(const int8_t*)pv; return 1;
   case VT_UI1: i = *(const uint8_t*)pv; return 1;
   case VT_I2: i = *(const int16_t*)pv; return 1;
   case VT_UI2: i = *(const uint16_t*)pv; return 1;
   case VT_I4:
   case VT_INT: i = *(const int32_t*)pv; return 1;
   case VT_UI4:
   case VT_UINT: i = *(const uint32_t*)pv; return 1;
   case VT_I8: i = *(const int64_t*)pv; return 1;
   case VT_UI8:
   {
      const uint64_t u = *(const uint64_t*)pv;
      if (u > (uint64_t)INT64_MAX)
         return 0;
      i = (int64_t)u;
      return 1;
   }
   case VT_R4: d = *(const float*)pv; return 2;
   case VT_R8: d = *(const double*)pv; return 2;
   default: return 0;
   }
}

// Follows VT_VARIANT | VT_BYREF chains. Returns nullptr for a dangling reference.
const VARIANT* Resolve(const VARIANT* p)
{
   for (int n = 0; p && V_VT(p) == (VT_VARIANT | VT_BYREF); ++n)
   {
      if (n > 32)
         return nullptr;
      p = V_VARIANTREF(p);
   }
   return p;
}

// Pointer to the value of a resolved, non-array VARIANT (nullptr for a null reference)
const void* ValuePtr(const VARIANT* const p)
{
   return (V_VT(p) & VT_BYREF) ? (const void*)V_BYREF(p) : (const void*)&p->llVal;
}

void WriteInt(ByteWriter& w, const int64_t i)
{
   if (i >= INT32_MIN && i <= INT32_MAX)
   {
      w.u8(TAG_I32);
      w.i32((int32_t)i);
   }
   else
   {
      w.u8(TAG_I64);
      w.u64((uint64_t)i);
   }
}

bool EncodeImpl(ByteWriter& w, const VARIANT& in, const int depth);

bool EncodeArray(ByteWriter& w, SAFEARRAY* const psa, const VARTYPE et, const int depth)
{
   if (!psa || SafeArrayGetDim(psa) != 1)
      return false;

   size_t cb;
   switch (et)
   {
   case VT_VARIANT: cb = sizeof(VARIANT); break;
   case VT_UI1:
   case VT_I1: cb = 1; break;
   case VT_I2:
   case VT_UI2: cb = 2; break;
   case VT_I4:
   case VT_UI4:
   case VT_INT:
   case VT_UINT:
   case VT_R4: cb = 4; break;
   case VT_I8:
   case VT_R8: cb = 8; break;
   default: return false;
   }
   if (psa->cbElements != cb)
      return false;

   const size_t count = psa->rgsabound[0].cElements;
   const int32_t lbound = psa->rgsabound[0].lLbound;
   if (count > MAX_ARRAY_ELEMENTS)
      return false;

   void* data = nullptr;
   if (count > 0 && (FAILED(SafeArrayAccessData(psa, &data)) || !data))
      return false;

   const uint8_t* const bytes = (const uint8_t*)data;
   bool ok = true;

   // Pass 1: classify
   bool allU8 = true, allI32 = true, allNum = true, anyReal = false;
   if (et == VT_UI1)
   {
      // always U8
   }
   else
   {
      for (size_t k = 0; k < count; ++k)
      {
         int64_t i = 0;
         double d = 0.;
         int kind;
         if (et == VT_VARIANT)
         {
            const VARIANT* const e = Resolve((const VARIANT*)(bytes + k * cb));
            if (!e)
            {
               ok = false;
               break;
            }
            const VARTYPE evt = V_VT(e);
            if (evt == VT_EMPTY)
            {
               kind = 1;
            }
            else if (evt & (VT_ARRAY | VT_VECTOR))
            {
               kind = 0;
            }
            else
            {
               const void* const pv = ValuePtr(e);
               kind = pv ? ReadNumRaw((VARTYPE)(evt & VT_TYPEMASK), pv, i, d) : 0;
            }
         }
         else
            kind = ReadNumRaw(et, bytes + k * cb, i, d);

         if (kind == 0)
         {
            allNum = false;
            break;
         }
         if (kind == 2)
            anyReal = true;
         else
         {
            if (i < 0 || i > 255)
               allU8 = false;
            if (i < INT32_MIN || i > INT32_MAX)
               allI32 = false;
         }
      }
   }

   ArrayFormat fmt;
   if (et == VT_UI1)
      fmt = FMT_U8;
   else if (!allNum)
      fmt = FMT_VARIANTS;
   else if (anyReal)
      fmt = FMT_R8;
   else if (allU8)
      fmt = FMT_U8;
   else if (allI32)
      fmt = FMT_I32;
   else
      fmt = FMT_VARIANTS;

   if (ok)
   {
      w.u8(TAG_ARRAY);
      w.i32(lbound);
      w.varu((uint32_t)count);
      w.u8(fmt);

      if (et == VT_UI1)
      {
         w.raw(bytes, count);
      }
      else if (fmt == FMT_VARIANTS)
      {
         for (size_t k = 0; k < count && ok; ++k)
         {
            if (et == VT_VARIANT)
               ok = EncodeImpl(w, *(const VARIANT*)(bytes + k * cb), depth + 1);
            else
            {
               VARIANT tmp;
               memset(&tmp, 0, sizeof(tmp));
               V_VT(&tmp) = et;
               memcpy(&tmp.llVal, bytes + k * cb, cb);
               ok = EncodeImpl(w, tmp, depth + 1);
            }
         }
      }
      else
      {
         const size_t esize = (fmt == FMT_U8) ? 1 : (fmt == FMT_I32) ? 4 : 8;
         const size_t at = w.size();
         w.vec().resize(at + count * esize);
         uint8_t* out = w.vec().data() + at;
         for (size_t k = 0; k < count; ++k)
         {
            int64_t i = 0;
            double d = 0.;
            int kind;
            if (et == VT_VARIANT)
            {
               const VARIANT* const e = Resolve((const VARIANT*)(bytes + k * cb));
               if (V_VT(e) == VT_EMPTY)
                  kind = 1;
               else
                  kind = ReadNumRaw((VARTYPE)(V_VT(e) & VT_TYPEMASK), ValuePtr(e), i, d);
            }
            else
               kind = ReadNumRaw(et, bytes + k * cb, i, d);

            if (fmt == FMT_U8)
               *out++ = (uint8_t)i;
            else if (fmt == FMT_I32)
            {
               const int32_t v = (int32_t)i;
               memcpy(out, &v, 4);
               out += 4;
            }
            else
            {
               const double v = (kind == 2) ? d : (double)i;
               memcpy(out, &v, 8);
               out += 8;
            }
         }
      }
   }

   if (count > 0)
      SafeArrayUnaccessData(psa);
   return ok;
}

bool EncodeImpl(ByteWriter& w, const VARIANT& in, const int depth)
{
   if (depth > MAX_DEPTH)
      return false;
   const VARIANT* const p = Resolve(&in);
   if (!p)
      return false;

   const VARTYPE vt = V_VT(p);
   if (vt & (VT_VECTOR | VT_RESERVED))
      return false;
   const bool byref = (vt & VT_BYREF) != 0;
   const VARTYPE base = (VARTYPE)(vt & VT_TYPEMASK);

   if (vt & VT_ARRAY)
   {
      SAFEARRAY* psa;
      if (byref)
      {
         SAFEARRAY* const* const pp = V_ARRAYREF(p);
         if (!pp)
            return false;
         psa = *pp;
      }
      else
         psa = V_ARRAY(p);
      return EncodeArray(w, psa, base, depth);
   }

   if (base == VT_EMPTY || base == VT_NULL)
   {
      if (byref)
         return false;
      w.u8(base == VT_EMPTY ? TAG_EMPTY : TAG_NULL);
      return true;
   }

   const void* const pv = ValuePtr(p);
   if (!pv)
      return false;

   int64_t i = 0;
   double d = 0.;
   switch (base)
   {
   case VT_I1:
   case VT_UI1:
   case VT_I2:
   case VT_UI2:
   case VT_I4:
   case VT_UI4:
   case VT_INT:
   case VT_UINT:
   case VT_I8:
   case VT_UI8:
      if (ReadNumRaw(base, pv, i, d) != 1)
         return false;
      WriteInt(w, i);
      return true;
   case VT_BOOL:
      w.u8(*(const VARIANT_BOOL*)pv != VARIANT_FALSE ? TAG_TRUE : TAG_FALSE);
      return true;
   case VT_R4:
      w.u8(TAG_R4);
      w.f32(*(const float*)pv);
      return true;
   case VT_R8:
      w.u8(TAG_R8);
      w.f64(*(const double*)pv);
      return true;
   case VT_ERROR:
      w.u8(TAG_ERROR);
      w.i32((int32_t)*(const SCODE*)pv);
      return true;
   case VT_CY:
      w.u8(TAG_CY);
      w.u64((uint64_t)((const CY*)pv)->int64);
      return true;
   case VT_DATE:
      w.u8(TAG_DATE);
      w.f64(*(const DATE*)pv);
      return true;
   case VT_BSTR:
   {
      const BSTR s = *(const BSTR*)pv;
      w.u8(TAG_BSTR);
      w.wstr(s, s ? SysStringLen(s) : 0);
      return true;
   }
   default:
      return false; // VT_DECIMAL, VT_DISPATCH, VT_UNKNOWN, records, ...
   }
}

bool DecodeImpl(ByteReader& r, VARIANT& out, const int depth);

bool DecodeArray(ByteReader& r, VARIANT& out, const int depth)
{
   const int32_t lbound = r.i32();
   const size_t count = r.varu();
   const uint8_t fmt = r.u8();
   if (!r.ok())
      return false;

   size_t esize;
   switch (fmt)
   {
   case FMT_U8: esize = 1; break;
   case FMT_I32: esize = 4; break;
   case FMT_R8: esize = 8; break;
   case FMT_VARIANTS: esize = 1; break; // every encoded element takes at least its tag byte
   default: return false;
   }
   if (count > MAX_ARRAY_ELEMENTS || count * esize > r.remaining())
      return false;

   SAFEARRAY* const psa = SafeArrayCreateVector(VT_VARIANT, lbound, (ULONG)count);
   if (!psa)
      return false;

   VARIANT* e = nullptr;
   if (count > 0 && (FAILED(SafeArrayAccessData(psa, (void**)&e)) || !e))
   {
      SafeArrayDestroy(psa);
      return false;
   }

   bool ok = true;
   if (fmt == FMT_VARIANTS)
   {
      for (size_t k = 0; k < count; ++k)
         if (!DecodeImpl(r, e[k], depth + 1))
         {
            ok = false;
            break;
         }
   }
   else
   {
      const uint8_t* const d = r.span(count * esize);
      if (!d && count > 0)
         ok = false;
      else
         for (size_t k = 0; k < count; ++k)
         {
            if (fmt == FMT_U8)
            {
               V_VT(&e[k]) = VT_I4;
               V_I4(&e[k]) = d[k];
            }
            else if (fmt == FMT_I32)
            {
               int32_t v;
               memcpy(&v, d + k * 4, 4);
               V_VT(&e[k]) = VT_I4;
               V_I4(&e[k]) = v;
            }
            else
            {
               double v;
               memcpy(&v, d + k * 8, 8);
               V_VT(&e[k]) = VT_R8;
               V_R8(&e[k]) = v;
            }
         }
   }

   if (count > 0)
      SafeArrayUnaccessData(psa);
   if (!ok)
   {
      SafeArrayDestroy(psa); // clears the elements decoded so far
      return false;
   }
   V_VT(&out) = VT_ARRAY | VT_VARIANT;
   V_ARRAY(&out) = psa;
   return true;
}

bool DecodeImpl(ByteReader& r, VARIANT& out, const int depth)
{
   if (depth > MAX_DEPTH)
      return false;
   const uint8_t tag = r.u8();
   if (!r.ok())
      return false;

   switch (tag)
   {
   case TAG_EMPTY:
      V_VT(&out) = VT_EMPTY;
      return true;
   case TAG_NULL:
      V_VT(&out) = VT_NULL;
      return true;
   case TAG_ERROR:
   {
      const int32_t v = r.i32();
      if (!r.ok())
         return false;
      V_VT(&out) = VT_ERROR;
      V_ERROR(&out) = (SCODE)v;
      return true;
   }
   case TAG_FALSE:
   case TAG_TRUE:
      V_VT(&out) = VT_BOOL;
      V_BOOL(&out) = (tag == TAG_TRUE) ? VARIANT_TRUE : VARIANT_FALSE;
      return true;
   case TAG_I32:
   {
      const int32_t v = r.i32();
      if (!r.ok())
         return false;
      V_VT(&out) = VT_I4;
      V_I4(&out) = v;
      return true;
   }
   case TAG_I64:
   {
      const uint64_t v = r.u64();
      if (!r.ok())
         return false;
      V_VT(&out) = VT_I8;
      V_I8(&out) = (LONGLONG)v;
      return true;
   }
   case TAG_R4:
   {
      const float v = r.f32();
      if (!r.ok())
         return false;
      V_VT(&out) = VT_R4;
      V_R4(&out) = v;
      return true;
   }
   case TAG_R8:
   {
      const double v = r.f64();
      if (!r.ok())
         return false;
      V_VT(&out) = VT_R8;
      V_R8(&out) = v;
      return true;
   }
   case TAG_CY:
   {
      const uint64_t v = r.u64();
      if (!r.ok())
         return false;
      V_VT(&out) = VT_CY;
      V_CY(&out).int64 = (LONGLONG)v;
      return true;
   }
   case TAG_DATE:
   {
      const double v = r.f64();
      if (!r.ok())
         return false;
      V_VT(&out) = VT_DATE;
      V_DATE(&out) = v;
      return true;
   }
   case TAG_BSTR:
   {
      const uint32_t count = r.varu();
      const uint8_t* const d = r.span((size_t)count * sizeof(wchar_t));
      if (!r.ok())
         return false;
      BSTR s = SysAllocStringLen(nullptr, count);
      if (!s)
         return false;
      if (count > 0)
         memcpy(s, d, (size_t)count * sizeof(wchar_t));
      V_VT(&out) = VT_BSTR;
      V_BSTR(&out) = s;
      return true;
   }
   case TAG_ARRAY:
      return DecodeArray(r, out, depth);
   default:
      return false;
   }
}

} // namespace

bool EncodeVariant(ByteWriter& w, const VARIANT& v)
{
   const size_t start = w.size();
   if (EncodeImpl(w, v, 0))
      return true;
   w.vec().resize(start);
   return false;
}

bool DecodeVariant(ByteReader& r, VARIANT& out)
{
   VARIANT tmp;
   VariantInit(&tmp);
   if (!DecodeImpl(r, tmp, 0))
   {
      VariantClear(&tmp);
      V_VT(&out) = VT_EMPTY;
      return false;
   }
   out = tmp;
   return true;
}

// ---------------------------------------------------------------------------------------------------------
// Transport

namespace
{

constexpr uint32_t MAX_MESSAGE = 64u * 1024u * 1024u;
constexpr size_t MAX_BACKLOG = 8u * 1024u * 1024u;
constexpr size_t DEFLATE_MIN = 512;
constexpr int DEFLATE_LEVEL = 1;

int g_wsaRefs = 0;

std::string WsaError(const char* const what, const int code)
{
   char buf[160];
   snprintf(buf, sizeof(buf), "%s failed (WSA error %d)", what, code);
   return buf;
}

bool SetNonBlocking(const SOCKET s)
{
   u_long on = 1;
   return ioctlsocket(s, FIONBIO, &on) == 0;
}

void SetNoDelay(const SOCKET s)
{
   BOOL on = TRUE;
   setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
}

std::string AddrToString(const sockaddr_in& a)
{
   char ip[INET_ADDRSTRLEN] = {};
   inet_ntop(AF_INET, (void*)&a.sin_addr, ip, sizeof(ip));
   return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

} // namespace

bool FrameMessage(std::vector<uint8_t>& out, const uint8_t type, const uint8_t* const payload, const size_t size)
{
   if (size > MAX_MESSAGE - 16)
      return false;

   bool deflated = false;
   const size_t headerAt = out.size();
   out.resize(headerAt + 4 + 2);

   if (size >= DEFLATE_MIN)
   {
      // The result must beat the raw payload including the 4 byte raw length that precedes it
      const size_t limit = size - 4;
      const size_t at = out.size();
      out.resize(at + 4 + limit);
      const mz_uint flags = tdefl_create_comp_flags_from_zip_params(DEFLATE_LEVEL, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
      const size_t n = tdefl_compress_mem_to_mem(out.data() + at + 4, limit, payload, size, (int)flags);
      if (n != 0 && n < limit)
      {
         const uint32_t rawLen = (uint32_t)size;
         memcpy(out.data() + at, &rawLen, 4);
         out.resize(at + 4 + n);
         deflated = true;
      }
      else
         out.resize(at);
   }
   if (!deflated && size > 0)
   {
      const size_t at = out.size();
      out.resize(at + size);
      memcpy(out.data() + at, payload, size);
   }

   const uint32_t len = (uint32_t)(out.size() - headerAt - 4);
   memcpy(out.data() + headerAt, &len, 4);
   out[headerAt + 4] = type;
   out[headerAt + 5] = deflated ? 1 : 0;
   return true;
}

namespace
{

class SocketPeer final : public TcpPeer
{
public:
   SocketPeer(const SOCKET s, const bool connecting, std::string remote)
      : m_sock(s)
      , m_connecting(connecting)
      , m_remote(std::move(remote))
   {
   }

   ~SocketPeer() override { Close(); }

   bool Send(const uint8_t type, const uint8_t* const payload, const size_t size) override
   {
      if (m_sock == INVALID_SOCKET)
         return false;
      if (!FrameMessage(m_out, type, payload, size))
         return false; // cannot be framed within the limit; the connection stays up

      if (m_connecting)
      {
         if (!Backlogged())
            return true;
         Close();
         return false;
      }
      if (!Flush())
         return false;
      if (Backlogged())
      {
         Close();
         return false;
      }
      return true;
   }

   bool Poll(std::vector<Message>& out) override
   {
      if (m_sock == INVALID_SOCKET)
         return false;

      if (m_connecting)
      {
         fd_set wr, ex;
         FD_ZERO(&wr);
         FD_ZERO(&ex);
         FD_SET(m_sock, &wr);
         FD_SET(m_sock, &ex);
         const timeval tv = { 0, 0 };
         const int n = select(0, nullptr, &wr, &ex, &tv);
         if (n == SOCKET_ERROR)
         {
            Close();
            return false;
         }
         if (n == 0)
            return true; // still connecting
         int soErr = 0;
         int len = sizeof(soErr);
         if (FD_ISSET(m_sock, &ex) || getsockopt(m_sock, SOL_SOCKET, SO_ERROR, (char*)&soErr, &len) != 0 || soErr != 0
            || !FD_ISSET(m_sock, &wr))
         {
            Close();
            return false;
         }
         m_connecting = false;
      }

      if (!Flush())
         return false;

      bool closing = false;
      uint8_t buf[65536];
      for (;;)
      {
         const int n = recv(m_sock, (char*)buf, (int)sizeof(buf), 0);
         if (n > 0)
         {
            m_in.insert(m_in.end(), buf, buf + n);
            m_received += (uint64_t)n;
            continue;
         }
         if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
            closing = true;
         break;
      }

      if (!Parse(out))
         closing = true;
      if (closing)
      {
         Close();
         return false;
      }
      return true;
   }

   bool Connected() const override { return m_sock != INVALID_SOCKET && !m_connecting; }
   bool Alive() const override { return m_sock != INVALID_SOCKET; }

   void Close() override
   {
      if (m_sock != INVALID_SOCKET)
      {
         closesocket(m_sock);
         m_sock = INVALID_SOCKET;
      }
      m_connecting = false;
      std::vector<uint8_t>().swap(m_out);
      m_outPos = 0;
      std::vector<uint8_t>().swap(m_in);
   }

   std::string RemoteAddress() const override { return m_remote; }
   uint64_t BytesSent() const override { return m_sent; }
   uint64_t BytesReceived() const override { return m_received; }

private:
   bool Backlogged() const { return m_out.size() - m_outPos > MAX_BACKLOG; }

   // false (and closed) on a socket error
   bool Flush()
   {
      while (m_outPos < m_out.size())
      {
         const size_t left = m_out.size() - m_outPos;
         const int n = send(m_sock, (const char*)m_out.data() + m_outPos, (int)std::min<size_t>(left, 1u << 20), 0);
         if (n > 0)
         {
            m_outPos += (size_t)n;
            m_sent += (uint64_t)n;
         }
         else if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
            break;
         else
         {
            Close();
            return false;
         }
      }
      if (m_outPos == m_out.size())
      {
         m_out.clear();
         m_outPos = 0;
      }
      else if (m_outPos >= (1u << 20) && m_outPos * 2 >= m_out.size())
      {
         m_out.erase(m_out.begin(), m_out.begin() + (ptrdiff_t)m_outPos);
         m_outPos = 0;
      }
      return true;
   }

   // false on a protocol violation
   bool Parse(std::vector<Message>& out)
   {
      size_t pos = 0;
      bool ok = true;
      while (m_in.size() - pos >= 4)
      {
         uint32_t len;
         memcpy(&len, m_in.data() + pos, 4);
         if (len < 2 || len > MAX_MESSAGE)
         {
            ok = false;
            break;
         }
         if (m_in.size() - pos - 4 < len)
            break;

         const uint8_t* const body = m_in.data() + pos + 4;
         const uint8_t type = body[0];
         const uint8_t flags = body[1];
         if (flags & ~1u)
         {
            ok = false;
            break;
         }

         Message m;
         m.type = type;
         const size_t plen = len - 2;
         if (flags & 1)
         {
            if (plen < 4)
            {
               ok = false;
               break;
            }
            uint32_t rawLen;
            memcpy(&rawLen, body + 2, 4);
            if (rawLen > MAX_MESSAGE)
            {
               ok = false;
               break;
            }
            m.payload.resize(rawLen);
            const size_t got = tinfl_decompress_mem_to_mem(m.payload.data(), rawLen, body + 6, plen - 4, 0);
            if (got != (size_t)rawLen)
            {
               ok = false;
               break;
            }
         }
         else
            m.payload.assign(body + 2, body + 2 + plen);

         out.push_back(std::move(m));
         pos += 4 + len;
      }
      if (pos > 0)
         m_in.erase(m_in.begin(), m_in.begin() + (ptrdiff_t)pos);
      return ok;
   }

   SOCKET m_sock;
   bool m_connecting;
   std::string m_remote;
   std::vector<uint8_t> m_out;
   size_t m_outPos = 0;
   std::vector<uint8_t> m_in;
   uint64_t m_sent = 0;
   uint64_t m_received = 0;
};

class SocketListener final : public TcpListener
{
public:
   SocketListener(const SOCKET s, const uint16_t port)
      : m_sock(s)
      , m_port(port)
   {
   }
   ~SocketListener() override
   {
      if (m_sock != INVALID_SOCKET)
         closesocket(m_sock);
   }

   std::unique_ptr<TcpPeer> Accept() override
   {
      sockaddr_in a = {};
      int alen = sizeof(a);
      const SOCKET s = accept(m_sock, (sockaddr*)&a, &alen);
      if (s == INVALID_SOCKET)
         return nullptr;
      if (!SetNonBlocking(s))
      {
         closesocket(s);
         return nullptr;
      }
      SetNoDelay(s);
      return std::make_unique<SocketPeer>(s, false, AddrToString(a));
   }

   uint16_t Port() const override { return m_port; }

private:
   SOCKET m_sock;
   uint16_t m_port;
};

} // namespace

bool WireStartup()
{
   if (g_wsaRefs == 0)
   {
      WSADATA wsa;
      if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
         return false;
   }
   ++g_wsaRefs;
   return true;
}

void WireShutdown()
{
   if (g_wsaRefs <= 0)
      return;
   if (--g_wsaRefs == 0)
      WSACleanup();
}

uint64_t NowUsec()
{
   static const LONGLONG freq = []
   {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      return f.QuadPart > 0 ? f.QuadPart : 1;
   }();
   LARGE_INTEGER c;
   QueryPerformanceCounter(&c);
   const uint64_t ticks = (uint64_t)c.QuadPart;
   const uint64_t secs = ticks / (uint64_t)freq;
   const uint64_t rem = ticks % (uint64_t)freq;
   return secs * 1000000ull + rem * 1000000ull / (uint64_t)freq;
}

std::unique_ptr<TcpListener> ListenTcp(const uint16_t port, std::string& err, const char* const bindAddress)
{
   sockaddr_in a = {};
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = htonl(INADDR_ANY);
   a.sin_port = htons(port);
   if (bindAddress && bindAddress[0] && inet_pton(AF_INET, bindAddress, &a.sin_addr) != 1)
   {
      err = std::string("not an IPv4 address to listen on: ") + bindAddress;
      return nullptr;
   }

   const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   if (s == INVALID_SOCKET)
   {
      err = WsaError("socket", WSAGetLastError());
      return nullptr;
   }
   BOOL on = TRUE;
   setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&on, sizeof(on));

   if (bind(s, (const sockaddr*)&a, sizeof(a)) != 0)
   {
      err = WsaError("bind", WSAGetLastError());
      closesocket(s);
      return nullptr;
   }
   if (listen(s, 8) != 0 || !SetNonBlocking(s))
   {
      err = WsaError("listen", WSAGetLastError());
      closesocket(s);
      return nullptr;
   }

   uint16_t actual = port;
   if (port == 0)
   {
      sockaddr_in b = {};
      int blen = sizeof(b);
      if (getsockname(s, (sockaddr*)&b, &blen) == 0)
         actual = ntohs(b.sin_port);
   }
   return std::make_unique<SocketListener>(s, actual);
}

std::unique_ptr<TcpPeer> ConnectTcp(const std::string& host, const uint16_t port, std::string& err)
{
   addrinfo hints = {};
   hints.ai_family = AF_INET;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_protocol = IPPROTO_TCP;
   addrinfo* res = nullptr;
   const int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
   if (rc != 0 || !res)
   {
      err = WsaError("resolving the address", rc != 0 ? rc : WSAHOST_NOT_FOUND);
      if (res)
         freeaddrinfo(res);
      return nullptr;
   }

   const sockaddr_in addr = *(const sockaddr_in*)res->ai_addr;
   freeaddrinfo(res);

   const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   if (s == INVALID_SOCKET)
   {
      err = WsaError("socket", WSAGetLastError());
      return nullptr;
   }
   if (!SetNonBlocking(s))
   {
      err = WsaError("ioctlsocket", WSAGetLastError());
      closesocket(s);
      return nullptr;
   }
   SetNoDelay(s);

   if (connect(s, (const sockaddr*)&addr, sizeof(addr)) != 0)
   {
      const int e = WSAGetLastError();
      if (e != WSAEWOULDBLOCK)
      {
         err = WsaError("connect", e);
         closesocket(s);
         return nullptr;
      }
   }
   return std::make_unique<SocketPeer>(s, true, host + ":" + std::to_string(port));
}

// ---------------------------------------------------------------------------------------------------------
// Recording

namespace
{

constexpr size_t RECORDING_MAX_STRING = 4096;
constexpr DWORD RECORDING_FLUSH_MS = 1000;

std::string Win32Error(const char* const what, const DWORD code)
{
   char buf[160];
   snprintf(buf, sizeof(buf), "%s failed (Windows error %lu)", what, (unsigned long)code);
   return buf;
}

class FileRecorder final : public StreamRecorder
{
public:
   FileRecorder(const HANDLE file, std::wstring path, RecordingInfo info)
      : m_file(file)
      , m_path(std::move(path))
      , m_info(std::move(info))
   {
      m_thread = std::thread([this] { Run(); });
   }

   ~FileRecorder() override { Close(); }

   bool Write(const uint8_t type, const uint8_t* const payload, const size_t size) override
   {
      if (m_closed || m_failed.load())
         return false;
      const uint64_t now = NowUsec();
      m_chunk.clear();
      if (m_chunks == 0)
      {
         m_t0 = now;
         FILETIME ft;
         GetSystemTimeAsFileTime(&ft);
         AppendHeader(m_chunk, ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
      }
      const size_t at = m_chunk.size();
      m_chunk.resize(at + 12);
      if (!FrameMessage(m_chunk, type, payload, size))
      {
         Fail("a message too large to record (" + std::to_string(size) + " bytes)");
         return false;
      }
      const uint64_t t = now - m_t0;
      const uint32_t n = (uint32_t)(m_chunk.size() - at - 12);
      memcpy(m_chunk.data() + at, &t, 8);
      memcpy(m_chunk.data() + at + 8, &n, 4);
      {
         std::lock_guard<std::mutex> lock(m_mutex);
         if (m_pending.size() + m_chunk.size() > RECORDING_MAX_QUEUED)
         {
            m_pending.clear();
            m_pending.shrink_to_fit();
            m_error = "the disk did not keep up: more than " + std::to_string(RECORDING_MAX_QUEUED >> 20) + " MiB waiting to be written";
            m_failed = true;
            return false;
         }
         m_pending.insert(m_pending.end(), m_chunk.begin(), m_chunk.end());
      }
      ++m_chunks;
      m_bytes += m_chunk.size();
      m_last = t;
      return true;
   }

   void Close() override
   {
      if (m_closed)
         return;
      m_closed = true;
      {
         std::lock_guard<std::mutex> lock(m_mutex);
         m_stop = true;
      }
      m_cv.notify_all();
      if (m_thread.joinable())
         m_thread.join();
      if (m_chunks == 0 && !m_failed)
      {
         CloseHandle(m_file);
         DeleteFileW(m_path.c_str());
         return;
      }
      if (!m_failed)
      {
         // Duration and chunk count, at their fixed offsets
         uint8_t tail[12];
         memcpy(tail, &m_last, 8);
         const uint32_t count = (uint32_t)std::min<uint64_t>(m_chunks, 0xFFFFFFFFu);
         memcpy(tail + 8, &count, 4);
         LARGE_INTEGER pos;
         pos.QuadPart = 24;
         DWORD wrote = 0;
         if (!SetFilePointerEx(m_file, pos, nullptr, FILE_BEGIN) || !WriteFile(m_file, tail, sizeof(tail), &wrote, nullptr) || wrote != sizeof(tail))
            m_error = Win32Error("completing the header", GetLastError());
      }
      CloseHandle(m_file);
   }

   std::string Error() const override
   {
      std::lock_guard<std::mutex> lock(m_mutex);
      return m_error;
   }
   uint64_t Chunks() const override { return m_chunks; }
   uint64_t FileBytes() const override { return m_bytes; }
   uint64_t DurationUsec() const override { return m_last; }

private:
   void AppendHeader(std::vector<uint8_t>& out, const uint64_t fileTime) const
   {
      ByteWriter w;
      w.raw("CCSTREC\x1A", 8);
      w.u16(RECORDING_FORMAT);
      w.u16(0); // header size, set below
      w.u16(m_info.protocol);
      w.u16(0);
      w.u64(fileTime);
      w.u64(0); // duration, set by Close
      w.u32(0); // chunk count, set by Close
      w.u32(m_info.partCount);
      w.u64(m_info.hash);
      for (const std::string* const s : { &m_info.table, &m_info.build })
      {
         const uint16_t n = (uint16_t)std::min<size_t>(s->size(), RECORDING_MAX_STRING);
         w.u16(n);
         w.raw(s->data(), n);
      }
      const uint16_t size = (uint16_t)w.size();
      memcpy(w.vec().data() + 10, &size, 2);
      out.insert(out.end(), w.data(), w.data() + w.size());
   }

   void Fail(const std::string& why)
   {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_error.empty())
         m_error = why;
      m_failed = true;
   }

   // The writer thread: every second, or when closing, takes what is queued and writes it
   void Run()
   {
      std::vector<uint8_t> batch;
      std::unique_lock<std::mutex> lock(m_mutex);
      for (;;)
      {
         m_cv.wait_for(lock, std::chrono::milliseconds(RECORDING_FLUSH_MS), [this] { return m_stop; });
         batch.swap(m_pending);
         const bool stop = m_stop;
         lock.unlock();
         size_t done = 0;
         const uint64_t t0 = NowUsec();
         while (done < batch.size() && !m_failed)
         {
            DWORD wrote = 0;
            const DWORD n = (DWORD)std::min<size_t>(batch.size() - done, 1u << 24);
            if (!WriteFile(m_file, batch.data() + done, n, &wrote, nullptr) || wrote == 0)
               Fail(Win32Error("writing the recording", GetLastError()));
            done += wrote;
         }
         if (done)
            ReportIo("recorder", t0, NowUsec(), done);
         batch.clear();
         lock.lock();
         if (stop)
            break;
      }
   }

   const HANDLE m_file;
   const std::wstring m_path;
   const RecordingInfo m_info;
   std::thread m_thread;
   mutable std::mutex m_mutex;
   std::condition_variable m_cv;
   std::vector<uint8_t> m_pending; // guarded by m_mutex
   bool m_stop = false;            // guarded by m_mutex
   std::string m_error;            // guarded by m_mutex
   std::atomic<bool> m_failed { false };
   bool m_closed = false;
   // Caller's thread only
   std::vector<uint8_t> m_chunk;
   uint64_t m_t0 = 0;
   uint64_t m_last = 0;
   uint64_t m_chunks = 0;
   uint64_t m_bytes = 0;
};

} // namespace

std::unique_ptr<StreamRecorder> CreateRecorder(const std::wstring& path, const RecordingInfo& info, std::string& err)
{
   // FILE_SHARE_READ: a recording in progress can be copied or inspected
   const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
   if (file == INVALID_HANDLE_VALUE)
   {
      err = Win32Error("creating the file", GetLastError());
      return nullptr;
   }
   return std::make_unique<FileRecorder>(file, path, info);
}

// ---------------------------------------------------------------------------------------------------------
// Async line writer and the I/O observer

namespace
{

std::atomic<IoObserver> g_ioObserver { nullptr };

class FileLineWriter final : public AsyncLineWriter
{
public:
   FileLineWriter(FILE* const file, const char* const ioName, std::function<void(const std::string&)> mirror)
      : m_file(file)
      , m_ioName(ioName)
      , m_mirror(std::move(mirror))
   {
      m_thread = std::thread([this] { Run(); });
   }

   ~FileLineWriter() override
   {
      {
         std::lock_guard<std::mutex> lock(m_mutex);
         m_stop = true;
      }
      m_cv.notify_all();
      if (m_thread.joinable())
         m_thread.join();
      if (m_file)
         fclose(m_file);
   }

   void Line(std::string line) override
   {
      {
         std::lock_guard<std::mutex> lock(m_mutex);
         m_queue.push_back(std::move(line));
         ++m_queued;
      }
      m_cv.notify_one();
   }

   void Flush() override
   {
      std::unique_lock<std::mutex> lock(m_mutex);
      const uint64_t target = m_queued;
      m_cv.notify_all();
      m_done.wait(lock, [&] { return m_written >= target || m_stop; });
   }

   uint64_t LinesWritten() const override
   {
      std::lock_guard<std::mutex> lock(m_mutex);
      return m_written;
   }

private:
   void Run()
   {
      std::vector<std::string> batch;
      std::string text;
      std::unique_lock<std::mutex> lock(m_mutex);
      for (;;)
      {
         m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
         batch.assign(std::make_move_iterator(m_queue.begin()), std::make_move_iterator(m_queue.end()));
         m_queue.clear();
         const bool stop = m_stop;
         lock.unlock();
         if (!batch.empty())
         {
            if (m_file)
            {
               text.clear();
               for (const std::string& s : batch)
               {
                  text += s;
                  text += '\n';
               }
               const uint64_t t0 = NowUsec();
               fwrite(text.data(), 1, text.size(), m_file);
               fflush(m_file);
               if (m_ioName)
                  ReportIo(m_ioName, t0, NowUsec(), text.size());
            }
            if (m_mirror)
               for (const std::string& s : batch)
                  m_mirror(s);
         }
         lock.lock();
         m_written += batch.size();
         batch.clear();
         m_done.notify_all();
         if (stop && m_queue.empty())
            break;
      }
   }

   FILE* const m_file;
   const char* const m_ioName;
   const std::function<void(const std::string&)> m_mirror;
   std::thread m_thread;
   mutable std::mutex m_mutex;
   std::condition_variable m_cv, m_done;
   std::deque<std::string> m_queue; // guarded by m_mutex
   uint64_t m_queued = 0, m_written = 0; // guarded by m_mutex
   bool m_stop = false;                  // guarded by m_mutex
};

} // namespace

std::unique_ptr<AsyncLineWriter> OpenAsyncLineWriter(const std::string& path, const char* const ioName, std::function<void(const std::string&)> mirror)
{
   FILE* f = nullptr;
   if (!path.empty())
   {
      f = _fsopen(path.c_str(), "a", _SH_DENYNO);
      if (!f)
         return nullptr;
   }
   else if (!mirror)
      return nullptr;
   return std::make_unique<FileLineWriter>(f, ioName, std::move(mirror));
}

void SetIoObserver(const IoObserver observer)
{
   g_ioObserver.store(observer);
}

void ReportIo(const char* const what, const uint64_t startUsec, const uint64_t endUsec, const size_t bytes)
{
   const IoObserver o = g_ioObserver.load();
   if (o)
      o(what, startUsec, endUsec, bytes);
}

}
