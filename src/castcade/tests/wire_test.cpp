// license:GPLv3+

#include <winsock2.h>
#include <ws2tcpip.h>

#include "../CastcadeWire.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <share.h>
#include <string>
#include <thread>
#include <vector>

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
static uint8_t Rnd()
{
   g_rng ^= g_rng << 13;
   g_rng ^= g_rng >> 7;
   g_rng ^= g_rng << 17;
   return (uint8_t)(g_rng >> 24);
}

// ---------------------------------------------------------------------------------------------------------

static void TestBuffers()
{
   ByteWriter w;
   w.u8(0xAB);
   w.u16(0xBEEF);
   w.u32(0xDEADBEEF);
   w.u64(0x0123456789ABCDEFull);
   w.i32(-12345);
   w.f32(1.5f);
   w.f64(-2.25);
   const uint32_t vs[] = { 0, 127, 128, 16383, 16384, 0xFFFFFFFFu };
   for (const uint32_t v : vs)
      w.varu(v);
   const uint8_t blobData[] = { 1, 2, 3 };
   w.blob(blobData, 3);
   w.str("hello");
   w.wstr(L"wideé", 5);
   const size_t patchAt = w.size();
   w.u32(0);
   w.patch_u32(patchAt, 777);
   CHECK(!w.empty());

   ByteReader r(w.data(), w.size());
   CHECK(r.u8() == 0xAB);
   CHECK(r.u16() == 0xBEEF);
   CHECK(r.u32() == 0xDEADBEEF);
   CHECK(r.u64() == 0x0123456789ABCDEFull);
   CHECK(r.i32() == -12345);
   CHECK(r.f32() == 1.5f);
   CHECK(r.f64() == -2.25);
   for (const uint32_t v : vs)
      CHECK(r.varu() == v);
   size_t bn = 0;
   const uint8_t* bp = r.blob(bn);
   CHECK(bp && bn == 3 && bp[2] == 3);
   CHECK(r.str() == "hello");
   CHECK(r.wstr() == L"wideé");
   CHECK(r.u32() == 777);
   CHECK(r.ok());
   CHECK(r.remaining() == 0);
   CHECK(r.u32() == 0);
   CHECK(!r.ok());
   CHECK(r.u8() == 0 && r.u64() == 0 && r.f64() == 0.0);
   CHECK(r.str().empty());

   ByteWriter w2;
   w2.varu(127);
   CHECK(w2.size() == 1);
   w2.varu(128);
   CHECK(w2.size() == 3);
   w2.varu(0xFFFFFFFFu);
   CHECK(w2.size() == 8);

   const uint8_t two[] = { 1, 2 };
   ByteReader r2(two, 2);
   CHECK(r2.u32() == 0);
   CHECK(!r2.ok());
   CHECK(r2.u8() == 0); // sticky
}

// ---------------------------------------------------------------------------------------------------------

static std::vector<uint8_t> Enc(const VARIANT& v, bool* ok = nullptr)
{
   ByteWriter w;
   const bool r = EncodeVariant(w, v);
   if (ok)
      *ok = r;
   return w.vec();
}

static bool Dec(const std::vector<uint8_t>& b, VARIANT& out)
{
   VariantInit(&out);
   ByteReader r(b.data(), b.size());
   const bool ok = DecodeVariant(r, out);
   return ok && r.ok() && r.remaining() == 0;
}

