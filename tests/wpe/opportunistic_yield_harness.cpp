#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <glib.h>

#define PLATFORM(value) 1
#define USE(value) USE_##value
#define USE_GLIB_EVENT_LOOP 1
#define USE_WEB_THREAD 0
#define dataLogLnIf(...) static_cast<void>(0)

namespace WTF {

    enum RunLoopSourcePriority : int {
        RunLoopDispatcher = 0,
        RunLoopTimer = 0,
        JavascriptTimer = 10,
        MainThreadSharedTimer = 10,
        AsyncIONetwork = 10,
    };

}

using WTF::RunLoopSourcePriority;

GMainContext *&activeContext() {
    static GMainContext *context = nullptr;
    return context;
}

unsigned &retryWakeCount() {
    static unsigned count = 0;
    return count;
}

unsigned &retryTimerConstructionCount() {
    static unsigned count = 0;
    return count;
}

int &configuredRetryPriority() {
    static int priority = -1;
    return priority;
}

std::vector<std::string> &dispatchOrder() {
    static std::vector<std::string> order;
    return order;
}

class ContextScope {
  public:
    ContextScope() {
        activeContext() = g_main_context_new();
        retryWakeCount() = 0;
        retryTimerConstructionCount() = 0;
        configuredRetryPriority() = -1;
        dispatchOrder().clear();
    }

    ~ContextScope() {
        g_main_context_unref(activeContext());
        activeContext() = nullptr;
    }
};

class LifetimeTrackedBase {
  public:
    LifetimeTrackedBase()
        : m_alive(std::make_shared<bool>(true)) {}

    LifetimeTrackedBase(const LifetimeTrackedBase &) = delete;
    LifetimeTrackedBase &operator=(const LifetimeTrackedBase &) = delete;

    virtual ~LifetimeTrackedBase() {
        *m_alive = false;
    }

    std::weak_ptr<bool> weakLifetime() const {
        return m_alive;
    }

  private:
    std::shared_ptr<bool> m_alive;
};

template <typename T> class WeakPtr {
  public:
    explicit WeakPtr(T *pointer)
        : m_pointer(pointer),
          m_lifetime(pointer ? pointer->weakLifetime() : std::weak_ptr<bool>{}) {}

    T *get() const {
        auto lifetime = m_lifetime.lock();
        return lifetime && *lifetime ? m_pointer : nullptr;
    }

    explicit operator bool() const {
        return get();
    }

  private:
    T *m_pointer{nullptr};
    std::weak_ptr<bool> m_lifetime;
};

template <typename T> WeakPtr(T *) -> WeakPtr<T>;

template <typename T> class RefPtr {
  public:
    RefPtr(T *pointer)
        : m_pointer(pointer) {}

    T *operator->() const {
        return m_pointer;
    }

    explicit operator bool() const {
        return m_pointer;
    }

  private:
    T *m_pointer{nullptr};
};

template <typename T> RefPtr(T *) -> RefPtr<T>;

template <typename T> using UniqueRef = std::unique_ptr<T>;

template <typename T, typename... Args> UniqueRef<T> makeUniqueRef(Args &&...args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}

template <typename T, typename... Args> std::unique_ptr<T> makeUnique(Args &&...args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}

namespace WebCore {

    struct Duration {
        double value{0};

        uint64_t microseconds() const {
            return static_cast<uint64_t>(value * 1000000.0);
        }
    };

    constexpr Duration operator-(Duration left, Duration right) {
        return {left.value - right.value};
    }

    constexpr Duration operator*(double multiplier, Duration duration) {
        return {multiplier * duration.value};
    }

    constexpr bool operator<(Duration left, Duration right) {
        return left.value < right.value;
    }

    constexpr bool operator>(Duration left, Duration right) {
        return left.value > right.value;
    }

    constexpr Duration operator""_ms(unsigned long long value) {
        return {static_cast<double>(value) / 1000.0};
    }

    const char *operator""_s(const char *value, size_t) {
        return value;
    }

    class MonotonicTime {
      public:
        MonotonicTime() = default;

        explicit MonotonicTime(double value)
            : m_value(value) {}

        explicit operator bool() const {
            return m_value != 0;
        }

        Duration secondsSinceEpoch() const {
            return {m_value};
        }

