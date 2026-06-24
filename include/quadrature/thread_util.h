#pragma once

#include <pthread.h>

/*
 * Set the current thread's OS name (the kernel `comm` field), once per thread.
 *
 * The name is what sampling profilers display per row — samply / perf / the
 * Firefox Profiler all read it — and it also shows up in gdb and htop. Linux
 * truncates names to 15 characters + NUL, so keep them short (e.g. "audio-rt").
 *
 * Safe and cheap to drop at the top of a pooled-worker entry point: a
 * thread-local guard makes every call after the first a no-op, so it never
 * issues a syscall on a hot path (e.g. the audio callback). See
 * docs/architecture/PROFILING.md.
 */
static inline void
quad_set_thread_name(const char *name)
{
    static _Thread_local int named = 0;
    if (!named) {
        pthread_setname_np(pthread_self(), name);
        named = 1;
    }
}