static void TestVariantScalars()
{
   VARIANT v, o;
   bool ok;

   VariantInit(&v);
   {
      auto b = Enc(v, &ok);
      CHECK(ok && Dec(b, o) && V_VT(&o) == VT_EMPTY);
   }
   V_VT(&v) = VT_NULL;
   {
      auto b = Enc(v, &ok);
      CHECK(ok && Dec(b, o) && V_VT(&o) == VT_NULL);
   }
   V_VT(&v) = VT_BOOL;
   V_BOOL(&v) = VARIANT_TRUE;
   {
      auto b = Enc(v, &ok);
      CHECK(ok && Dec(b, o) && V_VT(&o) == VT_BOOL && V_BOOL(&o) == VARIANT_TRUE);
   }
   V_BOOL(&v) = VARIANT_FALSE;
   {
      auto b = Enc(v, &ok);
      CHECK(ok && Dec(b, o) && V_VT(&o) == VT_BOOL && V_BOOL(&o) == VARIANT_FALSE);
   }
   V_BOOL(&v) = 1; // nonzero but not -1 still true
   {
      auto b = Enc(v, &ok);
      CHECK(ok && Dec(b, o) && V_BOOL(&o) == VARIANT_TRUE);
   }

   VARIANT a, b2, c, d;
   VariantInit(&a); V_VT(&a) = VT_I2; V_I2(&a) = 5;
   VariantInit(&b2); V_VT(&b2) = VT_I4; V_I4(&b2) = 5;
   VariantInit(&c); V_VT(&c) = VT_UI1; V_UI1(&c) = 5;
   VariantInit(&d); V_VT(&d) = VT_I8; V_I8(&d) = 5;
   CHECK(Enc(a) == Enc(b2));
   CHECK(Enc(a) == Enc(c));
   CHECK(Enc(a) == Enc(d));
   CHECK(Enc(a).size() == 5);
   {
      auto b = Enc(a);
      CHECK(Dec(b, o) && V_VT(&o) == VT_I4 && V_I4(&o) == 5);
   }
   V_VT(&v) = VT_I4;
   V_I4(&v) = INT32_MIN;
   { auto b = Enc(v); CHECK(Dec(b, o) && V_VT(&o) == VT_I4 && V_I4(&o) == INT32_MIN); }
   V_VT(&v) = VT_UI4;
   V_UI4(&v) = 0xFFFFFFFFu;
   { auto b = Enc(v); CHECK(b.size() == 9 && Dec(b, o) && V_VT(&o) == VT_I8 && V_I8(&o) == 0xFFFFFFFFll); }
   V_VT(&v) = VT_UI4;
   V_UI4(&v) = 7;
   { auto b = Enc(v); CHECK(b.size() == 5 && Dec(b, o) && V_VT(&o) == VT_I4 && V_I4(&o) == 7); }
   V_VT(&v) = VT_I8;
   V_I8(&v) = -0x123456789ALL;
   { auto b = Enc(v); CHECK(b.size() == 9 && Dec(b, o) && V_VT(&o) == VT_I8 && V_I8(&o) == -0x123456789ALL); }
   V_VT(&v) = VT_I2;
   V_I2(&v) = -3;
   { auto b = Enc(v); CHECK(Dec(b, o) && V_VT(&o) == VT_I4 && V_I4(&o) == -3); }
   V_VT(&v) = VT_UI8;
   V_UI8(&v) = 0xFFFFFFFFFFFFFFFFull;
   Enc(v, &ok);
   CHECK(!ok);

   V_VT(&v) = VT_R8;
   V_R8(&v) = 3.14159;
   { auto b = Enc(v); CHECK(Dec(b, o) && V_VT(&o) == VT_R8 && V_R8(&o) == 3.14159); }
   V_VT(&v) = VT_R4;
   V_R4(&v) = 2.5f;
   { auto b = Enc(v); CHECK(Dec(b, o) && V_VT(&o) == VT_R4 && V_R4(&o) == 2.5f); }

   V_VT(&v) = VT_DATE;
   V_DATE(&v) = 45000.5;
   { auto b = Enc(v); CHECK(Dec(b, o) && V_VT(&o) == VT_DATE && V_DATE(&o) == 45000.5); }
   V_VT(&v) = VT_CY;
   V_CY(&v).int64 = 123456789;
   { auto b = Enc(v); CHECK(Dec(b, o) && V_VT(&o) == VT_CY && V_CY(&o).int64 == 123456789); }

   V_VT(&v) = VT_ERROR;
   V_ERROR(&v) = DISP_E_PARAMNOTFOUND;
   { auto b = Enc(v); CHECK(Dec(b, o) && V_VT(&o) == VT_ERROR && V_ERROR(&o) == DISP_E_PARAMNOTFOUND); }

   V_VT(&v) = VT_BSTR;
   V_BSTR(&v) = nullptr;
   const auto nullEnc = Enc(v);
   V_BSTR(&v) = SysAllocString(L"");
   const auto emptyEnc = Enc(v);
   CHECK(nullEnc == emptyEnc);
   CHECK(Dec(emptyEnc, o) && V_VT(&o) == VT_BSTR && V_BSTR(&o) && SysStringLen(V_BSTR(&o)) == 0);
   VariantClear(&o);
   SysFreeString(V_BSTR(&v));
   V_BSTR(&v) = SysAllocStringLen(L"a\0b", 3);
   {
      auto b = Enc(v);
      CHECK(Dec(b, o) && V_VT(&o) == VT_BSTR && SysStringLen(V_BSTR(&o)) == 3 && V_BSTR(&o)[0] == L'a' && V_BSTR(&o)[1] == 0
         && V_BSTR(&o)[2] == L'b');
      VariantClear(&o);
   }
   SysFreeString(V_BSTR(&v));
   V_BSTR(&v) = SysAllocString(L"Héllo \U0001F600");
   {
      auto b = Enc(v);
      CHECK(Dec(b, o) && V_VT(&o) == VT_BSTR && wcscmp(V_BSTR(&o), L"Héllo \U0001F600") == 0);
      VariantClear(&o);
   }
   VariantClear(&v);

   VARIANT bad;
   VariantInit(&bad);
   V_VT(&bad) = VT_DISPATCH;
   Enc(bad, &ok);
   CHECK(!ok);
   V_VT(&bad) = VT_UNKNOWN;
   Enc(bad, &ok);
   CHECK(!ok);
   V_VT(&bad) = VT_DECIMAL;
   Enc(bad, &ok);
   CHECK(!ok);

   double dbl = 2.5;
   VARIANT inner, outer;
   VariantInit(&inner);
   V_VT(&inner) = VT_R8 | VT_BYREF;
   V_R8REF(&inner) = &dbl;
   VariantInit(&outer);
   V_VT(&outer) = VT_VARIANT | VT_BYREF;
   V_VARIANTREF(&outer) = &inner;
   {
      auto b = Enc(outer, &ok);
      CHECK(ok && Dec(b, o) && V_VT(&o) == VT_R8 && V_R8(&o) == 2.5);
   }
   VARIANT outer2;
   VariantInit(&outer2);
   V_VT(&outer2) = VT_VARIANT | VT_BYREF;
   V_VARIANTREF(&outer2) = &outer;
   {
      auto b = Enc(outer2, &ok);
      CHECK(ok && Dec(b, o) && V_VT(&o) == VT_R8 && V_R8(&o) == 2.5);
   }
   V_VARIANTREF(&outer) = nullptr;
   Enc(outer, &ok);
   CHECK(!ok);

   BSTR bs = SysAllocString(L"ref");
   V_VT(&v) = VT_BSTR | VT_BYREF;
   V_BSTRREF(&v) = &bs;
   {
      auto b = Enc(v, &ok);
      CHECK(ok && Dec(b, o) && wcscmp(V_BSTR(&o), L"ref") == 0);
      VariantClear(&o);
   }
   SysFreeString(bs);
   short s = 9;
   V_VT(&v) = VT_I2 | VT_BYREF;
   V_I2REF(&v) = &s;
   {
      auto b = Enc(v, &ok);
      CHECK(ok && Dec(b, o) && V_I4(&o) == 9);
   }
}

static SAFEARRAY* MakeVariantArray(const LONG lb, const int n)
{
   return SafeArrayCreateVector(VT_VARIANT, lb, (ULONG)n);
}