      private:
        double m_value{0};
    };

    double &testNow() {
        static double value = 100.0;
        return value;
    }

    class ApproximateTime {
      public:
        static MonotonicTime now() {
            return MonotonicTime{testNow()};
        }
    };

    template <typename Callback> class ScopeExit {
      public:
        explicit ScopeExit(Callback callback)
            : m_callback(std::move(callback)) {}

        ScopeExit(const ScopeExit &) = delete;
        ScopeExit &operator=(const ScopeExit &) = delete;

        ~ScopeExit() {
            if (m_active) {
                m_callback();
            }
        }

        void release() {
            m_active = false;
        }

      private:
        Callback m_callback;
        bool m_active{true};
    };

    template <typename Callback> ScopeExit<std::decay_t<Callback>> makeScopeExit(Callback &&callback) {
        return ScopeExit<std::decay_t<Callback>>(std::forward<Callback>(callback));
    }

    class RunLoop {
      public:
        static RunLoop *currentSingleton() {
            static RunLoop runLoop;
            return &runLoop;
        }

        class Timer {
          public:
            Timer(RunLoop *, const char *, std::function<void()> callback)
                : m_callback(std::move(callback)) {
                ++retryTimerConstructionCount();
            }

            Timer(const Timer &) = delete;
            Timer &operator=(const Timer &) = delete;

            ~Timer() {
                stop();
            }

            void setPriority(int priority) {
                m_priority = priority;
                configuredRetryPriority() = priority;
                if (m_source) {
                    g_source_set_priority(m_source, priority);
                }
            }

            void startOneShot(Duration) {
                stop();
                m_source = g_idle_source_new();
                g_source_set_priority(m_source, m_priority);
                g_source_set_callback(m_source, fire, this, nullptr);
                g_source_attach(m_source, activeContext());
                m_active = true;
            }

            void stop() {
                if (m_source) {
                    g_source_destroy(m_source);
                    g_source_unref(m_source);
                    m_source = nullptr;
                }
                m_active = false;
            }

            bool isActive() const {
                return m_active;
            }

          private:
            static gboolean fire(gpointer data) {
                auto *timer = static_cast<Timer *>(data);
                timer->m_active = false;
                ++retryWakeCount();
                dispatchOrder().emplace_back("retry");
                timer->m_callback();
                return G_SOURCE_REMOVE;
            }

            GSource *m_source{nullptr};
            std::function<void()> m_callback;
            int m_priority{0};
            bool m_active{false};
        };
    };

    class RunLoopObserver;

    std::vector<RunLoopObserver *> &registeredObservers() {
        static std::vector<RunLoopObserver *> observers;
        return observers;
    }

    class RunLoopObserver {
      public:
        enum class WellKnownOrder : uint8_t {
            OpportunisticTask,
        };

        enum class Type : bool {
            Repeating,
            OneShot,
        };

        RunLoopObserver(WellKnownOrder, std::function<void()> callback, Type type)
            : m_callback(std::move(callback)),
              m_repeating(type == Type::Repeating) {
            registeredObservers().push_back(this);
        }

        RunLoopObserver(const RunLoopObserver &) = delete;
        RunLoopObserver &operator=(const RunLoopObserver &) = delete;

        ~RunLoopObserver() {
            invalidate();
            auto &observers = registeredObservers();
            observers.erase(std::remove(observers.begin(), observers.end(), this), observers.end());
        }

        void schedule() {
            if (!m_scheduled) {
                m_scheduled = true;
                ++m_generation;
            }
        }

        void invalidate() {
            if (m_scheduled) {
                m_scheduled = false;
                ++m_generation;
            }
        }

        bool isScheduled() const {
            return m_scheduled;
        }

        void fire() {
            if (!m_scheduled) {
                return;
            }
            auto generation = m_generation;
            m_callback();
            if (!m_repeating && m_scheduled && m_generation == generation) {
                invalidate();
            }
        }

      private:
        std::function<void()> m_callback;
        bool m_repeating{false};
        bool m_scheduled{false};
        uint64_t m_generation{0};
    };

