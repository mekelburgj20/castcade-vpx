// license:GPLv3+

// Built without the precompiled header (see the CMake lists): this file must not depend on VPX.
#include "CastcadeUnwind.h"

#include <cstring>

// Structured exception handling guards every read here. The guarded functions hold no object with a destructor
// (MSVC refuses __try in a function that needs C++ unwinding), take no lock and allocate nothing.

namespace castcade
{

bool CopyForeignMemory(void* const dst, const void* const src, const size_t n)
{
   __try
   {
      memcpy(dst, src, n);
      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER)
   {
      return false;
   }
}

bool CopyForeignString(char* const dst, const char* const src, const size_t max)
{
   if (max == 0)
      return false;
   dst[0] = '\0';
   __try
   {
      size_t i = 0;
      for (; i + 1 < max && src[i] != '\0'; ++i)
         dst[i] = src[i];
      dst[i] = '\0';
      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER)
   {
      dst[0] = '\0';
      return false;
   }
}

#if defined(_M_X64)

namespace
{

void Translate(DWORD64& r, const uint64_t lo, const uint64_t hi, const uint64_t blo)
{
   if (r >= lo && r < hi)
      r = r - lo + blo;
}

void TranslateAll(CONTEXT& c, const uint64_t lo, const uint64_t hi, const uint64_t blo)
{
   Translate(c.Rsp, lo, hi, blo);
   Translate(c.Rbp, lo, hi, blo);
   Translate(c.Rbx, lo, hi, blo);
   Translate(c.Rsi, lo, hi, blo);
   Translate(c.Rdi, lo, hi, blo);
   Translate(c.R12, lo, hi, blo);
   Translate(c.R13, lo, hi, blo);
   Translate(c.R14, lo, hi, blo);
   Translate(c.R15, lo, hi, blo);
}

// The walk itself; *depth is kept up to date so that a fault keeps the frames found before it
void Walk(CONTEXT* const c, const uint64_t lo, const uint64_t hi, const uint64_t blo, const uint64_t bhi, uint64_t* const pcs, const int maxFrames,
   volatile int* const depth)
{
   TranslateAll(*c, lo, hi, blo);
   while (*depth < maxFrames)
   {
      pcs[*depth] = c->Rip;
      *depth = *depth + 1;
      if (c->Rsp < blo || c->Rsp + 8 > bhi)
         break;
      const DWORD64 prevRsp = c->Rsp;
      DWORD64 imageBase = 0;
      PRUNTIME_FUNCTION const fe = RtlLookupFunctionEntry(c->Rip, &imageBase, nullptr);
      if (!fe)
      {
         c->Rip = *(const DWORD64*)c->Rsp; // a leaf function: the return address is on top
         c->Rsp += 8;
      }
      else
      {
         PVOID handlerData = nullptr;
         DWORD64 establisher = 0;
         RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c->Rip, fe, c, &handlerData, &establisher, nullptr);
      }
      TranslateAll(*c, lo, hi, blo);
      if (c->Rip == 0 || c->Rsp <= prevRsp || c->Rsp > bhi)
         break;
   }
}

} // namespace

int UnwindStackCopy(const CONTEXT& ctx, uint8_t* const copy, const size_t n, uint64_t* const pcs, const int maxFrames, bool* const faulted)
{
   if (faulted)
      *faulted = false;
   if (maxFrames <= 0)
      return 0;
   if (n == 0 || copy == nullptr)
   {
      pcs[0] = ctx.Rip;
      return 1;
   }
   CONTEXT c = ctx;
   volatile int depth = 0;
   const uint64_t lo = ctx.Rsp, hi = ctx.Rsp + n;
   const uint64_t blo = (uint64_t)copy, bhi = blo + n;
   __try
   {
      Walk(&c, lo, hi, blo, bhi, pcs, maxFrames, &depth);
   }
   __except (EXCEPTION_EXECUTE_HANDLER)
   {
      if (faulted)
         *faulted = true;
   }
   if (depth == 0)
   {
      pcs[0] = ctx.Rip;
      depth = 1;
   }
   return depth;
}

#else

int UnwindStackCopy(const CONTEXT& ctx, uint8_t* const, const size_t, uint64_t* const pcs, const int maxFrames, bool* const faulted)
{
   if (faulted)
      *faulted = false;
   if (maxFrames <= 0)
      return 0;
#if defined(_M_IX86)
   pcs[0] = ctx.Eip;
#else
   pcs[0] = 0;
#endif
   return 1;
}

#endif

}