static void TestArrays()
{
   bool ok;
   VARIANT v, o;

   // 4096 x VT_UI1 in a VARIANT array
   {
      SAFEARRAY* sa = MakeVariantArray(0, 4096);
      VARIANT* p;
      SafeArrayAccessData(sa, (void**)&p);
      for (int i = 0; i < 4096; ++i)
      {
         VariantInit(&p[i]);
         V_VT(&p[i]) = VT_UI1;
         V_UI1(&p[i]) = (BYTE)(i % 101);
      }
      SafeArrayUnaccessData(sa);
      VariantInit(&v);
      V_VT(&v) = VT_ARRAY | VT_VARIANT;
      V_ARRAY(&v) = sa;
      auto b = Enc(v, &ok);
      CHECK(ok);
      CHECK(b.size() >= 4096 && b.size() <= 4096 + 12);
      CHECK(Dec(b, o));
      CHECK(V_VT(&o) == (VT_ARRAY | VT_VARIANT));
      if (V_VT(&o) == (VT_ARRAY | VT_VARIANT))
      {
         SAFEARRAY* ra = V_ARRAY(&o);
         LONG lb = -1, ub = -1;
         SafeArrayGetLBound(ra, 1, &lb);
         SafeArrayGetUBound(ra, 1, &ub);
         CHECK(lb == 0 && ub == 4095);
         VARIANT* rp;
         SafeArrayAccessData(ra, (void**)&rp);
         bool all = true;
         for (int i = 0; i < 4096; ++i)
            all = all && V_UI4(&rp[i]) == (uint32_t)(i % 101);
         CHECK(all);
         SafeArrayUnaccessData(ra);
      }
      VariantClear(&o);
      VariantClear(&v);
   }

   // 4096 x VT_I4 up to 0xFFFFFF, lower bound 1
   {
      SAFEARRAY* sa = MakeVariantArray(1, 4096);
      VARIANT* p;
      SafeArrayAccessData(sa, (void**)&p);
      for (int i = 0; i < 4096; ++i)
      {
         VariantInit(&p[i]);
         V_VT(&p[i]) = VT_I4;
         V_I4(&p[i]) = (i * 4095) & 0xFFFFFF;
      }
      V_I4(&p[7]) = 0xFFFFFF;
      SafeArrayUnaccessData(sa);
      VariantInit(&v);
      V_VT(&v) = VT_ARRAY | VT_VARIANT;
      V_ARRAY(&v) = sa;
      auto b = Enc(v, &ok);
      CHECK(ok);
      CHECK(b.size() >= 4096 * 4 && b.size() <= 4096 * 4 + 12);
      CHECK(Dec(b, o));
      SAFEARRAY* ra = V_ARRAY(&o);
      LONG lb = 0;
      SafeArrayGetLBound(ra, 1, &lb);
      CHECK(lb == 1);
      VARIANT* rp;
      SafeArrayAccessData(ra, (void**)&rp);
      bool all = true;
      for (int i = 0; i < 4096; ++i)
         all = all && V_VT(&rp[i]) == VT_I4 && V_I4(&rp[i]) == (i == 7 ? 0xFFFFFF : ((i * 4095) & 0xFFFFFF));
      CHECK(all);
      SafeArrayUnaccessData(ra);
      VariantClear(&o);
      VariantClear(&v);
   }

   // byref elements, VT_EMPTY as 0 -> U8
   {
      SAFEARRAY* sa = MakeVariantArray(0, 3);
      VARIANT* p;
      SafeArrayAccessData(sa, (void**)&p);
      VariantInit(&p[0]);
      VariantInit(&p[1]);
      V_VT(&p[1]) = VT_I2 | VT_BYREF;
      short s = 200;
      V_I2REF(&p[1]) = &s;
      VariantInit(&p[2]);
      V_VT(&p[2]) = VT_UI1;
      V_UI1(&p[2]) = 3;
      SafeArrayUnaccessData(sa);
      VariantInit(&v);
      V_VT(&v) = VT_ARRAY | VT_VARIANT;
      V_ARRAY(&v) = sa;
      auto b = Enc(v, &ok);
      CHECK(ok && b.size() == 1 + 4 + 1 + 1 + 3);
      CHECK(Dec(b, o));
      VARIANT* rp;
      SafeArrayAccessData(V_ARRAY(&o), (void**)&rp);
      CHECK(V_UI4(&rp[0]) == 0 && V_UI4(&rp[1]) == 200 && V_UI4(&rp[2]) == 3);
      SafeArrayUnaccessData(V_ARRAY(&o));
      VariantClear(&o);
      // the byref element must not be freed by clearing the array
      VARIANT* wp;
      SafeArrayAccessData(sa, (void**)&wp);
      VariantInit(&wp[1]);
      SafeArrayUnaccessData(sa);
      VariantClear(&v);
   }

   // reals mixed with integers -> R8
   {
      SAFEARRAY* sa = MakeVariantArray(0, 3);
      VARIANT* p;
      SafeArrayAccessData(sa, (void**)&p);
      VariantInit(&p[0]); V_VT(&p[0]) = VT_I4; V_I4(&p[0]) = 3;
      VariantInit(&p[1]); V_VT(&p[1]) = VT_R8; V_R8(&p[1]) = 1.5;
      VariantInit(&p[2]); V_VT(&p[2]) = VT_R4; V_R4(&p[2]) = 0.25f;
      SafeArrayUnaccessData(sa);
      VariantInit(&v);
      V_VT(&v) = VT_ARRAY | VT_VARIANT;
      V_ARRAY(&v) = sa;
      auto b = Enc(v, &ok);
      CHECK(ok && b.size() == 1 + 4 + 1 + 1 + 24);
      CHECK(Dec(b, o));
      VARIANT* rp;
      SafeArrayAccessData(V_ARRAY(&o), (void**)&rp);
      CHECK(V_VT(&rp[0]) == VT_R8 && V_R8(&rp[0]) == 3.0 && V_R8(&rp[1]) == 1.5 && V_R8(&rp[2]) == 0.25);
      SafeArrayUnaccessData(V_ARRAY(&o));
      VariantClear(&o);
      VariantClear(&v);
   }

   // mixed: number, string, bool, plus a nested array
   {
      SAFEARRAY* sa = MakeVariantArray(2, 4);
      VARIANT* p;
      SafeArrayAccessData(sa, (void**)&p);
      VariantInit(&p[0]); V_VT(&p[0]) = VT_I4; V_I4(&p[0]) = 42;
      VariantInit(&p[1]); V_VT(&p[1]) = VT_BSTR; V_BSTR(&p[1]) = SysAllocString(L"text");
      VariantInit(&p[2]); V_VT(&p[2]) = VT_BOOL; V_BOOL(&p[2]) = VARIANT_TRUE;
      VariantInit(&p[3]);
      V_VT(&p[3]) = VT_ARRAY | VT_VARIANT;
      SAFEARRAY* inner = MakeVariantArray(0, 2);
      VARIANT* ip;
      SafeArrayAccessData(inner, (void**)&ip);
      VariantInit(&ip[0]); V_VT(&ip[0]) = VT_I4; V_I4(&ip[0]) = 1;
      VariantInit(&ip[1]); V_VT(&ip[1]) = VT_I4; V_I4(&ip[1]) = 2;
      SafeArrayUnaccessData(inner);
      V_ARRAY(&p[3]) = inner;
      SafeArrayUnaccessData(sa);
      VariantInit(&v);
      V_VT(&v) = VT_ARRAY | VT_VARIANT;
      V_ARRAY(&v) = sa;
      auto b = Enc(v, &ok);
      CHECK(ok);
      CHECK(Dec(b, o));
      LONG lb = 0;
      SafeArrayGetLBound(V_ARRAY(&o), 1, &lb);
      CHECK(lb == 2);
      VARIANT* rp;
      SafeArrayAccessData(V_ARRAY(&o), (void**)&rp);
      CHECK(V_VT(&rp[0]) == VT_I4 && V_I4(&rp[0]) == 42);
      CHECK(V_VT(&rp[1]) == VT_BSTR && wcscmp(V_BSTR(&rp[1]), L"text") == 0);
      CHECK(V_VT(&rp[2]) == VT_BOOL && V_BOOL(&rp[2]) == VARIANT_TRUE);
      CHECK(V_VT(&rp[3]) == (VT_ARRAY | VT_VARIANT));
      SafeArrayUnaccessData(V_ARRAY(&o));
      VariantClear(&o);

      // a failing element fails the whole array and leaves the writer untouched
      VARIANT* wp;
      SafeArrayAccessData(sa, (void**)&wp);
      VariantClear(&wp[2]);
      V_VT(&wp[2]) = VT_UNKNOWN;
      V_UNKNOWN(&wp[2]) = nullptr;
      SafeArrayUnaccessData(sa);
      ByteWriter w;
      w.u32(0x11223344);
      CHECK(!EncodeVariant(w, v));
      CHECK(w.size() == 4);
      VariantClear(&v);
   }

   // typed arrays
   {
      SAFEARRAY* sa = SafeArrayCreateVector(VT_UI1, 0, 300);
      uint8_t* d;
      SafeArrayAccessData(sa, (void**)&d);
      for (int i = 0; i < 300; ++i)
         d[i] = (uint8_t)i;
      SafeArrayUnaccessData(sa);
      VariantInit(&v);
      V_VT(&v) = VT_ARRAY | VT_UI1;
      V_ARRAY(&v) = sa;
      auto b = Enc(v, &ok);
      CHECK(ok && b.size() == 1 + 4 + 2 + 1 + 300);
      CHECK(Dec(b, o));
      VARIANT* rp;
      SafeArrayAccessData(V_ARRAY(&o), (void**)&rp);
      CHECK(V_UI4(&rp[299]) == (uint8_t)299);
      SafeArrayUnaccessData(V_ARRAY(&o));
      VariantClear(&o);

      VARIANT br;
      VariantInit(&br);
      V_VT(&br) = VT_ARRAY | VT_UI1 | VT_BYREF;
      V_ARRAYREF(&br) = &sa;
      CHECK(Enc(br) == b);
      VariantClear(&v);
   }
   {
      SAFEARRAY* sa = SafeArrayCreateVector(VT_R8, 0, 3);
      double* d;
      SafeArrayAccessData(sa, (void**)&d);
      d[0] = 1; d[1] = 2.5; d[2] = -3;
      SafeArrayUnaccessData(sa);
      VariantInit(&v);
      V_VT(&v) = VT_ARRAY | VT_R8;
      V_ARRAY(&v) = sa;
      auto b = Enc(v, &ok);
      CHECK(ok && Dec(b, o));
      VARIANT* rp;
      SafeArrayAccessData(V_ARRAY(&o), (void**)&rp);
      CHECK(V_R8(&rp[1]) == 2.5 && V_R8(&rp[2]) == -3.0);
      SafeArrayUnaccessData(V_ARRAY(&o));
      VariantClear(&o);
      VariantClear(&v);
   }
   {
      SAFEARRAY* sa = SafeArrayCreateVector(VT_I4, 5, 3);
      int32_t* d;
      SafeArrayAccessData(sa, (void**)&d);
      d[0] = -1; d[1] = 100000; d[2] = INT32_MAX;
      SafeArrayUnaccessData(sa);
      VariantInit(&v);
      V_VT(&v) = VT_ARRAY | VT_I4;
      V_ARRAY(&v) = sa;
      auto b = Enc(v, &ok);
      CHECK(ok && b.size() == 1 + 4 + 1 + 1 + 12);
      CHECK(Dec(b, o));
      VARIANT* rp;
      SafeArrayAccessData(V_ARRAY(&o), (void**)&rp);
      CHECK(V_I4(&rp[0]) == -1 && V_I4(&rp[2]) == INT32_MAX);
      SafeArrayUnaccessData(V_ARRAY(&o));
      VariantClear(&o);
      VariantClear(&v);
   }
   // empty array
   {
      SAFEARRAY* sa = MakeVariantArray(0, 0);
      VariantInit(&v);
      V_VT(&v) = VT_ARRAY | VT_VARIANT;
      V_ARRAY(&v) = sa;
      auto b = Enc(v, &ok);
      CHECK(ok && Dec(b, o) && V_VT(&o) == (VT_ARRAY | VT_VARIANT));
      VariantClear(&o);
      VariantClear(&v);
   }

   // two-dimensional array: rejected, writer unchanged
   {
      SAFEARRAYBOUND bounds[2] = { { 2, 0 }, { 2, 0 } };
      SAFEARRAY* sa = SafeArrayCreate(VT_I4, 2, bounds);
      VariantInit(&v);
      V_VT(&v) = VT_ARRAY | VT_I4;
      V_ARRAY(&v) = sa;
      ByteWriter w;
      w.u16(0xABCD);
      CHECK(!EncodeVariant(w, v));
      CHECK(w.size() == 2);
      VariantClear(&v);
      SAFEARRAY* sb = SafeArrayCreate(VT_VARIANT, 2, bounds);
      V_VT(&v) = VT_ARRAY | VT_VARIANT;
      V_ARRAY(&v) = sb;
      CHECK(!EncodeVariant(w, v));
      CHECK(w.size() == 2);
      VariantClear(&v);
   }
   // array of objects
   {
      SAFEARRAY* sa = SafeArrayCreateVector(VT_UNKNOWN, 0, 2);
      VariantInit(&v);
      V_VT(&v) = VT_ARRAY | VT_UNKNOWN;
      V_ARRAY(&v) = sa;
      ByteWriter w;
      CHECK(!EncodeVariant(w, v) && w.size() == 0);
      VariantClear(&v);
   }
   // null array pointer
   {
      VariantInit(&v);
      V_VT(&v) = VT_ARRAY | VT_I4;
      V_ARRAY(&v) = nullptr;
      ByteWriter w;
      CHECK(!EncodeVariant(w, v) && w.size() == 0);
   }
}

