from wpe.patch_support import replace_once


IPC = "Source/WebKit/Platform/IPC/"
MESSAGE_LOG_HEADER = IPC + "MessageLog.h"
MESSAGE_LOG = IPC + "MessageLog.cpp"
CONNECTION = IPC + "Connection.cpp"
STREAM_SERVER_CONNECTION = IPC + "StreamServerConnection.cpp"
AUXILIARY_PROCESS = "Source/WebKit/Shared/AuxiliaryProcess.cpp"


SLOW_MESSAGE_REPORT = r'''std::optional<Seconds> hangLogThreshold()
{
    static const std::optional<Seconds> threshold = []() -> std::optional<Seconds> {
        auto milliseconds = parseInteger<unsigned>(StringView::fromLatin1(getenv("EDEN_WPE_HANG_LOG")));
        if (!milliseconds || !*milliseconds)
            return std::nullopt;
        return Seconds::fromMilliseconds(*milliseconds);
    }();
    return threshold;
}

void reportSlowMessage(ASCIILiteral activity, MessageName name, Seconds duration)
{
    auto threshold = hangLogThreshold();
    if (!threshold || duration < *threshold)
        return;
#if OS(LINUX)
    const char* processName = program_invocation_short_name;
#else
    const char* processName = "WebKit";
#endif
    WTFLogAlways("Eden hang recorder: %s spent %.0f ms %s %s", processName, duration.milliseconds(), activity.characters(), description(name).characters());
}

'''


