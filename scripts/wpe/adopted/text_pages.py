from wpe.patch_support import replace_once


def entry_patch(text):
    text = replace_once(text, '#include "WebProcessMain.h"', '''#include "WebProcessMain.h"
#if PLATFORM(WPE) && OS(LINUX)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <link.h>
#include <sys/mman.h>

static void edenAdviseTextHugePages() {
    const char* setting = std::getenv("WEBKIT_EDEN_TEXT_HUGE_PAGES");
    if (setting && !std::strcmp(setting, "0")) {
        return;
    }
    dl_iterate_phdr([](dl_phdr_info* information, size_t, void*) -> int {
        if (!information->dlpi_name || !std::strstr(information->dlpi_name, "libWPEWebKit-2.0.so")) {
            return 0;
        }
        constexpr uintptr_t alignment = 2 * 1024 * 1024;
        for (unsigned index = 0; index < information->dlpi_phnum; ++index) {
            const auto& segment = information->dlpi_phdr[index];
            if (segment.p_type != PT_LOAD || segment.p_flags != (PF_R | PF_X)) {
                continue;
            }
            uintptr_t start = (information->dlpi_addr + segment.p_vaddr + alignment - 1) & ~(alignment - 1);
            uintptr_t end = (information->dlpi_addr + segment.p_vaddr + segment.p_memsz) & ~(alignment - 1);
            if (end > start) {
                int result = madvise(reinterpret_cast<void*>(start), end - start, MADV_HUGEPAGE);
                if (std::getenv("WEBKIT_EDEN_EXPERIMENT_TRACE")) {
                    std::fprintf(stderr, "Eden text huge pages: advised %zu bytes result=%d\\n", static_cast<size_t>(end - start), result);
                }
            }
        }
        return 1;
    }, nullptr);
}
#endif''')
    return replace_once(text, '    return WebKit::WebProcessMain(argc, argv);', '#if PLATFORM(WPE) && OS(LINUX)\n    edenAdviseTextHugePages();\n#endif\n    return WebKit::WebProcessMain(argc, argv);')


PATCHES = {
    'Source/WebKit/WebProcess/EntryPoint/unix/WebProcessMain.cpp': entry_patch,
}