static void TestMalformed()
{
   SAFEARRAY* sa = MakeVariantArray(0, 4);
   VARIANT* p;
   SafeArrayAccessData(sa, (void**)&p);
   VariantInit(&p[0]); V_VT(&p[0]) = VT_I4; V_I4(&p[0]) = 123456;
   VariantInit(&p[1]); V_VT(&p[1]) = VT_BSTR; V_BSTR(&p[1]) = SysAllocString(L"hello");
   VariantInit(&p[2]); V_VT(&p[2]) = VT_R8; V_R8(&p[2]) = 1.5;
   VariantInit(&p[3]); V_VT(&p[3]) = VT_BOOL; V_BOOL(&p[3]) = VARIANT_TRUE;
   SafeArrayUnaccessData(sa);
   VARIANT v;
   VariantInit(&v);
   V_VT(&v) = VT_ARRAY | VT_VARIANT;
   V_ARRAY(&v) = sa;

   std::vector<std::vector<uint8_t>> samples;
   samples.push_back(Enc(v));
   VariantClear(&v);

   VARIANT s;
   VariantInit(&s);
   V_VT(&s) = VT_BSTR;
   V_BSTR(&s) = SysAllocString(L"abcdef");
   samples.push_back(Enc(s));
   VariantClear(&s);
   V_VT(&s) = VT_R8;
   V_R8(&s) = 1.0;
   samples.push_back(Enc(s));
   V_VT(&s) = VT_I8;
   V_I8(&s) = 1ll << 40;
   samples.push_back(Enc(s));

   SAFEARRAY* ua = SafeArrayCreateVector(VT_UI1, 0, 100);
   VariantInit(&v);
   V_VT(&v) = VT_ARRAY | VT_UI1;
   V_ARRAY(&v) = ua;
   samples.push_back(Enc(v));
   VariantClear(&v);

   for (const auto& b : samples)
   {
      CHECK(!b.empty());
      bool allFailed = true;
      for (size_t n = 0; n < b.size(); ++n)
      {
         VARIANT o;
         VariantInit(&o);
         ByteReader r(b.data(), n);
         if (DecodeVariant(r, o))
            allFailed = false;
         if (V_VT(&o) != VT_EMPTY)
            allFailed = false;
         VariantClear(&o);
      }
      CHECK(allFailed);
      VARIANT o;
      CHECK(Dec(b, o));
      VariantClear(&o);
   }

   // absurd counts
   {
      ByteWriter w;
      w.u8(12);
      w.i32(0);
      w.varu(0xFFFFFFFFu);
      w.u8(3);
      VARIANT o;
      VariantInit(&o);
      ByteReader r(w.data(), w.size());
      CHECK(!DecodeVariant(r, o));
      CHECK(V_VT(&o) == VT_EMPTY);
      for (const uint8_t fmt : { 0, 1, 2, 3 })
      {
         ByteWriter w2;
         w2.u8(12);
         w2.i32(0);
         w2.varu(50u * 1000u * 1000u);
         w2.u8(fmt);
         w2.u32(0);
         ByteReader r2(w2.data(), w2.size());
         VariantInit(&o);
         CHECK(!DecodeVariant(r2, o));
      }
      ByteWriter w3;
      w3.u8(11);
      w3.varu(0xFFFFFFFFu);
      ByteReader r3(w3.data(), w3.size());
      VariantInit(&o);
      CHECK(!DecodeVariant(r3, o));
      const uint8_t badTag[] = { 200 };
      ByteReader r4(badTag, 1);
      CHECK(!DecodeVariant(r4, o));
      ByteWriter w5;
      w5.u8(12); w5.i32(0); w5.varu(0); w5.u8(9);
      ByteReader r5(w5.data(), w5.size());
      CHECK(!DecodeVariant(r5, o));
      // deeply nested arrays must fail without overflowing the stack
      ByteWriter w6;
      for (int i = 0; i < 100000; ++i)
      {
         w6.u8(12); w6.i32(0); w6.varu(1); w6.u8(3);
      }
      ByteReader r6(w6.data(), w6.size());
      CHECK(!DecodeVariant(r6, o));
   }
}

