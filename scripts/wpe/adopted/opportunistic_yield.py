from wpe.patch_support import replace_once


def scheduler_header_patch(text):
    text = replace_once(text, "#include <wtf/WeakPtr.h>", "#include <wtf/WeakPtr.h>\n#include <wtf/RunLoop.h>\n#include <memory>")
    text = replace_once(
        text,
        "    void runLoopObserverFired();",
        "    void runLoopObserverFired();\n    void cancelPendingTask();",
    )
    return replace_once(
        text,
        "    MonotonicTime m_currentDeadline;\n    const UniqueRef<RunLoopObserver> m_runLoopObserver;",
        """    MonotonicTime m_currentDeadline;
    const UniqueRef<RunLoopObserver> m_runLoopObserver;
#if PLATFORM(WPE) && USE(GLIB_EVENT_LOOP)
    std::unique_ptr<RunLoop::Timer> m_retryTimer;
#endif""",
    )


def scheduler_patch(text):
    text = replace_once(
        text,
        "#include <wtf/SystemTracing.h>",
        """#include <wtf/SystemTracing.h>
#include <wtf/Scope.h>
#if PLATFORM(WPE) && USE(GLIB_EVENT_LOOP)
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <wtf/glib/RunLoopSourcePriority.h>
#endif""",
    )
    text = replace_once(
        text,
        "namespace WebCore {\n",
        """namespace WebCore {

static bool edenOpportunisticYieldEnabled()
{
#if PLATFORM(WPE) && USE(GLIB_EVENT_LOOP)
    static const bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_OPPORTUNISTIC_YIELD");
        return !value || std::strcmp(value, "0");
    }();
    return enabled;
#else
    return false;
#endif
}
""",
    )
    text = replace_once(
        text,
        """            protectedThis->runLoopObserverFired();
    }, RunLoopObserver::Type::OneShot))
{
}

OpportunisticTaskScheduler::~OpportunisticTaskScheduler() = default;""",
        """            protectedThis->runLoopObserverFired();
    }, edenOpportunisticYieldEnabled() ? RunLoopObserver::Type::Repeating : RunLoopObserver::Type::OneShot))
{
}

OpportunisticTaskScheduler::~OpportunisticTaskScheduler()
{
    cancelPendingTask();
}

void OpportunisticTaskScheduler::cancelPendingTask()
{
#if PLATFORM(WPE) && USE(GLIB_EVENT_LOOP)
    if (edenOpportunisticYieldEnabled()) {
        if (m_retryTimer)
            m_retryTimer->stop();
        m_runLoopObserver->invalidate();
        m_currentDeadline = MonotonicTime { };
    }
#endif
}""",
    )
    text = replace_once(
        text,
        """void OpportunisticTaskScheduler::rescheduleIfNeeded(MonotonicTime deadline)
{
    RefPtr page = m_page.get();
    if (page->isWaitingForLoadToFinish() || !page->isVisibleAndActive())""",
        """void OpportunisticTaskScheduler::rescheduleIfNeeded(MonotonicTime deadline)
{
    cancelPendingTask();
    RefPtr page = m_page.get();
    if (!page || page->isWaitingForLoadToFinish() || !page->isVisibleAndActive())""",
    )
    text = replace_once(
        text,
        """void OpportunisticTaskScheduler::runLoopObserverFired()
{
    constexpr bool verbose = false;""",
        """void OpportunisticTaskScheduler::runLoopObserverFired()
{
    constexpr bool verbose = false;
    auto cancelOnReturn = makeScopeExit([this] {
        cancelPendingTask();
    });""",
    )
    text = replace_once(
        text,
        """        m_runLoopObserver->invalidate();
        m_runLoopObserver->schedule();
        return;
    }

    TraceScope tracingScope""",
        """#if PLATFORM(WPE) && USE(GLIB_EVENT_LOOP)
        if (edenOpportunisticYieldEnabled()) {
            if (!m_retryTimer) {
                m_retryTimer = makeUnique<RunLoop::Timer>(RunLoop::currentSingleton(), "OpportunisticTaskScheduler::retryTimer"_s, [] { });
                constexpr auto priority = std::max({
                    RunLoopSourcePriority::RunLoopDispatcher,
                    RunLoopSourcePriority::MainThreadSharedTimer,
                    RunLoopSourcePriority::JavascriptTimer,
                    RunLoopSourcePriority::AsyncIONetwork
                }) + 1;
                m_retryTimer->setPriority(priority);
            }
            if (!m_retryTimer->isActive())
                m_retryTimer->startOneShot(0_ms);
        } else
#endif
        {
            m_runLoopObserver->invalidate();
            m_runLoopObserver->schedule();
        }
        cancelOnReturn.release();
        return;
    }

    TraceScope tracingScope""",
    )
    return replace_once(
        text,
        """    auto deadline = std::exchange(m_currentDeadline, MonotonicTime { });
    page->opportunisticallyRunIdleCallbacks(deadline);""",
        """    auto deadline = std::exchange(m_currentDeadline, MonotonicTime { });
    cancelPendingTask();
    cancelOnReturn.release();
    page->opportunisticallyRunIdleCallbacks(deadline);""",
    )


PATCHES = {
    "Source/WebCore/page/OpportunisticTaskScheduler.h": scheduler_header_patch,
    "Source/WebCore/page/OpportunisticTaskScheduler.cpp": scheduler_patch,
}