HANG_RECORDER = r'''#if OS(LINUX)
static constexpr size_t maximumHangFrames = 64;
static std::atomic<pid_t> hangCaptureTarget { 0 };
static std::atomic<int> hangCaptureCount { 0 };
static std::array<void*, maximumHangFrames> hangCaptureFrames;
static sem_t hangCaptureDone;
static std::atomic<double> hangHeartbeat { 0 };

static pid_t currentThreadIdentifier()
{
    return static_cast<pid_t>(syscall(SYS_gettid));
}

static void captureHangStack(int)
{
    if (hangCaptureTarget.load() != currentThreadIdentifier())
        return;
    hangCaptureCount.store(backtrace(hangCaptureFrames.data(), static_cast<int>(hangCaptureFrames.size())));
    sem_post(&hangCaptureDone);
}

static CString hangThreadName(pid_t thread)
{
    std::array<char, 64> name { };
    auto path = makeString("/proc/self/task/"_s, thread, "/comm"_s).utf8();
    int descriptor = open(path.data(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return "unknown"_s;
    auto length = read(descriptor, name.data(), name.size() - 1);
    close(descriptor);
    if (length <= 0)
        return "unknown"_s;
    auto characters = std::span { name }.first(static_cast<size_t>(length));
    if (characters.back() == '\n')
        characters = characters.first(characters.size() - 1);
    return CString { characters };
}

static bool hangThreadBlocksSignal(pid_t thread, int signalNumber)
{
    std::array<char, 4096> status { };
    auto path = makeString("/proc/self/task/"_s, thread, "/status"_s).utf8();
    int descriptor = open(path.data(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return false;
    auto length = read(descriptor, status.data(), status.size() - 1);
    close(descriptor);
    if (length <= 0)
        return false;
    auto text = StringView::fromLatin1(status.data());
    auto start = text.find("SigBlk:\t"_s);
    if (start == notFound)
        return false;
    auto mask = text.substring(start + 8, 16);
    auto blocked = parseInteger<uint64_t>(mask, 16);
    return blocked && (*blocked >> (signalNumber - 1)) & 1;
}

static void printHangStack(pid_t thread, int signalNumber)
{
    WTFLogAlways("  thread %d (%s)", static_cast<int>(thread), hangThreadName(thread).data());
    if (hangThreadBlocksSignal(thread, signalNumber)) {
        WTFLogAlways("    blocks the capture signal");
        return;
    }
    while (!sem_trywait(&hangCaptureDone)) { }
    hangCaptureCount.store(0);
    hangCaptureTarget.store(thread);
    if (syscall(SYS_tgkill, getpid(), thread, signalNumber)) {
        hangCaptureTarget.store(0);
        WTFLogAlways("    could not signal the thread");
        return;
    }
    timespec deadline { };
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 100'000'000;
    if (deadline.tv_nsec >= 1'000'000'000) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1'000'000'000;
    }
    int waited;
    do
        waited = sem_timedwait(&hangCaptureDone, &deadline);
    while (waited && errno == EINTR);
    hangCaptureTarget.store(0);
    if (waited) {
        WTFLogAlways("    the thread did not report its stack");
        return;
    }
    auto frames = std::span { hangCaptureFrames }.first(static_cast<size_t>(std::clamp(hangCaptureCount.load(), 0, static_cast<int>(maximumHangFrames))));
    for (auto* frame : frames) {
        Dl_info info { };
        if (dladdr(frame, &info) && info.dli_fname) {
            auto library = StringView::fromLatin1(info.dli_fname);
            auto slash = library.reverseFind('/');
            auto name = (slash == notFound ? library : library.substring(slash + 1)).utf8();
            WTFLogAlways("    %s+0x%zx", name.data(), static_cast<size_t>(reinterpret_cast<uintptr_t>(frame) - reinterpret_cast<uintptr_t>(info.dli_fbase)));
        }
        else
            WTFLogAlways("    %p", frame);
    }
}

static void printHangStacks(pid_t mainThread, int signalNumber)
{
    printHangStack(mainThread, signalNumber);
    DIR* directory = opendir("/proc/self/task");
    if (!directory)
        return;
    while (auto* entry = readdir(directory)) {
        auto thread = parseInteger<pid_t>(StringView::fromLatin1(entry->d_name));
        if (!thread || *thread == mainThread || *thread == currentThreadIdentifier())
            continue;
        printHangStack(*thread, signalNumber);
    }
    closedir(directory);
}

void startEdenHangRecorderIfRequested();

void startEdenHangRecorderIfRequested()
{
    static std::atomic<bool> started { false };
    auto milliseconds = parseInteger<unsigned>(StringView::fromLatin1(getenv("EDEN_WPE_HANG_LOG")));
    if (!milliseconds || !*milliseconds)
        return;
    if (started.exchange(true))
        return;
    if (sem_init(&hangCaptureDone, 0, 0))
        return;
    int signalNumber = SIGRTMIN + 5;
    struct sigaction action { };
    action.sa_handler = captureHangStack;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(signalNumber, &action, nullptr))
        return;
    std::array<void*, 1> warmUp { };
    backtrace(warmUp.data(), static_cast<int>(warmUp.size()));

    auto threshold = Seconds::fromMilliseconds(*milliseconds);
    auto interval = std::min(threshold / 4, 50_ms);
    pid_t mainThread = currentThreadIdentifier();
    hangHeartbeat.store(MonotonicTime::now().secondsSinceEpoch().value());
    static NeverDestroyed<std::unique_ptr<RunLoop::Timer>> heartbeat;
    heartbeat.get() = makeUnique<RunLoop::Timer>(RunLoop::mainSingleton(), "EdenHangRecorderHeartbeat"_s, [] {
        hangHeartbeat.store(MonotonicTime::now().secondsSinceEpoch().value());
    });
    heartbeat.get()->startRepeating(interval);
    WTFLogAlways("Eden hang recorder: %s reports main thread stalls over %u ms", program_invocation_short_name, *milliseconds);

    Thread::create("EdenHangRecorder"_s, [mainThread, threshold, interval, signalNumber] {
        bool stalled = false;
        double stallStart = 0;
        while (true) {
            WTF::sleep(interval);
            double lastHeartbeat = hangHeartbeat.load();
            double now = MonotonicTime::now().secondsSinceEpoch().value();
            if (now - lastHeartbeat >= threshold.seconds()) {
                if (stalled)
                    continue;
                stalled = true;
                stallStart = lastHeartbeat;
                WTFLogAlways("Eden hang recorder: %s main thread has not run for %.0f ms, thread stacks follow", program_invocation_short_name, (now - lastHeartbeat) * 1000);
                printHangStacks(mainThread, signalNumber);
                continue;
            }
            if (stalled) {
                stalled = false;
                WTFLogAlways("Eden hang recorder: %s main thread resumed after %.0f ms", program_invocation_short_name, (lastHeartbeat - stallStart) * 1000);
            }
        }
    })->detach();
}
#endif

'''


def message_log_header_patch(text):
    text = replace_once(
        text,
        "#include <wtf/Noncopyable.h>\n",
        "#include <optional>\n#include <wtf/Noncopyable.h>\n#include <wtf/Seconds.h>\n#include <wtf/text/ASCIILiteral.h>\n",
    )
    return replace_once(
        text,
        "WK_EXPORT const MessageLogMetadata& NODELETE messageLogMetadata();\n",
        "WK_EXPORT const MessageLogMetadata& NODELETE messageLogMetadata();\n\n"
        "std::optional<Seconds> hangLogThreshold();\n"
        "void reportSlowMessage(ASCIILiteral activity, MessageName, Seconds duration);\n",
    )


def message_log_patch(text):
    text = replace_once(
        text,
        '#include "MessageLog.h"\n',
        '#include "MessageLog.h"\n\n#include <cerrno>\n#include <cstdlib>\n#include <wtf/ProcessID.h>\n#include <wtf/text/StringToIntegerConversion.h>\n#include <wtf/text/StringView.h>\n',
    )
    return replace_once(
        text,
        "const MessageLogMetadata& messageLogMetadata()\n{\n    return gMessageLogMetadata;\n}\n",
        "const MessageLogMetadata& messageLogMetadata()\n{\n    return gMessageLogMetadata;\n}\n\n" + SLOW_MESSAGE_REPORT.rstrip("\n") + "\n",
    )