// ---------------------------------------------------------------------------------------------------------

struct Pair
{
   std::unique_ptr<TcpListener> listener;
   std::unique_ptr<TcpPeer> client;
   std::unique_ptr<TcpPeer> server;
};

static double Seconds()
{
   return (double)NowUsec() / 1e6;
}

// The first of ten loopback ports the test may listen on; `wire_test <port>` moves them
static uint16_t g_portBase = 47911;

static bool MakePair(Pair& p, uint16_t& portOut)
{
   std::string err;
   for (uint16_t port = g_portBase; port < g_portBase + 10; ++port)
   {
      p.listener = ListenTcp(port, err, "127.0.0.1"); // loopback only: no Windows Firewall prompt
      if (p.listener)
      {
         portOut = port;
         break;
      }
   }
   if (!p.listener)
      return false;
   p.client = ConnectTcp("127.0.0.1", portOut, err);
   if (!p.client)
      return false;
   std::vector<Message> sink;
   const double t0 = Seconds();
   while (Seconds() - t0 < 5.0 && (!p.server || !p.client->Connected()))
   {
      p.client->Poll(sink);
      if (!p.server)
         p.server = p.listener->Accept();
      if (p.server)
         p.server->Poll(sink);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
   return p.server && p.client->Connected() && sink.empty();
}

static void TestTransport()
{
   CHECK(WireStartup());
   CHECK(WireStartup());

   {
      std::string err;
      auto bad = ConnectTcp("no-such-host.invalid", 1234, err);
      CHECK(!bad && !err.empty());
   }

   Pair p;
   uint16_t port = 0;
   const bool made = MakePair(p, port);
   CHECK(made);
   if (!made)
      return;
   CHECK(p.listener->Port() == port);
   CHECK(p.client->Alive() && p.client->Connected() && p.server->Connected());
   CHECK(!p.client->RemoteAddress().empty() && !p.server->RemoteAddress().empty());
   printf("transport on port %u, client sees %s, server sees %s\n", port, p.client->RemoteAddress().c_str(),
      p.server->RemoteAddress().c_str());

   // second listener on the same port fails cleanly
   {
      std::string err;
      auto second = ListenTcp(port, err, "127.0.0.1");
      CHECK(!second);
      CHECK(!err.empty());
      printf("second listen: %s\n", err.c_str());
   }

   // an address that is not an IPv4 literal is refused before any socket exists
   {
      std::string err;
      auto bad = ListenTcp(47930, err, "not-an-address");
      CHECK(!bad);
      CHECK(!err.empty());
   }

   // 10,000 small messages each way
   {
      const int N = 10000;
      std::vector<Message> atServer, atClient;
      auto mk = [](int i, int dir, std::vector<uint8_t>& pl)
      {
         pl.assign(4 + (size_t)(i % 50), (uint8_t)dir);
         memcpy(pl.data(), &i, 4);
      };
      std::vector<uint8_t> pl;
      for (int i = 0; i < N; ++i)
      {
         mk(i, 1, pl);
         CHECK(p.client->Send((uint8_t)(1 + i % 4), pl.data(), pl.size()));
         mk(i, 2, pl);
         CHECK(p.server->Send((uint8_t)(1 + (i + 1) % 4), pl.data(), pl.size()));
         if (i % 200 == 0)
         {
            p.client->Poll(atClient);
            p.server->Poll(atServer);
         }
      }
      const double t0 = Seconds();
      while (Seconds() - t0 < 10.0 && (atServer.size() < (size_t)N || atClient.size() < (size_t)N))
      {
         p.client->Poll(atClient);
         p.server->Poll(atServer);
      }
      CHECK(atServer.size() == (size_t)N);
      CHECK(atClient.size() == (size_t)N);
      bool good = atServer.size() == (size_t)N && atClient.size() == (size_t)N;
      for (int i = 0; good && i < N; ++i)
      {
         int a, b;
         memcpy(&a, atServer[i].payload.data(), 4);
         memcpy(&b, atClient[i].payload.data(), 4);
         good = a == i && b == i && atServer[i].type == 1 + i % 4 && atClient[i].type == 1 + (i + 1) % 4
            && atServer[i].payload.size() == 4 + (size_t)(i % 50) && atClient[i].payload.size() == 4 + (size_t)(i % 50)
            && (i % 50 == 0 || atServer[i].payload[4] == 1) && (i % 50 == 0 || atClient[i].payload[4] == 2);
      }
      CHECK(good);
   }

   // empty payload
   {
      std::vector<Message> got;
      CHECK(p.client->Send(MSG_BYE, nullptr, 0));
      const double t0 = Seconds();
      while (Seconds() - t0 < 2.0 && got.empty())
         p.server->Poll(got);
      CHECK(got.size() == 1 && got[0].type == MSG_BYE && got[0].payload.empty());
   }

   // big payloads
   {
      const size_t SZ = 5u * 1024u * 1024u;
      std::vector<uint8_t> rnd(SZ);
      for (auto& b : rnd)
         b = Rnd();
      std::vector<uint8_t> comp(SZ);
      for (size_t i = 0; i < SZ; ++i)
         comp[i] = (uint8_t)((i / 64) % 7);

      for (int pass = 0; pass < 2; ++pass)
      {
         const auto& src = pass == 0 ? rnd : comp;
         const uint64_t before = p.client->BytesSent();
         std::vector<Message> got;
         CHECK(p.client->Send(MSG_KEYFRAME, src.data(), src.size()));
         const double t0 = Seconds();
         while (Seconds() - t0 < 20.0 && got.empty())
         {
            p.client->Poll(got);
            p.server->Poll(got);
         }
         CHECK(got.size() == 1);
         if (got.size() == 1)
         {
            CHECK(got[0].type == MSG_KEYFRAME);
            CHECK(got[0].payload == src);
         }
         std::vector<Message> sink;
         for (int k = 0; k < 100; ++k)
            p.client->Poll(sink);
         const uint64_t used = p.client->BytesSent() - before;
         printf("%s 5 MB payload: %llu bytes on the wire\n", pass == 0 ? "incompressible" : "compressible", (unsigned long long)used);
         if (pass == 0)
            CHECK(used >= SZ && used < SZ + 64);
         else
            CHECK(used < SZ / 20);
         CHECK(p.server->BytesReceived() > 0);
      }
   }

   // closing one end
   {
      p.server->Close();
      CHECK(!p.server->Alive());
      CHECK(!p.server->Send(1, nullptr, 0));
      std::vector<Message> sink;
      const double t0 = Seconds();
      bool alive = true;
      while (Seconds() - t0 < 1.0 && alive)
         alive = p.client->Poll(sink);
      CHECK(!alive);
      CHECK(!p.client->Alive());
      CHECK(!p.client->Send(1, nullptr, 0));
   }

   p.client.reset();
   p.server.reset();
   p.listener.reset();

   // close right after sending: the data sent before the close is still delivered
   {
      Pair q;
      uint16_t qp;
      if (MakePair(q, qp))
      {
         std::vector<uint8_t> data(1000, 7);
         q.client->Send(MSG_HELLO, data.data(), data.size());
         q.client->Send(MSG_FRAME, data.data(), 10);
         q.client->Close();
         std::vector<Message> got;
         const double t0 = Seconds();
         bool alive = true;
         while (Seconds() - t0 < 2.0 && alive)
            alive = q.server->Poll(got);
         CHECK(!alive);
         CHECK(got.size() == 2 && got[0].payload.size() == 1000 && got[1].type == MSG_FRAME);
      }
      else
         CHECK(false);
   }

   // connect to a port nobody listens on
   {
      std::string err;
      auto c = ConnectTcp("127.0.0.1", 47999, err);
      CHECK(c && c->Alive() && !c->Connected());
      if (c)
      {
         std::vector<Message> sink;
         const double t0 = Seconds();
         bool alive = true;
         while (Seconds() - t0 < 6.0 && alive)
         {
            alive = c->Poll(sink);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
         }
         const double el = Seconds() - t0;
         printf("refused connect took %.2f s\n", el);
         CHECK(!alive && el < 5.5);
         CHECK(!c->Alive());
      }
   }

   // a peer that never polls: backlog limit
   {
      Pair q;
      uint16_t qp;
      if (MakePair(q, qp))
      {
         std::vector<uint8_t> chunk(64 * 1024);
         int sent = 0;
         bool failed = false;
         for (int i = 0; i < 4000; ++i)
         {
            for (auto& b : chunk)
               b = Rnd();
            if (!q.client->Send(MSG_FRAME, chunk.data(), chunk.size()))
            {
               failed = true;
               break;
            }
            ++sent;
         }
         printf("backlog limit hit after %d x 64 KB (failed=%d), wire bytes %llu\n", sent, (int)failed,
            (unsigned long long)q.client->BytesSent());
         CHECK(failed);
         CHECK(!q.client->Alive());
         CHECK((uint64_t)sent * 64 * 1024 < 80ull * 1024 * 1024);
      }
      else
         CHECK(false);
   }

   // peers that send garbage: unknown flag, absurd length, bad deflate stream
   {
      Pair q;
      uint16_t qp;
      if (MakePair(q, qp))
      {
         sockaddr_in a = {};
         a.sin_family = AF_INET;
         a.sin_port = htons(qp);
         inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);

         const std::vector<std::vector<uint8_t>> attacks = {
            { 2, 0, 0, 0, 1, 0x80 },
            { 0x01, 0x00, 0x00, 0x04, 1, 0 }, // 64 MiB + 1
            { 10, 0, 0, 0, 1, 1, 100, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF },
         };
         for (const auto& attack : attacks)
         {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            CHECK(connect(s, (sockaddr*)&a, sizeof(a)) == 0);
            std::unique_ptr<TcpPeer> acc;
            const double t0 = Seconds();
            while (!acc && Seconds() - t0 < 2.0)
               acc = q.listener->Accept();
            CHECK(acc != nullptr);
            send(s, (const char*)attack.data(), (int)attack.size(), 0);
            std::vector<Message> sink;
            bool alive = true;
            const double t1 = Seconds();
            while (acc && alive && Seconds() - t1 < 2.0)
               alive = acc->Poll(sink);
            CHECK(!alive && sink.empty());
            closesocket(s);
         }
      }
      else
         CHECK(false);
   }

   WireShutdown();
   WireShutdown();
}