    void notifyBeforeWaiting() {
        auto snapshot = registeredObservers();
        for (auto *observer : snapshot) {
            auto &observers = registeredObservers();
            if (std::find(observers.begin(), observers.end(), observer) != observers.end()) {
                observer->fire();
            }
        }
    }

    struct TraceScope {
        TraceScope(uint64_t, uint64_t, uint64_t) {}
    };

    constexpr uint64_t PerformOpportunisticallyScheduledTasksStart = 1;
    constexpr uint64_t PerformOpportunisticallyScheduledTasksEnd = 2;

    class Document {
      public:
        explicit Document(bool hasPendingIdleCallback)
            : m_hasPendingIdleCallback(hasPendingIdleCallback) {}

        bool hasPendingIdleCallback() const {
            return m_hasPendingIdleCallback;
        }

      private:
        bool m_hasPendingIdleCallback{false};
    };

    class Settings {
      public:
        bool opportunisticSweepingAndGarbageCollectionEnabled() const {
            return m_gcEnabled;
        }

        void setGCEnabled(bool value) {
            m_gcEnabled = value;
        }

      private:
        bool m_gcEnabled{false};
    };

    class Page : public LifetimeTrackedBase {
      public:
        bool waitingForLoad{false};
        bool visibleAndActive{true};
        bool idleCallbackPending{false};
        unsigned idleCallbackCount{0};
        unsigned opportunisticTaskCount{0};
        std::function<void()> idleCallback;
        Settings pageSettings;

        bool isWaitingForLoadToFinish() const {
            return waitingForLoad;
        }

        bool isVisibleAndActive() const {
            return visibleAndActive;
        }

        template <typename Callback> bool findMatchingLocalDocument(Callback &&callback) const {
            Document document(idleCallbackPending);
            return callback(document);
        }

        Settings &settings() {
            return pageSettings;
        }

        Duration preferredRenderingUpdateInterval() const {
            return {0.016};
        }

        void opportunisticallyRunIdleCallbacks(MonotonicTime) {
            ++idleCallbackCount;
            idleCallbackPending = false;
            if (idleCallback) {
                idleCallback();
            }
        }

        void performOpportunisticallyScheduledTasks(MonotonicTime) {
            ++opportunisticTaskCount;
        }
    };

    class OpportunisticTaskScheduler : public LifetimeTrackedBase {
      public:
        explicit OpportunisticTaskScheduler(Page &page);
        ~OpportunisticTaskScheduler();

        bool isScheduled() const {
            return m_runLoopObserver->isScheduled();
        }

        bool retryTimerIsActive() const {
            return m_retryTimer && m_retryTimer->isActive();
        }

        bool retryTimerExists() const {
            return !!m_retryTimer;
        }

        uint64_t retryCount() const {
            return m_runloopCountAfterBeingScheduled;
        }

        bool hasImminentlyScheduledWork() const {
            return m_imminentlyScheduledWorkCount;
        }

        void setImminentlyScheduledWork(bool value) {
            m_imminentlyScheduledWorkCount = value ? 1 : 0;
        }

        void rescheduleIfNeeded(MonotonicTime deadline);

      private:
        void runLoopObserverFired();
        void cancelPendingTask();

        WeakPtr<Page> m_page;
        uint64_t m_imminentlyScheduledWorkCount{0};
        uint64_t m_runloopCountAfterBeingScheduled{0};
        MonotonicTime m_currentDeadline;
        const UniqueRef<RunLoopObserver> m_runLoopObserver;
        std::unique_ptr<RunLoop::Timer> m_retryTimer;
    };

    GENERATED_SCHEDULER_DEFINITIONS_TOKEN

}

class SourceHandle {
  public:
    explicit SourceHandle(GSource *source)
        : m_source(source) {}

    SourceHandle(const SourceHandle &) = delete;
    SourceHandle &operator=(const SourceHandle &) = delete;

    ~SourceHandle() {
        if (m_source) {
            g_source_destroy(m_source);
            g_source_unref(m_source);
        }
    }

    void attach() {
        g_source_attach(m_source, activeContext());
    }

    GSource *get() const {
        return m_source;
    }

  private:
    GSource *m_source{nullptr};
};

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

WebCore::MonotonicTime nearDeadline() {
    return WebCore::MonotonicTime{WebCore::testNow() + 0.005};
}

