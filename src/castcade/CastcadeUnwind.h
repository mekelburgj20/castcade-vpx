// license:GPLv3+

#pragma once

// Castcade frame probe: reading memory the probe does not own (another thread's stack, a module's export table) and
// unwinding a copy of a stack, so that a fault costs one sample and never the process. Nothing here knows about VPX:
// it builds and is tested on its own (src/castcade/tests). x64 only; elsewhere the functions do nothing.

#include <cstddef>
#include <cstdint>

#include <windows.h>

namespace castcade
{

// Copies n bytes from src, memory this thread does not own. False when reading it faulted (dst is then partly
// written). Takes no lock and allocates nothing, so it may run while another thread is suspended.
bool CopyForeignMemory(void* dst, const void* src, size_t n);

// Copies a NUL-terminated string of at most max - 1 characters from src into dst (always terminated). False when
// reading it faulted.
bool CopyForeignString(char* dst, const char* src, size_t max);

// Unwinds a copy of a thread's stack. ctx is that thread's context as captured; copy holds the n bytes that were
// at ctx.Rsp. Every register that points into [ctx.Rsp, ctx.Rsp + n) is moved into the copy before each step, so the
// unwinder reads the copy. pcs receives the instruction pointer of each frame, ctx.Rip first. A fault while
// unwinding (a frame bigger than the copy; a wrong Rip whose unwind codes send the unwinder elsewhere) ends the walk
// at the frames found so far and sets *faulted. Returns the number of frames written (at least 1).
int UnwindStackCopy(const CONTEXT& ctx, uint8_t* copy, size_t n, uint64_t* pcs, int maxFrames, bool* faulted = nullptr);

}
