#include "engine/wpe/wpedisplaysync.h"

#include <QEvent>
#include <QGuiApplication>
#include <QHash>
#include <QPointer>
#include <QQuickWindow>
#include <QScreen>
#include <QThread>
#include <atomic>
#include <functional>

namespace eden::engine {
    struct ScreenPulse {
        std::shared_ptr<WPEScreenSyncObserver> observer;
        std::atomic<bool> active{false};
        std::atomic<guint64> count{0};
        std::atomic<bool> valid{true};
        std::atomic<quintptr> sourceWindow{0};
        std::function<void()> requestFrame;
    };

    struct EdenWpeScreenObserver {
        WPEScreenSyncObserver parent;
        std::weak_ptr<ScreenPulse> *pulse;
    };
    struct EdenWpeScreenObserverClass {
        WPEScreenSyncObserverClass parent;
    };
    struct EdenWpeScreen {
        WPEScreen parent;
        std::shared_ptr<ScreenPulse> *pulse;
    };
    struct EdenWpeScreenClass {
        WPEScreenClass parent;
    };

    G_DEFINE_TYPE(EdenWpeScreenObserver, eden_wpe_screen_observer, WPE_TYPE_SCREEN_SYNC_OBSERVER)
    G_DEFINE_TYPE(EdenWpeScreen, eden_wpe_screen, WPE_TYPE_SCREEN)

    static void eden_wpe_screen_observer_init(EdenWpeScreenObserver *observer) {
        observer->pulse = new std::weak_ptr<ScreenPulse>;
    }

    static void eden_wpe_screen_observer_class_init(EdenWpeScreenObserverClass *klass) {
        G_OBJECT_CLASS(klass)->finalize = [](GObject *object) {
            delete reinterpret_cast<EdenWpeScreenObserver *>(object)->pulse;
            G_OBJECT_CLASS(eden_wpe_screen_observer_parent_class)->finalize(object);
        };
        WPE_SCREEN_SYNC_OBSERVER_CLASS(klass)->start = [](WPEScreenSyncObserver *observer) {
            if (auto pulse = reinterpret_cast<EdenWpeScreenObserver *>(observer)->pulse->lock()) {
                pulse->active.store(true);
                QMetaObject::invokeMethod(qApp, [pulse] { pulse->requestFrame(); }, Qt::QueuedConnection);
            }
        };
        WPE_SCREEN_SYNC_OBSERVER_CLASS(klass)->stop = [](WPEScreenSyncObserver *observer) {
            if (auto pulse = reinterpret_cast<EdenWpeScreenObserver *>(observer)->pulse->lock()) {
                pulse->active.store(false);
            }
        };
    }

    static void eden_wpe_screen_init(EdenWpeScreen *screen) {
        screen->pulse = new std::shared_ptr<ScreenPulse>;
    }

    static void eden_wpe_screen_class_init(EdenWpeScreenClass *klass) {
        G_OBJECT_CLASS(klass)->finalize = [](GObject *object) {
            delete reinterpret_cast<EdenWpeScreen *>(object)->pulse;
            G_OBJECT_CLASS(eden_wpe_screen_parent_class)->finalize(object);
        };
        WPE_SCREEN_CLASS(klass)->get_sync_observer = [](WPEScreen *screen) -> WPEScreenSyncObserver * {
            auto pulse = *reinterpret_cast<EdenWpeScreen *>(screen)->pulse;
            return pulse && pulse->valid.load() ? WPE_SCREEN_SYNC_OBSERVER(g_object_ref(pulse->observer.get()))
                                                : nullptr;
        };
        WPE_SCREEN_CLASS(klass)->invalidate = [](WPEScreen *screen) {
            if (auto pulse = *reinterpret_cast<EdenWpeScreen *>(screen)->pulse) {
                pulse->valid.store(false);
                pulse->active.store(false);
            }
            WPE_SCREEN_CLASS(eden_wpe_screen_parent_class)->invalidate(screen);
        };
    }