static void TestTiming()
{
   SAFEARRAY* sa = MakeVariantArray(0, 4096);
   VARIANT* p;
   SafeArrayAccessData(sa, (void**)&p);
   for (int i = 0; i < 4096; ++i)
   {
      VariantInit(&p[i]);
      V_VT(&p[i]) = VT_UI1;
      V_UI1(&p[i]) = (BYTE)(i % 101);
   }
   SafeArrayUnaccessData(sa);
   VARIANT v;
   VariantInit(&v);
   V_VT(&v) = VT_ARRAY | VT_VARIANT;
   V_ARRAY(&v) = sa;

   const uint64_t t0 = NowUsec();
   for (int i = 0; i < 1000; ++i)
   {
      ByteWriter w;
      EncodeVariant(w, v);
      ByteReader r(w.data(), w.size());
      VARIANT o;
      VariantInit(&o);
      DecodeVariant(r, o);
      VariantClear(&o);
   }
   const uint64_t t1 = NowUsec();
   VariantClear(&v);

   Pair q;
   uint16_t qp;
   uint64_t deflUs = 0;
   if (WireStartup() && MakePair(q, qp))
   {
      std::vector<uint8_t> pl(16384);
      for (size_t i = 0; i < pl.size(); ++i)
         pl[i] = (uint8_t)((i * i / 31) & 0x3F);
      std::vector<Message> sink;
      uint64_t spent = 0;
      for (int i = 0; i < 1000; ++i)
      {
         const uint64_t a = NowUsec();
         q.client->Send(MSG_FRAME, pl.data(), pl.size());
         spent += NowUsec() - a;
         q.client->Poll(sink);
         q.server->Poll(sink);
         sink.clear();
      }
      deflUs = spent;
      printf("16 KB payload: %llu wire bytes per message on average\n", (unsigned long long)(q.client->BytesSent() / 1000));
   }
   q = Pair();
   WireShutdown();
   printf("TIMING: U8 4096 encode+decode x1000 = %llu us total (%.1f us each); frame+deflate 16 KB x1000 (Send incl. socket write) = %llu us total (%.1f us each)\n",
      (unsigned long long)(t1 - t0), (double)(t1 - t0) / 1000.0, (unsigned long long)deflUs, (double)deflUs / 1000.0);
}

// ---------------------------------------------------------------------------------------------------------
// FrameMessage and the recording file

static std::vector<uint8_t> ReadFileBytes(const std::wstring& path)
{
   std::vector<uint8_t> b;
   FILE* f = nullptr;
   if (!(f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO))) // the recorder holds it open for writing
      return b;
   uint8_t buf[65536];
   size_t n;
   while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      b.insert(b.end(), buf, buf + n);
   fclose(f);
   return b;
}

static std::wstring TempPath(const wchar_t* const name)
{
   wchar_t dir[MAX_PATH];
   GetTempPathW(MAX_PATH, dir);
   return std::wstring(dir) + name;
}