WebCore::MonotonicTime distantDeadline() {
    return WebCore::MonotonicTime{WebCore::testNow() + 10.0};
}

void runTurn() {
    WebCore::notifyBeforeWaiting();
    g_main_context_iteration(activeContext(), FALSE);
}

void scheduleFirstRetry(WebCore::Page &page, WebCore::OpportunisticTaskScheduler &scheduler) {
    page.idleCallbackPending = true;
    scheduler.setImminentlyScheduledWork(true);
    scheduler.rescheduleIfNeeded(distantDeadline());
    WebCore::notifyBeforeWaiting();
    require(scheduler.isScheduled(), "busy scheduler lost its activity observer");
    require(scheduler.retryTimerIsActive(), "busy scheduler did not arm its retry timer");
}

void testContinuousReadyTimerBound() {
    ContextScope context;
    WebCore::Page page;
    page.idleCallbackPending = true;
    WebCore::OpportunisticTaskScheduler scheduler(page);
    scheduler.setImminentlyScheduledWork(true);
    scheduler.rescheduleIfNeeded(distantDeadline());

    unsigned sharedTimerCalls = 0;
    SourceHandle sharedTimer(g_idle_source_new());
    g_source_set_priority(sharedTimer.get(), RunLoopSourcePriority::MainThreadSharedTimer);
    g_source_set_callback(
        sharedTimer.get(),
        [](gpointer data) -> gboolean {
            auto *calls = static_cast<unsigned *>(data);
            ++*calls;
            return G_SOURCE_CONTINUE;
        },
        &sharedTimerCalls,
        nullptr
    );
    sharedTimer.attach();

    unsigned sharedTimerCallsAtIdle = 0;
    page.idleCallback = [&] {
        sharedTimerCallsAtIdle = sharedTimerCalls;
    };

    for (unsigned turn = 0; turn < 12 && !page.idleCallbackCount; ++turn) {
        runTurn();
    }

    require(page.idleCallbackCount == 1, "continuous shared timer prevented bounded idle cleanup");
    require(sharedTimerCallsAtIdle == 9, "idle cleanup did not wait through nine ready timer turns");
    require(scheduler.retryCount() == 10, "continuous shared timer did not force cleanup at retry ten");
    require(!scheduler.isScheduled(), "settled cleanup left a repeating observer scheduled");
    require(!scheduler.retryTimerIsActive(), "settled cleanup left a retry timer active");
}

void testNoImminentWorkBound() {
    ContextScope context;
    WebCore::Page page;
    page.idleCallbackPending = true;
    WebCore::OpportunisticTaskScheduler scheduler(page);
    scheduler.rescheduleIfNeeded(nearDeadline());

    require(!scheduler.retryTimerExists(), "retry timer was allocated before the first retry");

    SourceHandle sharedTimer(g_idle_source_new());
    g_source_set_priority(sharedTimer.get(), RunLoopSourcePriority::MainThreadSharedTimer);
    g_source_set_callback(
        sharedTimer.get(),
        [](gpointer) -> gboolean {
            dispatchOrder().emplace_back("shared");
            return G_SOURCE_REMOVE;
        },
        nullptr,
        nullptr
    );
    sharedTimer.attach();

    runTurn();
    require(page.idleCallbackCount == 0, "idle callback ran before the shared timer");
    require(dispatchOrder().size() == 1 && dispatchOrder()[0] == "shared", "shared timer did not run first");
    require(configuredRetryPriority() == 11, "retry timer did not use priority 11");
    require(scheduler.retryTimerExists(), "first retry did not lazily allocate its timer");

    runTurn();
    require(retryWakeCount() == 1, "priority 11 retry source did not run after the shared timer");
    require(dispatchOrder().size() == 2 && dispatchOrder()[1] == "retry", "retry timer ordering changed");

    for (unsigned turn = 0; turn < 7 && !page.idleCallbackCount; ++turn) {
        runTurn();
    }

    require(page.idleCallbackCount == 1, "non-imminent task did not run within its five-retry bound");
    require(scheduler.retryCount() == 5, "non-imminent task ran at the wrong retry count");
    require(!scheduler.isScheduled(), "non-imminent cleanup left its observer scheduled");
    require(!scheduler.retryTimerIsActive(), "non-imminent cleanup left its retry timer active");

    bool watchdogFired = false;
    SourceHandle watchdog(g_timeout_source_new(20));
    g_source_set_priority(watchdog.get(), RunLoopSourcePriority::RunLoopDispatcher);
    g_source_set_callback(
        watchdog.get(),
        [](gpointer data) -> gboolean {
            *static_cast<bool *>(data) = true;
            return G_SOURCE_REMOVE;
        },
        &watchdogFired,
        nullptr
    );
    watchdog.attach();
    g_main_context_iteration(activeContext(), TRUE);
    require(watchdogFired, "settled idle did not block until external work arrived");
}

