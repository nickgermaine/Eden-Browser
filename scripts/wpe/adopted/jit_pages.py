from wpe.patch_support import replace_once


def jit_patch(text):
    text = replace_once(text, '#include "config.h"', '#include "config.h"\n#if OS(LINUX)\n#include <sys/mman.h>\n#include <cstdlib>\n#include <cstring>\n#endif')
    anchor = '        g_jscConfig.endExecutableMemory = reservationEnd;'
    return replace_once(text, anchor, anchor + '''
#if OS(LINUX)
        const char* hugePages = std::getenv("WEBKIT_EDEN_JIT_HUGE_PAGES");
        if (!hugePages || std::strcmp(hugePages, "0")) {
            constexpr uintptr_t alignment = 2 * 1024 * 1024;
            uintptr_t begin = (reinterpret_cast<uintptr_t>(reservation.base) + alignment - 1) & ~(alignment - 1);
            uintptr_t end = reinterpret_cast<uintptr_t>(reservationEnd) & ~(alignment - 1);
            if (end > begin) {
                int result = madvise(reinterpret_cast<void*>(begin), end - begin, MADV_HUGEPAGE);
                if (std::getenv("WEBKIT_EDEN_EXPERIMENT_TRACE")) {
                    dataLog("Eden JIT huge pages: advised ", end - begin, " bytes result=", result, "\\n");
                }
            }
        }
#endif''')


PATCHES = {
    'Source/JavaScriptCore/jit/ExecutableAllocator.cpp': jit_patch,
}