    class WpeDisplaySyncState final : public QObject {
      public:
        explicit WpeDisplaySyncState(WPEDisplay *display)
            : m_display(display),
              m_worker(new QObject) {
            m_thread.setObjectName("EdenWpeSync");
            m_worker->moveToThread(&m_thread);
            connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
            m_thread.start();
            for (auto *screen : QGuiApplication::screens()) {
                addScreen(screen);
            }
            connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen *screen) { addScreen(screen); });
            connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen *screen) {
                auto *native = m_screens.take(screen);
                if (!native) {
                    return;
                }
                wpe_screen_invalidate(native);
                wpe_display_screen_removed(m_display, native);
                g_object_unref(native);
                updateWindows();
            });
        }

        ~WpeDisplaySyncState() override {
            for (const auto &connection : std::as_const(m_frameConnections)) {
                disconnect(connection);
            }
            for (auto *screen : std::as_const(m_screens)) {
                wpe_screen_invalidate(screen);
            }
            m_thread.quit();
            m_thread.wait();
            for (auto *screen : std::as_const(m_screens)) {
                g_object_unref(screen);
            }
        }

        guint screenCount() const {
            return QGuiApplication::screens().size();
        }

        guint64 pulseCount() const {
            guint64 total = 0;
            for (auto *screen : std::as_const(m_screens)) {
                auto pulse = *reinterpret_cast<EdenWpeScreen *>(screen)->pulse;
                total += pulse->count.load(std::memory_order_relaxed);
            }
            return total;
        }

        WPEScreen *screen(guint index) const {
            return m_screens.value(QGuiApplication::screens().value(index));
        }

        void bind(WPEView *view, QQuickWindow *window, bool visible) {
            if (window) {
                const auto existing = m_views.constFind(view);
                if (existing != m_views.cend() && existing->window == window && existing->visible == visible) {
                    return;
                }
                m_views.insert(view, {window, visible});
                if (!m_frameConnections.contains(window)) {
                    window->installEventFilter(this);
                    connect(window, &QWindow::screenChanged, this, [this] { updateWindows(); });
                    connect(window, &QWindow::visibilityChanged, this, [this] { updateWindows(); });
                    connect(window, &QObject::destroyed, this, [this, window] {
                        m_frameConnections.remove(window);
                        updateWindows();
                    });
                    m_frameConnections.insert(window, {});
                }
            } else {
                m_views.remove(view);
            }
            updateWindows();
        }

      private:
        bool eventFilter(QObject *object, QEvent *event) override {
            if (event->type() == QEvent::Expose) {
                updateWindows();
            }
            return QObject::eventFilter(object, event);
        }

        struct ViewBinding {
            QPointer<QQuickWindow> window;
            bool visible;
        };

        void addScreen(QScreen *screen) {
            if (m_screens.contains(screen)) {
                return;
            }
            auto *native = WPE_SCREEN(g_object_new(eden_wpe_screen_get_type(), "id", ++m_nextScreenId, nullptr));
            auto pulse = std::make_shared<ScreenPulse>();
            auto *observer = WPE_SCREEN_SYNC_OBSERVER(g_object_new(eden_wpe_screen_observer_get_type(), nullptr));
            pulse->observer = std::shared_ptr<WPEScreenSyncObserver>(observer, [](WPEScreenSyncObserver *value) {
                g_object_unref(value);
            });
            *reinterpret_cast<EdenWpeScreenObserver *>(observer)->pulse = pulse;
            *reinterpret_cast<EdenWpeScreen *>(native)->pulse = pulse;
            QPointer<WpeDisplaySyncState> guard(this);
            pulse->requestFrame = [guard, screen] {
                if (guard) {
                    guard->requestFrame(screen);
                }
            };
            m_screens.insert(screen, native);
            updateScreen(screen);
            connect(screen, &QScreen::geometryChanged, this, [this, screen] { updateScreen(screen); });
            connect(screen, &QScreen::physicalSizeChanged, this, [this, screen] { updateScreen(screen); });
            connect(screen, &QScreen::physicalDotsPerInchChanged, this, [this, screen] { updateScreen(screen); });
            connect(screen, &QScreen::refreshRateChanged, this, [this, screen] { updateScreen(screen); });
            wpe_display_screen_added(m_display, native);
        }

        void updateScreen(QScreen *screen) {
            auto *native = m_screens.value(screen);
            if (!native) {
                return;
            }
            const auto geometry = screen->geometry();
            const auto physical = screen->physicalSize();
            wpe_screen_set_position(native, geometry.x(), geometry.y());
            wpe_screen_set_size(native, geometry.width(), geometry.height());
            wpe_screen_set_physical_size(native, qRound(physical.width()), qRound(physical.height()));
            wpe_screen_set_scale(native, screen->devicePixelRatio());
            wpe_screen_set_refresh_rate(native, qMax(1000, qRound(screen->refreshRate() * 1000)));
            updateWindows();
        }

        void updateWindows() {
            for (auto iterator = m_views.begin(); iterator != m_views.end();) {
                if (!iterator->window) {
                    iterator = m_views.erase(iterator);
                    continue;
                }
                if (auto *toplevel = wpe_view_get_toplevel(iterator.key())) {
                    auto *native = m_screens.value(iterator->window->screen());
                    if (g_object_get_data(G_OBJECT(toplevel), "eden-screen") != native) {
                        g_object_set_data_full(
                            G_OBJECT(toplevel),
                            "eden-screen",
                            native ? g_object_ref(native) : nullptr,
                            g_object_unref
                        );
                        wpe_toplevel_screen_changed(toplevel);
                    }
                }
                ++iterator;
            }
            for (auto iterator = m_frameConnections.begin(); iterator != m_frameConnections.end(); ++iterator) {
                disconnect(iterator.value());
                auto *window = iterator.key();
                auto *native = m_screens.value(window->screen());
                if (!native) {
                    iterator.value() = {};
                    continue;
                }
                auto pulse = *reinterpret_cast<EdenWpeScreen *>(native)->pulse;
                iterator.value() = connect(
                    window,
                    &QQuickWindow::frameSwapped,
                    m_worker,
                    [pulse, window] {
                        if (!pulse->valid.load() || !pulse->active.load() ||
                            pulse->sourceWindow.load() != reinterpret_cast<quintptr>(window)) {
                            return;
                        }
                        pulse->count.fetch_add(1, std::memory_order_relaxed);
                        WPE_SCREEN_SYNC_OBSERVER_GET_CLASS(pulse->observer.get())->sync(pulse->observer.get());
                        QMetaObject::invokeMethod(qApp, [pulse] { pulse->requestFrame(); }, Qt::QueuedConnection);
                    },
                    Qt::QueuedConnection
                );
            }
            for (auto *screen : m_screens.keys()) {
                requestFrame(screen);
            }
        }

        void requestFrame(QScreen *screen) {
            auto *native = m_screens.value(screen);
            if (!native) {
                return;
            }
            auto pulse = *reinterpret_cast<EdenWpeScreen *>(native)->pulse;
            if (!pulse->valid.load() || !pulse->active.load()) {
                pulse->sourceWindow.store(0);
                return;
            }
            QQuickWindow *selected = nullptr;
            for (const auto &binding : std::as_const(m_views)) {
                auto *window = binding.window.data();
                if (binding.visible && window && window->isVisible() && window->isExposed() &&
                    window->visibility() != QWindow::Minimized && window->screen() == screen) {
                    if (!selected || reinterpret_cast<quintptr>(window) == pulse->sourceWindow.load()) {
                        selected = window;
                    }
                }
            }
            pulse->sourceWindow.store(reinterpret_cast<quintptr>(selected));
            if (selected) {
                selected->update();
            }
        }

        WPEDisplay *m_display;
        QHash<QScreen *, WPEScreen *> m_screens;
        QHash<WPEView *, ViewBinding> m_views;
        QHash<QQuickWindow *, QMetaObject::Connection> m_frameConnections;
        guint32 m_nextScreenId{0};
        QThread m_thread;
        QObject *m_worker;
    };

    WpeDisplaySync::WpeDisplaySync(WPEDisplay *display)
        : m_state(std::make_unique<WpeDisplaySyncState>(display)) {}

    WpeDisplaySync::~WpeDisplaySync() = default;

    guint WpeDisplaySync::screenCount() const {
        return m_state->screenCount();
    }

    guint64 WpeDisplaySync::pulseCount() const {
        return m_state->pulseCount();
    }

    WPEScreen *WpeDisplaySync::screen(guint index) const {
        return m_state->screen(index);
    }

    void WpeDisplaySync::bind(WPEView *view, QQuickWindow *window, bool visible) {
        m_state->bind(view, window, visible);
    }
}