void testCancellationPaths() {
    {
        ContextScope context;
        WebCore::Page page;
        WebCore::OpportunisticTaskScheduler scheduler(page);
        scheduleFirstRetry(page, scheduler);
        page.waitingForLoad = true;
        scheduler.rescheduleIfNeeded(distantDeadline());
        require(!scheduler.isScheduled(), "loading page kept the observer scheduled");
        require(!scheduler.retryTimerIsActive(), "loading page kept the retry timer active");
    }

    {
        ContextScope context;
        WebCore::Page page;
        WebCore::OpportunisticTaskScheduler scheduler(page);
        scheduleFirstRetry(page, scheduler);
        page.visibleAndActive = false;
        scheduler.rescheduleIfNeeded(distantDeadline());
        require(!scheduler.isScheduled(), "hidden page kept the observer scheduled");
        require(!scheduler.retryTimerIsActive(), "hidden page kept the retry timer active");
    }

    {
        ContextScope context;
        WebCore::Page page;
        WebCore::OpportunisticTaskScheduler scheduler(page);
        page.idleCallbackPending = true;
        scheduler.setImminentlyScheduledWork(true);
        scheduler.rescheduleIfNeeded(nearDeadline());
        WebCore::notifyBeforeWaiting();
        require(scheduler.retryTimerIsActive(), "expiry case did not arm its retry timer");
        WebCore::testNow() += 0.1;
        WebCore::notifyBeforeWaiting();
        require(!scheduler.isScheduled(), "expired deadline kept the observer scheduled");
        require(!scheduler.retryTimerIsActive(), "expired deadline kept the retry timer active");
    }

    {
        ContextScope context;
        WebCore::Page page;
        WebCore::OpportunisticTaskScheduler scheduler(page);
        scheduleFirstRetry(page, scheduler);
        scheduler.rescheduleIfNeeded(WebCore::MonotonicTime{});
        WebCore::notifyBeforeWaiting();
        require(!scheduler.isScheduled(), "missing deadline kept the observer scheduled");
        require(!scheduler.retryTimerIsActive(), "missing deadline kept the retry timer active");
    }

    {
        ContextScope context;
        auto page = std::make_unique<WebCore::Page>();
        WebCore::OpportunisticTaskScheduler scheduler(*page);
        scheduleFirstRetry(*page, scheduler);
        page.reset();
        WebCore::notifyBeforeWaiting();
        require(!scheduler.isScheduled(), "lost page kept the observer scheduled");
        require(!scheduler.retryTimerIsActive(), "lost page kept the retry timer active");
    }
}

void testDestructionWithPendingRetry() {
    ContextScope context;
    WebCore::Page page;
    unsigned priorWakeCount = 0;
    {
        WebCore::OpportunisticTaskScheduler scheduler(page);
        scheduleFirstRetry(page, scheduler);
        priorWakeCount = retryWakeCount();
    }

    g_main_context_iteration(activeContext(), FALSE);
    require(retryWakeCount() == priorWakeCount, "destroyed scheduler left a retry callback attached");
}