static void TestFraming()
{
   // What FrameMessage produces is byte for byte what a TcpPeer puts on the wire
   CHECK(WireStartup());
   std::string err;
   std::unique_ptr<TcpListener> listener;
   uint16_t port = 0;
   for (uint16_t p = g_portBase; p < g_portBase + 10 && !listener; ++p)
      if ((listener = ListenTcp(p, err, "127.0.0.1")))
         port = p;
   CHECK(listener != nullptr);
   if (!listener)
      return;
   sockaddr_in a = {};
   a.sin_family = AF_INET;
   a.sin_port = htons(port);
   inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
   SOCKET raw = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   CHECK(connect(raw, (sockaddr*)&a, sizeof(a)) == 0);
   std::unique_ptr<TcpPeer> peer;
   const double t0 = Seconds();
   while (!peer && Seconds() - t0 < 2.0)
      peer = listener->Accept();
   CHECK(peer != nullptr);
   if (peer)
   {
      std::vector<uint8_t> compressible(5000), random(5000), shortOne(100, 9);
      for (size_t i = 0; i < compressible.size(); ++i)
         compressible[i] = (uint8_t)((i / 10) % 5);
      for (auto& b : random)
         b = Rnd();
      const struct { uint8_t type; const std::vector<uint8_t>* p; } cases[] = { { MSG_BYE, nullptr }, { MSG_HELLO, &shortOne }, { MSG_KEYFRAME, &compressible },
         { MSG_FRAME, &random } };
      std::vector<uint8_t> expect;
      for (const auto& c : cases)
      {
         const size_t at = expect.size();
         CHECK(FrameMessage(expect, c.type, c.p ? c.p->data() : nullptr, c.p ? c.p->size() : 0));
         CHECK(peer->Send(c.type, c.p ? c.p->data() : nullptr, c.p ? c.p->size() : 0));
         if (c.p == &compressible)
            CHECK(expect[at + 5] == 1 && expect.size() - at < c.p->size() / 4); // deflated
         if (c.p == &random || c.p == &shortOne)
            CHECK(expect[at + 5] == 0);
      }
      std::vector<uint8_t> got;
      const double t1 = Seconds();
      while (got.size() < expect.size() && Seconds() - t1 < 3.0)
      {
         std::vector<Message> sink;
         peer->Poll(sink);
         char buf[8192];
         u_long avail = 0;
         ioctlsocket(raw, FIONREAD, &avail);
         if (avail)
         {
            const int n = recv(raw, buf, sizeof(buf), 0);
            if (n > 0)
               got.insert(got.end(), buf, buf + n);
         }
      }
      CHECK(got == expect);
      CHECK(peer->BytesSent() == expect.size());
      printf("framing: %zu bytes from FrameMessage, %zu from the socket, identical %d\n", expect.size(), got.size(), (int)(got == expect));
   }
   closesocket(raw);
   peer.reset();
   listener.reset();
   WireShutdown();

   // A payload that cannot be framed leaves the output untouched
   std::vector<uint8_t> out(3, 1);
   static const uint8_t one = 0;
   CHECK(!FrameMessage(out, MSG_FRAME, &one, 64u * 1024u * 1024u));
   CHECK(out.size() == 3);
}

static void TestRecorder()
{
   const std::wstring path = TempPath(L"castcade-wire-test.castrec");
   DeleteFileW(path.c_str());
   RecordingInfo info;
   info.protocol = 1;
   info.partCount = 553;
   info.hash = 0x0123456789ABCDEFull;
   info.table = "Attack from Mars LE (Midway 1995) v600.vpx";
   info.build = "10.8.0.0 test";

   // The messages and when they were written
   std::vector<uint8_t> big(20000);
   for (size_t i = 0; i < big.size(); ++i)
      big[i] = (uint8_t)((i * 7 / 13) & 0x1F);
   std::vector<std::pair<uint8_t, std::vector<uint8_t>>> sent;
   sent.emplace_back(MSG_HELLO, std::vector<uint8_t>(40, 3));
   sent.emplace_back(MSG_KEYFRAME, big);
   for (int i = 0; i < 30; ++i)
   {
      std::vector<uint8_t> f(200 + i * 37);
      for (auto& b : f)
         b = (uint8_t)(Rnd() & 0x0F);
      sent.emplace_back(MSG_FRAME, f);
   }
   sent.emplace_back(MSG_BYE, std::vector<uint8_t>());

   FILETIME before;
   GetSystemTimeAsFileTime(&before);
   std::string err;
   std::unique_ptr<StreamRecorder> rec = CreateRecorder(path, info, err);
   CHECK(rec != nullptr);
   if (!rec)
      return;
   const uint64_t w0 = NowUsec();
   uint64_t wroteAt[64] = {};
   for (size_t i = 0; i < sent.size(); ++i)
   {
      wroteAt[i] = NowUsec() - w0;
      CHECK(rec->Write(sent[i].first, sent[i].second.data(), sent[i].second.size()));
      if (i == 10)
      {
         // past the writer thread's one second flush: the file holds the header and the first chunks by now
         std::this_thread::sleep_for(std::chrono::milliseconds(1300));
         const std::vector<uint8_t> partial = ReadFileBytes(path);
         CHECK(partial.size() > RECORDING_HEADER_FIXED && memcmp(partial.data(), "CCSTREC\x1A", 8) == 0);
         printf("recorder: %zu bytes on disk after the first flush\n", partial.size());
      }
      else
         std::this_thread::sleep_for(std::chrono::milliseconds(3));
   }
   CHECK(rec->Chunks() == sent.size());
   const uint64_t queued = rec->FileBytes();
   rec->Close();
   rec->Close(); // twice is harmless
   CHECK(rec->Error().empty());
   CHECK(!rec->Write(MSG_FRAME, nullptr, 0)); // closed

   const std::vector<uint8_t> f = ReadFileBytes(path);
   CHECK(f.size() == queued);
   ByteReader r(f.data(), f.size());
   char magic[8];
   r.raw(magic, 8);
   CHECK(memcmp(magic, "CCSTREC\x1A", 8) == 0);
   CHECK(r.u16() == RECORDING_FORMAT);
   const uint16_t headerSize = r.u16();
   CHECK(r.u16() == 1);
   CHECK(r.u16() == 0);
   const uint64_t start = r.u64();
   const uint64_t beforeFt = ((uint64_t)before.dwHighDateTime << 32) | before.dwLowDateTime;
   CHECK(start >= beforeFt && start - beforeFt < 50000000ull); // within 5 s
   const uint64_t duration = r.u64();
   CHECK(r.u32() == sent.size());
   CHECK(r.u32() == 553);
   CHECK(r.u64() == 0x0123456789ABCDEFull);
   auto str16 = [&r]
   {
      const uint16_t n = r.u16();
      std::string s(n, '\0');
      r.raw(s.data(), n);
      return s;
   };
   CHECK(str16() == info.table);
   CHECK(str16() == info.build);
   CHECK(r.ok() && f.size() - r.remaining() == headerSize);

   size_t k = 0;
   uint64_t lastT = 0;
   bool chunksOk = true;
   while (r.remaining() > 0 && r.ok())
   {
      const uint64_t t = r.u64();
      const uint32_t n = r.u32();
      const uint8_t* const body = r.span(n);
      if (!r.ok() || k >= sent.size())
      {
         chunksOk = false;
         break;
      }
      std::vector<uint8_t> expect;
      FrameMessage(expect, sent[k].first, sent[k].second.data(), sent[k].second.size());
      uint32_t len;
      memcpy(&len, body, 4);
      chunksOk = chunksOk && n == expect.size() && memcmp(body, expect.data(), n) == 0 && len == n - 4 && body[4] == sent[k].first;
      chunksOk = chunksOk && (k == 0 ? t == 0 : t >= lastT) && t <= wroteAt[k] + 2000 && t + 2000 >= wroteAt[k] - wroteAt[0];
      lastT = t;
      ++k;
   }
   CHECK(chunksOk);
   CHECK(k == sent.size());
   CHECK(duration == lastT);
   printf("recorder: %zu bytes, header %u bytes, %zu chunks, duration %.3f s\n", f.size(), headerSize, k, duration / 1e6);
   DeleteFileW(path.c_str());

   // A recorder that never got a message leaves no file behind
   {
      auto empty = CreateRecorder(path, info, err);
      CHECK(empty != nullptr);
      CHECK(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES);
      empty.reset();
      CHECK(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES);
   }

   // A folder that does not exist: refused with a reason
   {
      std::string why;
      auto bad = CreateRecorder(TempPath(L"no-such-folder-castcade\\x.castrec"), info, why);
      CHECK(bad == nullptr && !why.empty());
      printf("recorder in a missing folder: %s\n", why.c_str());
   }

   // A recorder destroyed without Close completes its file (the header says duration and count)
   {
      auto r2 = CreateRecorder(path, info, err);
      CHECK(r2 && r2->Write(MSG_HELLO, nullptr, 0) && r2->Write(MSG_BYE, nullptr, 0));
      r2.reset();
      const std::vector<uint8_t> g = ReadFileBytes(path);
      uint32_t count = 0;
      if (g.size() >= 36)
         memcpy(&count, g.data() + 32, 4);
      CHECK(count == 2);
      DeleteFileW(path.c_str());
   }

   // Cost of Write on the caller's thread: typical frame payloads, a few hundred bytes, some deflated
   {
      auto r3 = CreateRecorder(path, info, err);
      std::vector<uint8_t> frame(400), frame2(1500);
      for (size_t i = 0; i < frame2.size(); ++i)
         frame2[i] = (uint8_t)((i / 3) & 7);
      const int N = 20000;
      uint64_t worst = 0;
      const uint64_t a = NowUsec();
      for (int i = 0; i < N; ++i)
      {
         const uint64_t b = NowUsec();
         r3->Write(MSG_FRAME, (i & 1) ? frame.data() : frame2.data(), (i & 1) ? frame.size() : frame2.size());
         worst = (std::max)(worst, NowUsec() - b);
      }
      const uint64_t spent = NowUsec() - a;
      r3->Close();
      CHECK(r3->Error().empty() && r3->Chunks() == (uint64_t)N);
      printf("TIMING: recorder Write of a 400 B / 1.5 KB frame x%d = %.2f us each on average, worst %llu us\n", N, (double)spent / N, (unsigned long long)worst);
      DeleteFileW(path.c_str());
   }
}