def connection_patch(text):
    text = replace_once(
        text,
        "    auto messageName = encoder->messageName();\n\n#if ENABLE(CORE_IPC_SIGNPOSTS)\n    uintptr_t signpostIdentifier = 0;\n",
        "    auto messageName = encoder->messageName();\n    auto sendStarted = MonotonicTime::now();\n\n#if ENABLE(CORE_IPC_SIGNPOSTS)\n    uintptr_t signpostIdentifier = 0;\n",
    )
    text = replace_once(
        text,
        "        popPendingSyncRequestID(syncRequestID);\n    });\n",
        "        popPendingSyncRequestID(syncRequestID);\n        reportSlowMessage(\"waiting for the reply to\"_s, messageName, MonotonicTime::now() - sendStarted);\n    });\n",
    )
    text = replace_once(
        text,
        "    } else\n        protect(client())->didReceiveSyncMessage(*this, decoder, replyEncoder);\n",
        "    } else {\n"
        "        auto handlingStarted = MonotonicTime::now();\n"
        "        protect(client())->didReceiveSyncMessage(*this, decoder, replyEncoder);\n"
        "        reportSlowMessage(\"handling\"_s, decoder.messageName(), MonotonicTime::now() - handlingStarted);\n"
        "    }\n",
    )
    return replace_once(
        text,
        "    client->didReceiveMessage(*this, decoder);\n",
        "    auto handlingStarted = MonotonicTime::now();\n"
        "    client->didReceiveMessage(*this, decoder);\n"
        "    reportSlowMessage(\"handling\"_s, decoder.messageName(), MonotonicTime::now() - handlingStarted);\n",
    )


def stream_server_connection_patch(text):
    return replace_once(
        text,
        "    IPC::messageLog().add(message.messageName());\n    receiver.didReceiveStreamMessage(*this, message);\n",
        "    IPC::messageLog().add(message.messageName());\n"
        "    auto handlingStarted = MonotonicTime::now();\n"
        "    receiver.didReceiveStreamMessage(*this, message);\n"
        "    reportSlowMessage(\"handling\"_s, message.messageName(), MonotonicTime::now() - handlingStarted);\n",
    )


def auxiliary_process_patch(text):
    text = replace_once(
        text,
        "#include <wtf/SetForScope.h>\n",
        "#include <wtf/MonotonicTime.h>\n#include <wtf/NeverDestroyed.h>\n#include <wtf/RunLoop.h>\n#include <wtf/SetForScope.h>\n#include <wtf/Threading.h>\n#include <wtf/text/CString.h>\n#include <wtf/text/MakeString.h>\n#include <wtf/text/StringToIntegerConversion.h>\n",
    )
    text = replace_once(
        text,
        "#if OS(LINUX)\n#include <wtf/MemoryPressureHandler.h>\n",
        "#if OS(LINUX)\n#include <cerrno>\n#include <dirent.h>\n#include <dlfcn.h>\n#include <execinfo.h>\n#include <fcntl.h>\n#include <semaphore.h>\n#include <signal.h>\n#include <sys/syscall.h>\n#include <time.h>\n#include <wtf/MemoryPressureHandler.h>\n",
    )
    text = replace_once(
        text,
        "void AuxiliaryProcess::initialize(AuxiliaryProcessInitializationParameters&& parameters)\n",
        HANG_RECORDER + "void AuxiliaryProcess::initialize(AuxiliaryProcessInitializationParameters&& parameters)\n",
    )
    return replace_once(
        text,
        "    initializeProcessName(parameters);\n",
        "    initializeProcessName(parameters);\n\n#if OS(LINUX)\n    startEdenHangRecorderIfRequested();\n#endif\n",
    )


WEB_CONTEXT = "Source/WebKit/UIProcess/API/glib/WebKitWebContext.cpp"


def web_context_patch(text):
    text = replace_once(
        text,
        "using namespace WebKit;\n",
        "#if OS(LINUX)\nnamespace WebKit {\nvoid startEdenHangRecorderIfRequested();\n}\n#endif\n\nusing namespace WebKit;\n",
    )
    return replace_once(
        text,
        "static void webkitWebContextConstructed(GObject* object)\n"
        "{\n"
        "    G_OBJECT_CLASS(webkit_web_context_parent_class)->constructed(object);\n",
        "static void webkitWebContextConstructed(GObject* object)\n"
        "{\n"
        "    G_OBJECT_CLASS(webkit_web_context_parent_class)->constructed(object);\n"
        "#if OS(LINUX)\n"
        "    startEdenHangRecorderIfRequested();\n"
        "#endif\n",
    )


PATCHES = {
    MESSAGE_LOG_HEADER: message_log_header_patch,
    MESSAGE_LOG: message_log_patch,
    CONNECTION: connection_patch,
    STREAM_SERVER_CONNECTION: stream_server_connection_patch,
    AUXILIARY_PROCESS: auxiliary_process_patch,
    WEB_CONTEXT: web_context_patch,
}
