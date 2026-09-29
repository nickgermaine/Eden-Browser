from wpe.patch_support import replace_once


def threads_patch(text):
    text = replace_once(text, '#include <wtf/linux/RealTimeThreads.h>', '#include <wtf/linux/RealTimeThreads.h>\n#include <sys/resource.h>\n#include <cstdlib>\n#include <cstring>')
    anchor = '        Thread::initializeCurrentThreadInternal(context->name);'
    return replace_once(text, anchor, anchor + '''
#if OS(LINUX)
        static int helperNice = [] {
            const char* setting = std::getenv("WEBKIT_EDEN_HELPER_NICE");
            char* end = nullptr;
            long value = setting ? std::strtol(setting, &end, 10) : 3;
            return (!setting || (end != setting && !*end)) && value >= 0 && value <= 19 ? static_cast<int>(value) : 0;
        }();
        const char* name = context->name.characters();
        if (helperNice && (!std::strcmp(name, "JITWorker") || !std::strcmp(name, "HeapHelper") || !std::strcmp(name, "SkiaCPUWorker") || !std::strcmp(name, "CanvasDDL"))) {
            if (getpriority(PRIO_PROCESS, 0) < helperNice) {
                int result = setpriority(PRIO_PROCESS, 0, helperNice);
                if (std::getenv("WEBKIT_EDEN_EXPERIMENT_TRACE")) {
                    WTFLogAlways("Eden helper priority: %s nice=%d result=%d", name, getpriority(PRIO_PROCESS, 0), result);
                }
            }
        }
#endif''')


PATCHES = {
    'Source/WTF/wtf/Threading.cpp': threads_patch,
}