void testPollWakesForFirstRetry() {
    ContextScope context;
    WebCore::Page page;
    WebCore::OpportunisticTaskScheduler scheduler(page);
    scheduleFirstRetry(page, scheduler);

    bool watchdogFired = false;
    SourceHandle watchdog(g_timeout_source_new(500));
    g_source_set_priority(watchdog.get(), RunLoopSourcePriority::RunLoopDispatcher);
    g_source_set_callback(
        watchdog.get(),
        [](gpointer data) -> gboolean {
            *static_cast<bool *>(data) = true;
            return G_SOURCE_REMOVE;
        },
        &watchdogFired,
        nullptr
    );
    watchdog.attach();

    gint maxPriority = 0;
    g_main_context_prepare(activeContext(), &maxPriority);
    gint queryTimeout = -1;
    g_main_context_query(activeContext(), maxPriority, &queryTimeout, nullptr, 0);
    gint64 start = g_get_monotonic_time();
    g_main_context_iteration(activeContext(), TRUE);
    gint64 elapsed = g_get_monotonic_time() - start;

    require(queryTimeout == 0, "GLib query did not see the zero-delay retry source");
    require(retryWakeCount() == 1, "blocking GLib poll did not dispatch the retry source");
    require(!watchdogFired, "retry source missed its bounded poll watchdog");
    require(elapsed < 250000, "retry source did not wake the blocking poll promptly");
}

void testHeavyCallbackExpirySettles() {
    ContextScope context;
    WebCore::Page page;
    page.idleCallbackPending = true;
    page.idleCallback = [] {
        WebCore::testNow() += 0.1;
    };
    WebCore::OpportunisticTaskScheduler scheduler(page);
    scheduler.rescheduleIfNeeded(nearDeadline());

    for (unsigned turn = 0; turn < 7 && !page.idleCallbackCount; ++turn) {
        runTurn();
    }

    require(page.idleCallbackCount == 1, "heavy idle callback did not run");
    require(!scheduler.isScheduled(), "expired callback deadline left its observer scheduled");
    require(!scheduler.retryTimerIsActive(), "expired callback deadline left its retry timer active");
}

void testReentrantRequest() {
    ContextScope context;
    WebCore::Page page;
    page.idleCallbackPending = true;
    WebCore::OpportunisticTaskScheduler scheduler(page);
    page.idleCallback = [&] {
        if (page.idleCallbackCount == 1) {
            WebCore::testNow() += 0.1;
            page.idleCallbackPending = true;
            scheduler.rescheduleIfNeeded(nearDeadline());
        }
    };
    scheduler.rescheduleIfNeeded(nearDeadline());

    for (unsigned turn = 0; turn < 7 && page.idleCallbackCount < 1; ++turn) {
        runTurn();
    }

    require(page.idleCallbackCount == 1, "initial idle callback did not run");
    require(scheduler.isScheduled(), "reentrant request was canceled when the first callback returned");

    page.idleCallback = {};
    for (unsigned turn = 0; turn < 7 && page.idleCallbackCount < 2; ++turn) {
        runTurn();
    }

    require(page.idleCallbackCount == 2, "reentrant idle request did not run");
    require(!scheduler.isScheduled(), "reentrant idle request left its observer scheduled");
}

void testDisabledBaseline() {
    ContextScope context;
    WebCore::Page page;
    page.idleCallbackPending = true;
    WebCore::OpportunisticTaskScheduler scheduler(page);
    scheduler.rescheduleIfNeeded(nearDeadline());

    for (unsigned turn = 0; turn < 7 && !page.idleCallbackCount; ++turn) {
        runTurn();
    }

    require(page.idleCallbackCount == 1, "disabled baseline changed the original non-imminent retry policy");
    require(scheduler.retryCount() == 5, "disabled baseline changed the original five retry threshold");
    require(retryWakeCount() == 0, "disabled policy started the opportunistic retry timer");
    require(!scheduler.retryTimerExists(), "disabled policy allocated its retry timer");
}

int main(int argc, char **argv) {
    try {
        if (argc != 2) {
            throw std::runtime_error("expected enabled or disabled policy");
        }

        bool expectedEnabled = std::string(argv[1]) == "enabled";
        require(
            WebCore::edenOpportunisticYieldEnabled() == expectedEnabled,
            "feature environment did not match the requested process mode"
        );

        if (expectedEnabled) {
            testContinuousReadyTimerBound();
            testNoImminentWorkBound();
            testCancellationPaths();
            testDestructionWithPendingRetry();
            testPollWakesForFirstRetry();
            testHeavyCallbackExpirySettles();
            testReentrantRequest();
        } else {
            testDisabledBaseline();
        }

        std::cout << (expectedEnabled ? "enabled" : "disabled") << " scheduler behavior passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
