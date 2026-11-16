// license:GPLv3+

#include <winsock2.h>
#include <ws2tcpip.h>

#include "../CastcadeWire.h"

#include <chrono>
#include <cstdio>
#include <thread>

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

static bool MakePair(Pair& p, uint16_t& portOut)
{
   std::string err;
   for (uint16_t port = 47911; port < 47921; ++port)
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

int main()
{
   TestBuffers();
   TestVariantScalars();
   TestArrays();
   TestMalformed();
   TestTransport();
   TestTiming();
   printf("SUMMARY: %d checks, %d failures\n", g_checks, g_fails);
   return g_fails ? 1 : 0;
}
