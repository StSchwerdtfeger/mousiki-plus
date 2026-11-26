// Gives the memory the allocator holds on to after a big temporary (the scan of a library, the tag cache read at start-up) back to
// the system. Only glibc keeps freed memory in its arenas for good; elsewhere this does nothing.
#pragma once
#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace muisc {
inline void release_free_memory() {
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}
}