static std::vector<std::string> g_ioSeen;
static void IoSeen(const char* const what, const uint64_t start, const uint64_t end, const size_t bytes)
{
   if (end >= start && bytes > 0)
      g_ioSeen.push_back(what);
}

static void TestAsyncLineWriter()
{
   const std::wstring wpath = TempPath(L"castcade_wire_test_lines.log");
   DeleteFileW(wpath.c_str());
   char path[MAX_PATH];
   WideCharToMultiByte(CP_ACP, 0, wpath.c_str(), -1, path, MAX_PATH, nullptr, nullptr);

   // Lines arrive in order, the mirror sees each once on the writer thread, and writes are reported
   SetIoObserver(&IoSeen);
   std::vector<std::string> mirrored;
   std::mutex mm;
   const DWORD caller = GetCurrentThreadId();
   bool mirrorOnCaller = false;
   {
      std::unique_ptr<AsyncLineWriter> w = OpenAsyncLineWriter(path, "testlog", [&](const std::string& s)
      {
         std::lock_guard<std::mutex> lock(mm);
         mirrored.push_back(s);
         mirrorOnCaller |= GetCurrentThreadId() == caller;
      });
      CHECK(w != nullptr);
      uint64_t worst = 0;
      for (int i = 0; i < 2000; ++i)
      {
         const uint64_t a = NowUsec();
         w->Line("line " + std::to_string(i));
         worst = (std::max)(worst, NowUsec() - a);
      }
      w->Flush();
      CHECK(w->LinesWritten() == 2000);
      printf("TIMING: AsyncLineWriter::Line x2000, worst %llu us on the caller's thread\n", (unsigned long long)worst);
      w->Line("last");
   } // the destructor writes what is queued
   SetIoObserver(nullptr);
   const std::vector<uint8_t> bytes = ReadFileBytes(wpath);
   std::string text(bytes.begin(), bytes.end());
   std::string expect;
   for (int i = 0; i < 2000; ++i)
      expect += "line " + std::to_string(i) + "\r\n"; // text mode, as the stream log always was
   expect += "last\r\n";
   CHECK(text == expect);
   CHECK(mirrored.size() == 2001 && mirrored.front() == "line 0" && mirrored.back() == "last");
   CHECK(!mirrorOnCaller);
   CHECK(!g_ioSeen.empty() && g_ioSeen.front() == "testlog");
   DeleteFileW(wpath.c_str());

   // Appending: a second writer adds to the file
   {
      std::unique_ptr<AsyncLineWriter> a = OpenAsyncLineWriter(path);
      a->Line("one");
   }
   {
      std::unique_ptr<AsyncLineWriter> b = OpenAsyncLineWriter(path);
      b->Line("two");
   }
   const std::vector<uint8_t> two = ReadFileBytes(wpath);
   CHECK(std::string(two.begin(), two.end()) == "one\r\ntwo\r\n");
   DeleteFileW(wpath.c_str());

   // Mirror only (no file), and the refusals
   std::vector<std::string> only;
   {
      std::unique_ptr<AsyncLineWriter> m = OpenAsyncLineWriter(std::string(), nullptr, [&](const std::string& s) { only.push_back(s); });
      CHECK(m != nullptr);
      m->Line("x");
      m->Line("y");
   }
   CHECK(only.size() == 2 && only[0] == "x" && only[1] == "y");
   CHECK(OpenAsyncLineWriter(std::string()) == nullptr);
   CHECK(OpenAsyncLineWriter("Z:\\no\\such\\folder\\castcade.log") == nullptr);
}

int main(int argc, char** argv)
{
   if (argc > 1)
   {
      const int base = atoi(argv[1]);
      if (base < 1024 || base > 65000)
      {
         printf("usage: wire_test [first loopback port, default 47911]\n");
         return 2;
      }
      g_portBase = (uint16_t)base;
   }
   printf("loopback ports %u..%u\n", g_portBase, g_portBase + 9);
   TestBuffers();
   TestVariantScalars();
   TestArrays();
   TestMalformed();
   TestTransport();
   TestTiming();
   TestFraming();
   TestRecorder();
   TestAsyncLineWriter();
   printf("SUMMARY: %d checks, %d failures\n", g_checks, g_fails);
   return g_fails ? 1 : 0;
}
