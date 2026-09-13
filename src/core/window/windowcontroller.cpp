#include "core/window/windowcontroller.h"
#if EDEN_ENABLE_AUTOMATION
#include "core/automation/performancemetrics.h"
#endif
#include "core/bookmarks/bookmarkstore.h"
#include "core/downloads/downloadmanager.h"
#include "core/history/historystore.h"
#include "core/omni/omniboxcontroller.h"
#include "core/permissions/permissionstore.h"
#include "core/profiles/cookiehandoffcoordinator.h"
#include "core/profiles/profilecolors.h"
#include "core/profiles/profilecontext.h"
#include "core/profiles/profilemanager.h"
#include "core/profiles/profilesettings.h"
#include "core/profiles/sessionstore.h"
#include "core/profiles/windowregistry.h"
#include "core/settings/settingsstore.h"
#include "core/settings/shortcutregistry.h"
#include "core/urlsanitizer.h"
#include "core/window/tabmodel.h"
#include "core/window/thumbnailcache.h"
#include "engine/engineprofile.h"
#include "engine/engineprofilemap.h"
#include "engine/engineregistry.h"
#include "engine/engineview.h"
#include "passwords/credentialvault.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QCursor>
#include <QDataStream>
#include <QDir>
#include <QDrag>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPixmap>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QSet>
#include <QStringList>
#include <QUrlQuery>

#include <algorithm>
#include <chrono>
#include <functional>

#include <unistd.h>

namespace eden::core {

    struct TabDragSession {
        QPointer<WindowController> grabOwner;
        QPointer<WindowController> current;
        QPointer<WindowController> origin;
        QPointer<WindowController> tornWindow;
        QPointer<QQuickItem> area;
        QPointer<QQuickItem> view;
        QPointF cursorInArea;
        QPointF pointerInWindow;
        qreal pressOffset = 0;
        qreal normalExtent = 0;
        qreal pinnedExtent = 0;
        qreal spacing = 0;
        int currentIndex = -1;
        int originIndex = -1;
        bool vertical = false;
        bool pinned = false;
        bool torn = false;
        bool dnd = false;
        bool hovering = false;
        bool tornCreated = false;
        bool active = false;
    };

    static TabDragSession &tabDragSession() {
        static TabDragSession session;
        return session;
    }

    static constexpr qreal kTabTearOffMargin = 48;

    static int credentialMatchScore(const QString &candidate, const QString &query) {
        const QString normalizedCandidate = candidate.toCaseFolded();
        const QString normalizedQuery = query.trimmed().toCaseFolded();
        if (normalizedQuery.isEmpty()) {
            return 1;
        }
        if (normalizedCandidate.startsWith(normalizedQuery)) {
            return 3000 - normalizedCandidate.size();
        }
        const qsizetype containedAt = normalizedCandidate.indexOf(normalizedQuery);
        if (containedAt >= 0) {
            return 2000 - static_cast<int>(containedAt) * 4 - normalizedCandidate.size();
        }
        qsizetype candidateIndex = 0;
        int gaps = 0;
        for (const QChar character : normalizedQuery) {
            const qsizetype found = normalizedCandidate.indexOf(character, candidateIndex);
            if (found < 0) {
                return -1;
            }
            gaps += static_cast<int>(found - candidateIndex);
            candidateIndex = found + 1;
        }
        return 1000 - gaps * 4 - normalizedCandidate.size();
    }

    static qint64 residentMemoryKiB(qint64 processId) {
        if (processId <= 0) {
            return -1;
        }
        QFile status(QString("/proc/%1/status").arg(processId));
        if (!status.open(QIODevice::ReadOnly)) {
            return -1;
        }
        const QList<QByteArray> lines = status.readAll().split('\n');
        for (const QByteArray &line : lines) {
            if (!line.startsWith("VmRSS:")) {
                continue;
            }
            const QList<QByteArray> fields = line.simplified().split(' ');
            bool valid = false;
            const qint64 value = fields.size() > 1 ? fields.at(1).toLongLong(&valid) : 0;
            return valid ? value : -1;
        }
        return -1;
    }

    static qint64 browserResidentMemoryKiB() {
        const qint64 value = residentMemoryKiB(QCoreApplication::applicationPid());
        if (value >= 0) {
            return value;
        }
        QFile statm("/proc/self/statm");
        if (!statm.open(QIODevice::ReadOnly)) {
            return -1;
        }
        const QList<QByteArray> fields = statm.readAll().simplified().split(' ');
        bool valid = false;
        const qint64 pages = fields.size() > 1 ? fields.at(1).toLongLong(&valid) : 0;
        const long pageBytes = sysconf(_SC_PAGESIZE);
        return valid && pages >= 0 && pageBytes > 0 ? pages * pageBytes / 1024 : -1;
    }

    static QString memoryLabel(qint64 kibibytes) {
        if (kibibytes < 0) {
            return "Unavailable";
        }
        return QString::number(static_cast<double>(kibibytes) / 1024.0, 'f', 1) + " MB";
    }

    static QList<qint64> processChildren(qint64 processId) {
        QList<qint64> result;
        const QStringList tasks =
            QDir(QString("/proc/%1/task").arg(processId)).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Unsorted);
        for (const QString &task : tasks) {
            QFile children(QString("/proc/%1/task/%2/children").arg(processId).arg(task));
            if (!children.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QList<QByteArray> fields = children.readAll().simplified().split(' ');
            for (const QByteArray &field : fields) {
                bool valid = false;
                const qint64 child = field.toLongLong(&valid);
                if (valid && child > 0) {
                    result.append(child);
                    result.append(processChildren(child));
                }
            }
        }
        return result;
    }

    static QByteArray processCommandLine(qint64 processId) {
        QFile commandLine(QString("/proc/%1/cmdline").arg(processId));
        if (!commandLine.open(QIODevice::ReadOnly)) {
            return {};
        }
        QByteArray value = commandLine.readAll();
        value.replace('\0', ' ');
        return value;
    }

    struct RendererProcessSample {
        qint64 memoryKiB = 0;
        int processCount = 0;
    };

    static RendererProcessSample rendererProcessesMemory() {
        RendererProcessSample sample;
        for (qint64 processId : processChildren(QCoreApplication::applicationPid())) {
            if (!processCommandLine(processId).contains("--type=renderer")) {
                continue;
            }
            const qint64 memory = residentMemoryKiB(processId);
            if (memory < 0) {
                continue;
            }
            sample.memoryKiB += memory;
            ++sample.processCount;
        }
        return sample;
    }

    static QVariantMap sharedProcessMemory() {
        qint64 gpuMemory = 0;
        qint64 networkMemory = 0;
        bool gpuAvailable = false;
        bool networkAvailable = false;
        for (qint64 processId : processChildren(QCoreApplication::applicationPid())) {
            const QByteArray commandLine = processCommandLine(processId);
            const qint64 memory = residentMemoryKiB(processId);
            if (memory < 0) {
                continue;
            }
            if (commandLine.contains("--type=gpu-process")) {
                gpuMemory += memory;
                gpuAvailable = true;
            } else if (
                commandLine.contains("--type=utility") &&
                (commandLine.contains("network.mojom.NetworkService") || commandLine.contains("network-service"))
            ) {
                networkMemory += memory;
                networkAvailable = true;
            }
        }
        QVariantMap result;
        result.insert("sharedBrowserMemory", memoryLabel(browserResidentMemoryKiB()));
        result.insert("sharedGpuMemory", gpuAvailable ? memoryLabel(gpuMemory) : QString("Unavailable"));
        result.insert("sharedNetworkMemory", networkAvailable ? memoryLabel(networkMemory) : QString("Unavailable"));
        return result;
    }

    static void loadDropMetrics(TabDragSession &session, QQuickItem *area) {
        session.area = area;
        session.view = area ? area->findChild<QQuickItem *>("tabDropView") : nullptr;
        if (!area) {
            return;
        }
        const bool wasVertical = session.vertical;
        session.vertical = area->property("verticalDropLayout").toBool();
        session.normalExtent = area->property("dropNormalExtent").toReal();
        session.pinnedExtent = area->property("dropPinnedExtent").toReal();
        session.spacing = area->property("dropSpacing").toReal();
        const qreal draggedExtent = session.pinned ? session.pinnedExtent : session.normalExtent;
        if (wasVertical != session.vertical) {
            session.pressOffset = draggedExtent / 2;
        }
        session.pressOffset = std::clamp(session.pressOffset, 0.0, std::max(0.0, draggedExtent));
    }

    static qreal dragLeadingPosition(const TabDragSession &session) {
        QQuickItem *view = session.view;
        const QPointF local = session.area->mapToItem(view, session.cursorInArea);
        const qreal content = session.vertical ? local.y() + view->property("contentY").toReal()
                                               : local.x() + view->property("contentX").toReal();
        return content - session.pressOffset;
    }

    static int dragSlotIndex(const TabDragSession &session, qreal leading, int pinnedCount, int count, bool insertion) {
        const qreal pinnedStep = session.pinnedExtent + session.spacing;
        const qreal normalStep = session.normalExtent + session.spacing;
        if (session.pinned) {
            const int slot = pinnedStep > 0 ? qRound(leading / pinnedStep) : 0;
            return std::clamp(slot, 0, insertion ? pinnedCount : std::max(0, pinnedCount - 1));
        }
        const qreal normalStart = pinnedCount * pinnedStep;
        const int slot = pinnedCount + (normalStep > 0 ? qRound((leading - normalStart) / normalStep) : 0);
        return std::clamp(slot, pinnedCount, insertion ? count : std::max(pinnedCount, count - 1));
    }

    WindowController::WindowController(QObject *parent)
        : QObject(parent),
          m_shortcuts(std::make_unique<ShortcutRegistry>()) {
        connect(this, &WindowController::currentEngineChanged, this, &WindowController::currentUrlChanged);
        connect(this, &WindowController::currentEngineChanged, this, [this] {
            if (m_contentFullscreen && (!m_fullscreenEngine || m_fullscreenEngine != currentEngine())) {
                exitContentFullscreen();
            }
        });
        connect(m_shortcuts.get(), &ShortcutRegistry::commandTriggered, this, [this](const QString &command) {
            executeCommand(command);
        });
        m_tabDragGuard.setInterval(200);
        connect(&m_tabDragGuard, &QTimer::timeout, this, [this] {
            const TabDragSession &session = tabDragSession();
            if (session.active && !session.dnd && session.grabOwner == this &&
                !(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
                endTabDrag(false);
            }
        });
        m_previewCaptureClock.start();
        m_previewCaptureTimer.setSingleShot(true);
        connect(&m_previewCaptureTimer, &QTimer::timeout, this, [this] {
            const QSet<quint64> pending = std::exchange(m_pendingPreviewTabIds, {});
            for (quint64 tabId : pending) {
                const int index = m_tabs ? m_tabs->indexForTabId(tabId) : -1;
                if (index >= 0) {
                    captureTabPreview(index);
                }
            }
        });
    }

    WindowController::~WindowController() {
        if (m_window) {
            m_window->removeEventFilter(this);
        }
        if (tabDragSession().active && tabDragSession().grabOwner == this) {
            tabDragSession() = {};
        }
        if (m_registeredAsWindow) {
            prepareToClose();
        }
    }

    TabModel *WindowController::tabs() const {
        return m_tabs.get();
    }

    int WindowController::activeIndex() const {
        return m_activeIndex;
    }

    QObject *WindowController::currentEngine() const {
        return m_tabs ? m_tabs->engineAt(m_activeIndex) : nullptr;
    }

    QUrl WindowController::currentUrl() const {
        if (!m_tabs || m_activeIndex < 0 || m_activeIndex >= m_tabs->rowCount()) {
            return {};
        }
        return urlWithoutCredentials(m_tabs->data(m_tabs->index(m_activeIndex), TabModel::UrlRole).toUrl());
    }

    QString WindowController::displayUrl() const {
        QUrl url = currentUrl();
        if (url.scheme() == "eden" || url.scheme() == "about") {
            return url.toString();
        }
        url.setQuery(QString());
        url.setFragment(QString());
        return url.toDisplayString(QUrl::RemovePassword);
    }

    QString WindowController::mode() const {
        return m_privateWindow ? "private" : "normal";
    }

    OmniboxController *WindowController::omnibox() const {
        return m_omnibox.get();
    }

    ShortcutRegistry *WindowController::shortcuts() const {
        return m_shortcuts.get();
    }

    HistoryStore *WindowController::history() const {
        return m_context ? m_context->history() : nullptr;
    }

    BookmarkStore *WindowController::bookmarks() const {
        return m_context ? m_context->bookmarks() : nullptr;
    }

    DownloadManager *WindowController::downloads() const {
        if (m_privateWindow) {
            return m_privateDownloads.get();
        }
        return m_context ? m_context->downloads() : nullptr;
    }

    ProfileSettings *WindowController::profileSettings() const {
        return m_context ? m_context->settings() : nullptr;
    }

    QObject *WindowController::credentialVault() const {
        return m_context ? m_context->credentialVault() : nullptr;
    }

    QObject *WindowController::permissionStore() const {
        return m_context ? m_context->permissions() : nullptr;
    }

    QString WindowController::profileId() const {
        return m_context ? m_context->idString() : QString();
    }

    QString WindowController::profileDisplayName() const {
        return m_context ? m_context->displayName() : QString();
    }

    QString WindowController::profileAvatarUrl() const {
        if (!m_context) {
            return {};
        }
        return QStringLiteral("image://eden-profile-avatar/%1?revision=%2")
            .arg(m_context->idString())
            .arg(m_context->avatarRevision());
    }

    QColor WindowController::profileColor() const {
        return profileColorForSeed(m_context ? m_context->colorSeed() : 0);
    }

    void WindowController::notifyProfileIdentityChanged() {
        emit profileIdentityChanged();
    }

    QString WindowController::openPane() const {
        return m_openPane;
    }

    int WindowController::paneWidth() const {
        return m_paneWidth;
    }

    bool WindowController::sidebarExpanded() const {
        return m_sidebarExpanded;
    }

    bool WindowController::findVisible() const {
        return m_findVisible;
    }

    bool WindowController::commandPaletteVisible() const {
        return m_commandPaletteVisible;
    }

    bool WindowController::contentFullscreen() const {
        return m_contentFullscreen;
    }

    QVariantList WindowController::pageContextMenuActions() const {
        return m_pageContextMenuActions;
    }

    QPoint WindowController::pageContextMenuPosition() const {
        return m_pageContextMenuPosition;
    }

    QString WindowController::pageContextMenuSurface() const {
        return m_pageContextMenuSurface;
    }

    QVariantMap WindowController::javaScriptDialog() const {
        return m_javaScriptDialog;
    }

    QVariantMap WindowController::permissionRequest() const {
        return m_permissionRequest;
    }

    QVariantMap WindowController::displayCaptureRequest() const {
        return m_displayCaptureRequest;
    }

    QVariantMap WindowController::fileDialog() const {
        return m_fileDialog;
    }

    QVariantMap WindowController::credentialPrompt() const {
        return m_credentialPrompt;
    }

    QString WindowController::credentialDraftUsername() const {
        return m_credentialPrompt.value("username").toString();
    }

    QString WindowController::credentialDraftPassword() const {
        return m_pendingCredentialPassword;
    }

    QVariantList WindowController::autofillSuggestions() const {
        return m_autofillSuggestions;
    }

    QPoint WindowController::autofillPopupPosition() const {
        return {
            qRound(m_autofillField.value("x").toDouble()),
            qRound(m_autofillField.value("y").toDouble() + m_autofillField.value("height").toDouble())
        };
    }

    bool WindowController::credentialKeyVisible() const {
        if (!m_credentialPrompt.isEmpty()) {
            return true;
        }
        return std::any_of(
            m_autofillSuggestions.cbegin(),
            m_autofillSuggestions.cend(),
            [](const QVariant &suggestion) { return suggestion.toMap().value("kind").toString() == "credential"; }
        );
    }

    int WindowController::tabDragRevision() const {
        return m_tabDragRevision;
    }

    int WindowController::tabPreviewRevision() const {
        return m_tabPreviewRevision;
    }

    bool WindowController::isInitialized() const {
        return m_initialized;
    }

    bool WindowController::isPrivateWindow() const {
        return m_privateWindow;
    }

    std::shared_ptr<ProfileContext> WindowController::profileContext() const {
        return m_context;
    }

    QQuickWindow *WindowController::window() const {
        return m_window.data();
    }

    void WindowController::initialize(
        const std::shared_ptr<ProfileContext> &context,
        bool privateWindow,
        const QString &engineName,
        bool createInitialTab
    ) {
        if (m_initialized || !context || context->activate() != ProfileError::None) {
            return;
        }
        m_initialized = true;
        m_context = context;
        m_privateWindow = privateWindow;
        connect(context->settings(), &ProfileSettings::errorChanged, this, [this] {
            if (m_context->settings()->error() != ProfileError::None) {
                emit transientMessageRequested(m_context->settings()->errorMessage());
            }
        });
        connect(context->sessions(), &SessionStore::errorChanged, this, [this] {
            if (m_context->sessions()->error() != ProfileError::None) {
                emit transientMessageRequested(m_context->sessions()->errorMessage());
            }
        });
        connect(context->history(), &HistoryStore::errorChanged, this, [this] {
            if (m_context->history()->error() != ProfileError::None) {
                emit transientMessageRequested(profileErrorMessage(m_context->history()->error()));
            }
        });
        engine::EngineRegistry *registry = engine::EngineRegistry::instance();
        const QString requestedEngineName = engineName.isEmpty() ? context->settings()->defaultEngine() : engineName;
        const std::optional<engine::Backend> requestedBackend = registry->backendForId(requestedEngineName);
        m_defaultBackend = requestedBackend.value_or(
            registry->descriptors().isEmpty() ? engine::Backend::QtWebEngine
                                              : registry->descriptors().constFirst().backend
        );
        m_engineName = registry->idForBackend(m_defaultBackend);
        QObject *windowCandidate = parent();
        while (windowCandidate && !m_window) {
            m_window = qobject_cast<QQuickWindow *>(windowCandidate);
            windowCandidate = windowCandidate->parent();
        }
        if (m_window) {
            m_window->installEventFilter(this);
            m_windowedSize = m_window->size();
            m_geometryPersistTimer.setSingleShot(true);
            m_geometryPersistTimer.setInterval(400);
            connect(&m_geometryPersistTimer, &QTimer::timeout, this, [this] {
                if (!m_window) {
                    return;
                }
                const QWindow::Visibility visibility = m_window->visibility();
                if (visibility != QWindow::Windowed && visibility != QWindow::Maximized) {
                    return;
                }
                SettingsStore::instance()->setWindowGeometry(
                    m_windowedSize.width(),
                    m_windowedSize.height(),
                    visibility == QWindow::Maximized
                );
            });
            const auto trackGeometry = [this] {
                if (m_window && m_window->visibility() == QWindow::Windowed) {
                    m_windowedSize = m_window->size();
                }
                m_geometryPersistTimer.start();
            };
            connect(m_window, &QWindow::widthChanged, this, trackGeometry);
            connect(m_window, &QWindow::heightChanged, this, trackGeometry);
            connect(m_window, &QWindow::visibilityChanged, this, trackGeometry);
        }
        WindowRegistry::instance()->registerWindow(m_window, this, context->idString(), privateWindow);
        context->registerWindow(this);
        m_registeredAsWindow = true;
        if (privateWindow) {
            m_privateDownloads = std::make_unique<DownloadManager>();
            m_privateProfiles = context->createPrivateEngineProfileMap();
        }
        if (!requestedBackend) {
            qWarning().noquote() << "The requested engine is not available in this build, using" << m_engineName;
        }
        auto profileResolver = [this](engine::Backend backend) -> std::shared_ptr<engine::EngineProfile> {
            if (m_privateWindow) {
                std::shared_ptr<engine::EngineProfile> profile = m_privateProfiles->profile(backend);
                connectPrivateProfile(profile.get());
                return profile;
            }
            return m_context->engineProfile(backend);
        };
        auto factory = [](engine::Backend backend, engine::EngineProfile *profile) {
            return engine::EngineFactory::create(backend, profile);
        };
        m_tabs = std::make_unique<TabModel>(
            std::move(factory),
            std::move(profileResolver),
            registry,
            m_defaultBackend,
            privateWindow,
            context->idString()
        );
        m_tabs->setConversionGate([this](
                                      std::shared_ptr<engine::EngineProfile> source,
                                      engine::Backend destination,
                                      std::function<void()> proceed
                                  ) {
            std::shared_ptr<ProfileContext> context = m_context;
            if (!context || context->signOutInProgress() || m_privateWindow) {
                proceed();
                return;
            }
            std::shared_ptr<engine::EngineProfile> destinationProfile = context->engineProfile(destination);
            if (!destinationProfile) {
                proceed();
                return;
            }
            const QString destinationName = engine::EngineRegistry::instance()->displayName(destination);
            QPointer<WindowController> guard(this);
            context->cookieHandoff()->requestHandoff(
                std::move(source),
                std::move(destinationProfile),
                [guard, destinationName, proceed = std::move(proceed)](ProfileError error, const QString &) {
                    if (guard && error != ProfileError::None && error != ProfileError::Cancelled) {
                        emit guard->transientMessageRequested(
                            QStringLiteral(
                                "Eden could not carry all sign-in data to %1. You may need to sign in again."
                            )
                                .arg(destinationName)
                        );
                    }
                    proceed();
                }
            );
        });
        m_thumbnailCache = std::make_unique<ThumbnailCache>();
        m_thumbnailCache->registerProvider(qmlEngine(this));
        m_devToolsPaneWidth = SettingsStore::instance()->devToolsPaneWidth();
        m_devToolsPaneHeight = SettingsStore::instance()->devToolsPaneHeight();
        emit devToolsPaneSizeChanged();
        connect(context->settings(), &ProfileSettings::defaultEngineChanged, this, [this] {
            switchEngine(m_context->settings()->defaultEngine());
        });
        connect(SettingsStore::instance(), &SettingsStore::devToolsPlacementChanged, this, [this] {
            const engine::EngineView::DevToolsPlacement placement =
                SettingsStore::instance()->devToolsPlacement() == "bottom" ? engine::EngineView::DevToolsBottom
                                                                           : engine::EngineView::DevToolsRight;
            for (int row = 0; row < m_tabs->rowCount(); ++row) {
                engine::EngineView *view = m_tabs->engineViewAt(row);
                if (view && !view->devToolsOpen()) {
                    view->setDevToolsPlacement(placement);
                }
            }
        });
        m_omnibox = std::make_unique<OmniboxController>(
            m_tabs.get(),
            context->history(),
            context->bookmarks(),
            context->settings()
        );
        connect(m_tabs.get(), &TabModel::tabAdded, this, [this](int index, bool background) {
            if (!background) {
                setActiveIndex(index);
            }
        });
        connect(m_tabs.get(), &TabModel::tabEngineCreated, this, [this](int, engine::EngineView *view) {
            connectEngine(view);
        });
        connect(m_tabs.get(), &TabModel::tabTransferredOut, this, [this](engine::EngineView *view) {
            if (view) {
                disconnect(view, nullptr, this, nullptr);
            }
            if (m_pageContextMenuEngine == view) {
                dismissPageContextMenu();
            }
            if (m_javaScriptDialogEngine == view) {
                resolveJavaScriptDialog(false);
            }
            if (m_permissionRequestEngine == view) {
                dismissPermissionRequest();
            }
            if (m_displayCaptureRequestEngine == view) {
                resolveDisplayCaptureRequest();
            }
            if (m_fileDialogEngine == view) {
                resolveFileDialog(false);
            }
        });
        connect(
            m_tabs.get(),
            &QAbstractItemModel::dataChanged,
            this,
            [this](const QModelIndex &, const QModelIndex &, const QList<int> &roles) {
                if (roles.isEmpty() || roles.contains(TabModel::ActivityIndicatorsRole)) {
                    ++m_tabPreviewRevision;
                    emit tabPreviewRevisionChanged();
                }
            }
        );
        connect(m_tabs.get(), &TabModel::operationOccurred, this, [this] { scheduleSessionSave(); });
        connect(m_tabs.get(), &TabModel::tabMoved, this, [this](int from, int to) {
            int nextActiveIndex = m_activeIndex;
            if (m_activeIndex == from) {
                nextActiveIndex = to;
            } else if (from < m_activeIndex && m_activeIndex <= to) {
                --nextActiveIndex;
            } else if (to <= m_activeIndex && m_activeIndex < from) {
                ++nextActiveIndex;
            }
            setActiveIndex(nextActiveIndex);
        });
        connect(
            m_tabs.get(),
            &TabModel::externalViewRequested,
            this,
            [this](engine::EngineNewViewRequest *request, int sourceIndex, const QString &backendId) {
                if (!request) {
                    return;
                }
                engine::EngineView *target = nullptr;
                if (request->disposition() == engine::EngineView::Disposition::CurrentTab) {
                    target = m_tabs->engineViewAt(sourceIndex);
                } else if (request->disposition() == engine::EngineView::Disposition::NewWindow) {
                    WindowController *destination =
                        ProfileManager::instance()
                            ->createBrowserWindow(m_context, m_privateWindow, backendId, false, false);
                    if (destination) {
                        const int row = destination->newTab(QUrl("about:blank"), false, backendId);
                        target = destination->tabs()->engineViewAt(row);
                    }
                } else {
                    const bool background = request->disposition() == engine::EngineView::Disposition::NewBackgroundTab;
                    const int row = newTab(QUrl("about:blank"), background, backendId);
                    target = m_tabs->engineViewAt(row);
                }
                if (!target) {
                    return;
                }
                if (!request->openIn(target)) {
                    target->load(request->requestedUrl().isEmpty() ? QUrl("about:blank") : request->requestedUrl());
                }
            }
        );
        connect(m_tabs.get(), &TabModel::tabCloseRequestedForWindow, this, [this] {
            const TabDragSession &session = tabDragSession();
            if (session.active && m_window && (session.grabOwner == this || session.tornWindow == this)) {
                m_closeDeferredForTabDrag = true;
                m_window->setVisible(false);
                return;
            }
            emit closeWindowRequested();
        });
        connect(m_tabs.get(), &QAbstractItemModel::rowsRemoved, this, [this](const QModelIndex &, int first, int last) {
            if (!m_tabs || m_tabs->rowCount() == 0) {
                setActiveIndex(-1);
                return;
            }
            const bool activeRemoved = m_activeIndex >= first && m_activeIndex <= last;
            int nextActiveIndex = m_activeIndex;
            if (m_activeIndex > last) {
                nextActiveIndex = m_activeIndex - (last - first + 1);
            } else if (activeRemoved) {
                nextActiveIndex = first;
            }
            nextActiveIndex = std::min(nextActiveIndex, m_tabs->rowCount() - 1);
            if (nextActiveIndex == m_activeIndex) {
                if (activeRemoved) {
                    emit currentEngineChanged();
                    emit currentBookmarkedChanged();
                    scheduleSessionSave();
                }
                return;
            }
            setActiveIndex(nextActiveIndex);
        });
        connect(
            m_tabs.get(),
            &QAbstractItemModel::rowsAboutToBeRemoved,
            this,
            [this](const QModelIndex &, int first, int last) {
                if (!m_thumbnailCache) {
                    return;
                }
                for (int row = first; row <= last; ++row) {
                    m_previewCaptureTimes.remove(m_tabs->tabIdAt(row));
                    m_pendingPreviewTabIds.remove(m_tabs->tabIdAt(row));
                    m_previewCapturesInFlight.remove(m_tabs->tabIdAt(row));
                    m_thumbnailCache->remove(m_tabs->tabIdAt(row));
                }
                ++m_tabPreviewRevision;
                emit tabPreviewRevisionChanged();
            }
        );
        if (BookmarkStore *bookmarkStore = context->bookmarks()) {
            connect(bookmarkStore, &QAbstractItemModel::rowsInserted, this, [this] {
                emit currentBookmarkedChanged();
            });
            connect(bookmarkStore, &QAbstractItemModel::rowsRemoved, this, [this] { emit currentBookmarkedChanged(); });
            connect(bookmarkStore, &QAbstractItemModel::modelReset, this, [this] { emit currentBookmarkedChanged(); });
        }
        emit tabsChanged();
        emit modeChanged();
        emit profileChanged();
        emit profileIdentityChanged();
        if (createInitialTab && m_tabs->rowCount() == 0) {
            newTab();
        }
    }

    void WindowController::setActiveIndex(int index) {
        if (!m_tabs) {
            index = -1;
        } else if (index < -1 || index >= m_tabs->rowCount()) {
            return;
        }
        if (m_activeIndex == index) {
            return;
        }
        if (qEnvironmentVariableIsSet("EDEN_PERF") && m_window) {
            const auto started = std::chrono::steady_clock::now();
            auto connection = std::make_shared<QMetaObject::Connection>();
            *connection = connect(m_window, &QQuickWindow::frameSwapped, m_window, [connection, started] {
                disconnect(*connection);
                const double milliseconds =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
#if EDEN_ENABLE_AUTOMATION
                PerformanceMetrics::record("tabswitch.input_to_frame_ms", milliseconds);
#endif
                qInfo("EDEN_PERF tabswitch.input_to_frame_ms=%.3f", milliseconds);
            });
        }
        if (m_activeIndex >= 0) {
            captureTabPreview(m_activeIndex);
        }
        if (index >= 0) {
            m_tabs->ensureEngine(index);
        }
        dismissPageContextMenu();
        resolveJavaScriptDialog(false);
        dismissPermissionRequest();
        resolveDisplayCaptureRequest();
        resolveFileDialog(false);
        m_activeIndex = index;
        emit activeIndexChanged();
        emit currentEngineChanged();
        emit currentBookmarkedChanged();
        refreshAutofillSuggestions();
        scheduleSessionSave();
        if (index >= 0 && !m_tabs->data(m_tabs->index(index), TabModel::InternalPageRole).toString().isEmpty()) {
            QPointer<WindowController> guard(this);
            QTimer::singleShot(350, this, [guard, index] {
                if (guard && guard->m_activeIndex == index) {
                    guard->captureTabPreview(index);
                }
            });
        }
    }

    int WindowController::newTab(const QUrl &url, bool background, const QString &backendId) {
        if (m_context && m_context->signOutInProgress()) {
            return -1;
        }
        const QUrl destination = url.isEmpty() && m_context ? m_context->settings()->newTabDestination() : url;
        return m_tabs ? m_tabs->addTab(destination, background, backendId) : -1;
    }

    bool WindowController::switchEngine(const QString &backendId) {
        if (!m_tabs || !m_tabs->setDefaultBackend(backendId)) {
            return false;
        }
        if (m_activeIndex >= 0) {
            captureTabPreview(m_activeIndex);
        }
        if (!m_tabs->convertAllTabs(backendId, m_activeIndex)) {
            return false;
        }
        m_engineName = backendId;
        m_defaultBackend = engine::EngineRegistry::instance()->backendForId(backendId).value_or(m_defaultBackend);
        emit currentEngineChanged();
        scheduleSessionSave();
        return true;
    }

    QVariantList WindowController::tabContextMenuActions(int index) const {
        if (!m_tabs || index < 0 || index >= m_tabs->rowCount()) {
            return {};
        }
        const auto action =
            [](const QString &id, const QString &title, const QString &icon = {}, const QVariantList &children = {}) {
                QVariantMap value;
                value.insert("id", id);
                value.insert("title", title);
                value.insert("icon", icon);
                if (!children.isEmpty()) {
                    value.insert("children", children);
                }
                return value;
            };
        QVariantList openIn;
        const QString currentBackend = m_tabs->backendIdAt(index);
        for (const engine::EngineDescriptor &descriptor : engine::EngineRegistry::instance()->descriptors()) {
            if (descriptor.id != currentBackend) {
                openIn.append(action("open_in:" + descriptor.id, descriptor.displayName, "globe"));
            }
        }
        QVariantList actions = {
            action("new", "New tab", "add"),
            action("reload", "Reload", "refresh"),
            action("duplicate", "Duplicate", "copy"),
            action("pin", m_tabs->data(m_tabs->index(index), TabModel::PinnedRole).toBool() ? "Unpin" : "Pin", "pin"),
            action(
                "mute",
                m_tabs->data(m_tabs->index(index), TabModel::MutedRole).toBool() ? "Unmute" : "Mute",
                "muted"
            ),
        };
        if (!openIn.isEmpty()) {
            actions.append(action("open_in", "Open in", "globe", openIn));
        }
        actions.append(action("close", "Close", "close"));
        actions.append(action("close_others", "Close others", "trash"));
        return actions;
    }

    void WindowController::executeTabContextMenuCommand(int index, const QString &command) {
        if (!m_tabs || index < 0 || index >= m_tabs->rowCount()) {
            return;
        }
        if (command.startsWith("open_in:")) {
            const QString backendId = command.sliced(8);
            captureTabPreview(index);
            m_tabs->convertTab(index, backendId, index == m_activeIndex);
            emit currentEngineChanged();
            scheduleSessionSave();
        } else if (command == "new") {
            newTabAndFocusOmnibox();
        } else if (command == "reload") {
            if (engine::EngineView *view = m_tabs->engineViewAt(index)) {
                view->reload();
            }
        } else if (command == "duplicate") {
            m_tabs->duplicateTab(index);
        } else if (command == "pin") {
            m_tabs->pinTab(index, !m_tabs->data(m_tabs->index(index), TabModel::PinnedRole).toBool());
        } else if (command == "mute") {
            m_tabs->toggleMuted(index);
        } else if (command == "close") {
            closeTab(index);
        } else if (command == "close_others") {
            m_tabs->closeOthers(index);
        }
    }

    QVariantMap WindowController::tabPreview(int index) {
        if (!m_tabs || index < 0 || index >= m_tabs->rowCount()) {
            return {};
        }
        QVariantMap preview = tabPreviewMetadata(index, true);
        if (m_thumbnailCache) {
            const QVariant thumbnail = m_thumbnailCache->value(m_tabs->tabIdAt(index)).value("thumbnail");
            if (thumbnail.isValid()) {
                preview.insert("thumbnail", thumbnail);
            }
        }
        return preview;
    }

    QVariantMap WindowController::tabPreviewMetadata(int index, bool sampleMemory) const {
        if (!m_tabs || index < 0 || index >= m_tabs->rowCount()) {
            return {};
        }
        const QModelIndex modelIndex = m_tabs->index(index);
        QVariantMap preview;
        preview.insert("title", m_tabs->data(modelIndex, TabModel::TitleRole));
        preview.insert(
            "url",
            m_tabs->data(modelIndex, TabModel::UrlRole).toUrl().toDisplayString(QUrl::RemovePassword)
        );
        preview.insert("favicon", m_tabs->data(modelIndex, TabModel::FaviconRole));
        preview.insert("engine", m_tabs->data(modelIndex, TabModel::EngineNameRole));
        preview.insert("activityIndicators", m_tabs->data(modelIndex, TabModel::ActivityIndicatorsRole));
        const bool discarded = m_tabs->isDiscarded(index);
        const bool internal = !m_tabs->data(modelIndex, TabModel::InternalPageRole).toString().isEmpty();
        if (discarded) {
            preview.insert("rendererMemory", "0.0 MB");
            preview.insert("rendererAttribution", "Discarded");
        } else if (internal) {
            preview.insert("rendererMemory", "0.0 MB");
            preview.insert("rendererAttribution", "Shell page");
        } else if (sampleMemory) {
            const qint64 processId = m_tabs->rendererProcessIdAt(index);
            const qint64 memory = residentMemoryKiB(processId);
            preview.insert("rendererPid", processId);
            if (processId > 0 && memory >= 0) {
                preview.insert("rendererMemory", memoryLabel(memory));
                preview.insert(
                    "rendererAttribution",
                    m_tabs->rendererProcessUseCount(processId) > 1 ? "Shared renderer" : "Dedicated renderer"
                );
            } else {
                const RendererProcessSample sample = rendererProcessesMemory();
                if (sample.processCount == 1) {
                    preview.insert("rendererMemory", memoryLabel(sample.memoryKiB));
                    preview.insert("rendererAttribution", "Dedicated renderer");
                } else if (sample.processCount > 1) {
                    preview.insert("rendererMemory", memoryLabel(sample.memoryKiB));
                    preview.insert(
                        "rendererAttribution",
                        QString("All renderers (%1 processes)").arg(sample.processCount)
                    );
                } else {
                    preview.insert("rendererMemory", "Unavailable");
                    preview.insert("rendererAttribution", "Renderer mapping unavailable");
                }
            }
        } else {
            preview.insert("rendererMemory", "Unavailable");
            preview.insert("rendererAttribution", "Not sampled yet");
        }
        const QVariantMap shared = sampleMemory ? sharedProcessMemory()
                                                : QVariantMap{
                                                      {"sharedBrowserMemory", "Unavailable"},
                                                      {"sharedGpuMemory", "Unavailable"},
                                                      {"sharedNetworkMemory", "Unavailable"}
                                                  };
        for (auto iterator = shared.cbegin(); iterator != shared.cend(); ++iterator) {
            preview.insert(iterator.key(), iterator.value());
        }
        return preview;
    }

    void WindowController::captureTabPreview(int index) {
        if (!m_tabs || !m_thumbnailCache || index < 0 || index >= m_tabs->rowCount()) {
            return;
        }
        const quint64 tabId = m_tabs->tabIdAt(index);
        if (m_previewCapturesInFlight.contains(tabId)) {
            m_pendingPreviewTabIds.insert(tabId);
            return;
        }
        constexpr qint64 captureIntervalMs = 2000;
        const qint64 now = m_previewCaptureClock.elapsed();
        const qint64 elapsed = now - m_previewCaptureTimes.value(tabId, -captureIntervalMs);
        if (elapsed < captureIntervalMs) {
            m_pendingPreviewTabIds.insert(tabId);
            const int remaining = static_cast<int>(captureIntervalMs - elapsed);
            if (!m_previewCaptureTimer.isActive() || m_previewCaptureTimer.remainingTime() > remaining) {
                m_previewCaptureTimer.start(remaining);
            }
            return;
        }
        engine::EngineView *view = m_tabs->engineViewAt(index);
        const bool internal = !m_tabs->data(m_tabs->index(index), TabModel::InternalPageRole).toString().isEmpty();
        if (!internal && (!view || !(view->capabilities() & engine::EngineView::ThumbnailCapture))) {
            m_pendingPreviewTabIds.remove(tabId);
            return;
        }
        m_previewCaptureTimes.insert(tabId, now);
        m_previewCapturesInFlight.insert(tabId);
        m_pendingPreviewTabIds.remove(tabId);
        const QUrl url = m_tabs->data(m_tabs->index(index), TabModel::UrlRole).toUrl();
        const QPointer<engine::EngineView> capturedView(view);
        QPointer<WindowController> guard(this);
        const auto started = std::chrono::steady_clock::now();
        auto completed = [guard, tabId, url, capturedView, started](const QImage &image) {
            if (!guard || !guard->m_tabs || !guard->m_thumbnailCache) {
                return;
            }
            guard->m_previewCapturesInFlight.remove(tabId);
            const int index = guard->m_tabs->indexForTabId(tabId);
            if (index < 0) {
                return;
            }
            if (!image.isNull() && guard->m_tabs->engineViewAt(index) == capturedView &&
                guard->m_tabs->data(guard->m_tabs->index(index), TabModel::UrlRole).toUrl() == url) {
                guard->m_thumbnailCache->putMetadata(tabId, guard->tabPreviewMetadata(index, false));
                if (guard->m_thumbnailCache->putImage(tabId, image)) {
                    ++guard->m_tabPreviewRevision;
                    emit guard->tabPreviewRevisionChanged();
                }
            }
            if (qEnvironmentVariableIsSet("EDEN_PERF")) {
                const double milliseconds =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                qInfo("EDEN_PERF preview.capture_ms=%.3f", milliseconds);
            }
            if (guard->m_pendingPreviewTabIds.contains(tabId)) {
                guard->captureTabPreview(index);
            }
        };
        if (internal) {
            captureInternalPagePreview(index, std::move(completed));
        } else {
            view->requestThumbnail(QSize(420, 236), std::move(completed));
        }
    }

    void WindowController::captureInternalPagePreview(int index, engine::EngineView::ThumbnailCallback callback) {
        if (!m_window || !m_tabs || index < 0 || index >= m_tabs->rowCount()) {
            callback({});
            return;
        }
        QList<QQuickItem *> delegates;
        std::function<void(QQuickItem *)> collect = [&collect, &delegates](QQuickItem *item) {
            if (!item) {
                return;
            }
            if (item->objectName() == "tabViewport") {
                delegates.append(item);
            }
            const QList<QQuickItem *> children = item->childItems();
            for (QQuickItem *child : children) {
                collect(child);
            }
        };
        collect(m_window->contentItem());
        for (QQuickItem *delegate : delegates) {
            if (delegate->property("index").toInt() != index || !delegate->isVisible() || delegate->width() <= 0 ||
                delegate->height() <= 0) {
                continue;
            }
            QSharedPointer<QQuickItemGrabResult> grab = delegate->grabToImage(QSize(420, 236));
            if (!grab) {
                break;
            }
            connect(grab.get(), &QQuickItemGrabResult::ready, this, [grab, callback = std::move(callback)]() mutable {
                callback(grab->image());
                grab.clear();
            });
            return;
        }
        callback({});
    }

    int WindowController::newTabAndFocusOmnibox() {
        const int index = newTab();
        if (index >= 0) {
            QMetaObject::invokeMethod(this, [this] { emit focusOmniboxRequested(); }, Qt::QueuedConnection);
        }
        return index;
    }

    void WindowController::closeTab(int index) {
        if (m_tabs) {
            m_tabs->closeTab(index);
        }
    }

    void WindowController::navigate(const QUrl &url) {
        if (!m_tabs) {
            return;
        }
        QUrl destination = url;
        if (destination.scheme() == "eden") {
            if (!TabModel::internalPageForUrl(destination).isEmpty()) {
                if (m_activeIndex >= 0 && m_tabs->setInternalPage(m_activeIndex, destination)) {
                    if (!m_privateWindow && history()) {
                        history()->recordVisit(
                            destination,
                            m_tabs->data(m_tabs->index(m_activeIndex), TabModel::TitleRole).toString()
                        );
                    }
                    emit currentEngineChanged();
                    emit currentBookmarkedChanged();
                    scheduleSessionSave();
                } else {
                    newTab(destination);
                }
                return;
            }
            if (engine::EngineView *view = m_tabs->engineViewAt(m_activeIndex)) {
                const QUrl mapped = view->internalUrlFor(destination);
                if (!mapped.isEmpty()) {
                    destination = mapped;
                }
            }
        }
        if (engine::EngineView *view = m_tabs->engineViewAt(m_activeIndex)) {
            view->load(destination);
        } else if (m_activeIndex >= 0) {
            const int internalIndex = m_activeIndex;
            newTab(destination);
            m_tabs->closeTab(internalIndex);
        }
    }

    void WindowController::navigateText(const QString &text, bool controlEnter) {
        if (m_omnibox) {
            const QUrl url = m_omnibox->destination(text, controlEnter);
            if (url.isEmpty() || !url.isValid()) {
                return;
            }
            navigate(url);
            m_omnibox->setQuery({});
        }
    }

    void WindowController::activateSuggestion(int row) {
        if (m_omnibox) {
            const QUrl url = m_omnibox->suggestionUrl(row);
            if (url.isEmpty() || !url.isValid()) {
                return;
            }
            const int tab = m_omnibox->suggestionTabIndex(row);
            if (tab >= 0) {
                setActiveIndex(tab);
            } else {
                navigate(url);
            }
            m_omnibox->setQuery({});
        }
    }

    void WindowController::beginTabDrag(int index, QQuickItem *visual, qreal pressX, qreal pressY) {
        if (!m_tabs || !m_window || !visual || index < 0 || index >= m_tabs->rowCount() || tabDragSession().active) {
            return;
        }
        QQuickItem *area = visual;
        while (area && area->objectName() != "tabDropArea") {
            area = area->parentItem();
        }
        if (!area) {
            return;
        }
        setActiveIndex(index);
        TabDragSession &session = tabDragSession();
        session = {};
        session.grabOwner = this;
        session.current = this;
        session.origin = this;
        session.currentIndex = index;
        session.originIndex = index;
        session.pinned = m_tabs->data(m_tabs->index(index), TabModel::PinnedRole).toBool();
        loadDropMetrics(session, area);
        session.pressOffset = session.vertical ? pressY : pressX;
        const qreal draggedExtent = session.pinned ? session.pinnedExtent : session.normalExtent;
        session.pressOffset = std::clamp(session.pressOffset, 0.0, std::max(0.0, draggedExtent));
        session.cursorInArea = area->mapFromItem(visual, QPointF(pressX, pressY));
        session.active = true;
        QCoreApplication::instance()->installEventFilter(this);
        m_tabDragGuard.start();
        refreshTabDragVisuals();
    }

    bool WindowController::eventFilter(QObject *watched, QEvent *event) {
        if (watched == m_window && event->type() == QEvent::WindowActivate) {
            if (WindowRegistry *registry = WindowRegistry::instance()) {
                registry->touchActivation(this);
            }
        }
        if (watched == m_window && event->type() == QEvent::MouseButtonPress) {
            engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr;
            const QPointF position = static_cast<QMouseEvent *>(event)->position();
            if (view && !view->containsPageScenePoint(position)) {
                view->releaseFocus();
            }
        }
        TabDragSession &session = tabDragSession();
        if (!session.active || session.dnd || session.grabOwner != this) {
            return QObject::eventFilter(watched, event);
        }
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            endTabDrag(true);
            return true;
        }
        if (!watched->isWindowType() || watched != m_window) {
            return QObject::eventFilter(watched, event);
        }
        if (event->type() == QEvent::MouseMove) {
            updateTabDrag(static_cast<QMouseEvent *>(event)->position());
        } else if (
            event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton
        ) {
            updateTabDrag(static_cast<QMouseEvent *>(event)->position());
            endTabDrag(false);
        }
        return QObject::eventFilter(watched, event);
    }

    void WindowController::updateTabDrag(const QPointF &windowPosition) {
        TabDragSession &session = tabDragSession();
        WindowController *current = session.current;
        if (!current || !current->m_tabs || !session.area || !session.view) {
            endTabDrag(true);
            return;
        }
        session.pointerInWindow = windowPosition;
        const QPointF local = session.area->mapFromScene(windowPosition);
        const qreal perpendicular = session.vertical ? local.x() : local.y();
        const qreal perpendicularExtent = session.vertical ? session.area->width() : session.area->height();
        const qreal along = session.vertical ? local.y() : local.x();
        const qreal alongExtent = session.vertical ? session.area->height() : session.area->width();
        if (perpendicular < -kTabTearOffMargin || perpendicular > perpendicularExtent + kTabTearOffMargin ||
            along < -kTabTearOffMargin || along > alongExtent + kTabTearOffMargin) {
            beginTornDrag();
            return;
        }
        session.cursorInArea = local;
        reorderDraggedTab();
        refreshTabDragVisuals();
    }

    void WindowController::reorderDraggedTab() {
        TabDragSession &session = tabDragSession();
        WindowController *current = session.current;
        QQuickItem *view = session.view;
        if (!current || !current->m_tabs || !session.area || !view) {
            return;
        }
        const QPointF local = session.area->mapToItem(view, session.cursorInArea);
        const qreal along = session.vertical ? local.y() : local.x();
        const qreal viewExtent = session.vertical ? view->height() : view->width();
        const char *contentProperty = session.vertical ? "contentY" : "contentX";
        const char *contentExtentProperty = session.vertical ? "contentHeight" : "contentWidth";
        const qreal maximum = std::max<qreal>(0, view->property(contentExtentProperty).toReal() - viewExtent);
        qreal contentPosition = view->property(contentProperty).toReal();
        if (along < 40 && contentPosition > 0) {
            view->setProperty(contentProperty, std::max<qreal>(0, contentPosition - 18));
        } else if (along > viewExtent - 40 && contentPosition < maximum) {
            view->setProperty(contentProperty, std::min(maximum, contentPosition + 18));
        }
        const int count = current->m_tabs->rowCount();
        const int pinnedCount = current->m_tabs->pinnedCount();
        const int slot = dragSlotIndex(session, dragLeadingPosition(session), pinnedCount, count, false);
        if (slot != session.currentIndex && current->m_tabs->moveTab(session.currentIndex, slot)) {
            session.currentIndex = slot;
        }
    }

    void WindowController::beginTornDrag() {
        TabDragSession &session = tabDragSession();
        WindowController *source = session.current;
        QQuickWindow *sourceWindow = source->m_window;
        if (!sourceWindow) {
            endTabDrag(true);
            return;
        }
        const QPointF pointerInWindow = session.pointerInWindow;
        const bool waylandPlatform = QGuiApplication::platformName().startsWith("wayland");
        QQuickItem *sourceArea = session.area;
        QPointF anchored = pointerInWindow;
        QPointF attachPoint = pointerInWindow;
        if (sourceArea) {
            anchored = sourceArea->mapFromScene(pointerInWindow);
            if (session.vertical) {
                anchored.setX(sourceArea->width() / 2);
                anchored.setY(std::clamp(anchored.y(), 0.0, sourceArea->height()));
            } else {
                anchored.setY(sourceArea->height() / 2);
                anchored.setX(std::clamp(anchored.x(), 0.0, sourceArea->width()));
            }
            attachPoint = sourceArea->mapToScene(anchored);
        }
        WindowController *torn = source;
        if (source->m_tabs->rowCount() > 1) {
            torn = ProfileManager::instance()->createBrowserWindow(
                source->m_context,
                source->m_privateWindow,
                source->m_engineName,
                false,
                false,
                false
            );
            if (!torn || !torn->m_window) {
                return;
            }
            torn->m_window->resize(sourceWindow->size());
            if (!waylandPlatform) {
                torn->m_window->setPosition(sourceWindow->position() + (pointerInWindow - attachPoint).toPoint());
            }
            if (!source->m_tabs->transferTabTo(session.currentIndex, torn->m_tabs.get(), 0)) {
                torn->m_window->close();
                return;
            }
        }
        session.tornWindow = torn;
        session.tornCreated = torn != source;
        session.current = torn;
        session.currentIndex = 0;
        session.torn = true;
        session.dnd = true;
        session.hovering = false;
        loadDropMetrics(session, torn->visibleDropArea());
        session.cursorInArea = anchored;
        QCoreApplication::instance()->removeEventFilter(this);
        m_tabDragGuard.stop();
        refreshTabDragVisuals();

        QMimeData *mimeData = new QMimeData();
        mimeData->setData("application/x-eden-tab", QByteArrayLiteral("move"));
        QByteArray windowData;
        QDataStream windowStream(&windowData, QIODevice::WriteOnly);
        windowStream << reinterpret_cast<qintptr>(static_cast<QWindow *>(torn->m_window.data()));
        mimeData->setData("application/x-qt-mainwindowdrag-window", windowData);
        QByteArray offsetData;
        QDataStream offsetStream(&offsetData, QIODevice::WriteOnly);
        offsetStream << attachPoint.toPoint();
        mimeData->setData("application/x-qt-mainwindowdrag-position", offsetData);

        QDrag *drag = new QDrag(this);
        drag->setMimeData(mimeData);
        QPixmap pixmap(1, 1);
        pixmap.fill(Qt::transparent);
        drag->setPixmap(pixmap);
        const auto settleTornMetrics = [] {
            TabDragSession &settled = tabDragSession();
            if (settled.active && settled.torn && settled.area) {
                loadDropMetrics(settled, settled.area);
            }
            refreshTabDragVisuals();
        };
        if (QQuickItem *tornArea = session.area) {
            connect(tornArea, &QQuickItem::widthChanged, drag, settleTornMetrics);
            connect(tornArea, &QQuickItem::heightChanged, drag, settleTornMetrics);
        }
        if (QQuickItem *tornView = session.view) {
            connect(tornView, &QQuickItem::widthChanged, drag, settleTornMetrics);
            connect(tornView, &QQuickItem::heightChanged, drag, settleTornMetrics);
        }
        QTimer::singleShot(0, drag, settleTornMetrics);
        if (!waylandPlatform) {
            QTimer *mover = new QTimer(drag);
            const QPoint grabOffset = attachPoint.toPoint();
            QPointer<WindowController> tornGuard(torn);
            connect(mover, &QTimer::timeout, drag, [tornGuard, grabOffset] {
                if (tornGuard && tornGuard->m_window && tornGuard->m_window->isVisible()) {
                    tornGuard->m_window->setPosition(QCursor::pos() - grabOffset);
                }
            });
            mover->start(16);
        }
        const Qt::DropAction result = drag->exec(Qt::MoveAction);
        finalizeTornDrag(waylandPlatform && result == Qt::IgnoreAction);
        drag->deleteLater();
    }

    void WindowController::tabDragEntered(QQuickItem *area, qreal x, qreal y) {
        handleStripDrag(area, x, y);
    }

    void WindowController::tabDragMoved(QQuickItem *area, qreal x, qreal y) {
        handleStripDrag(area, x, y);
    }

    void WindowController::tabDragLeft() {
        TabDragSession &session = tabDragSession();
        if (session.active && session.dnd && session.current == this) {
            session.hovering = false;
            refreshTabDragVisuals();
        }
    }

    bool WindowController::tabDragDropped(QQuickItem *area, qreal x, qreal y) {
        TabDragSession &session = tabDragSession();
        if (!session.active || !session.dnd) {
            return false;
        }
        handleStripDrag(area, x, y);
        return session.current == this;
    }

    void WindowController::handleStripDrag(QQuickItem *area, qreal x, qreal y) {
        TabDragSession &session = tabDragSession();
        if (!session.active || !session.dnd || !m_tabs || !area) {
            return;
        }
        WindowController *current = session.current;
        if (!current || !current->m_tabs) {
            return;
        }
        if (current != this && (current->m_context != m_context || current->m_privateWindow != m_privateWindow)) {
            return;
        }
        if (current == this) {
            session.cursorInArea = QPointF(x, y);
            session.hovering = true;
            reorderDraggedTab();
            refreshTabDragVisuals();
            return;
        }
        QQuickItem *previousArea = session.area;
        QQuickItem *previousView = session.view;
        const bool previousVertical = session.vertical;
        const qreal previousPressOffset = session.pressOffset;
        loadDropMetrics(session, area);
        if (!session.view) {
            session.area = previousArea;
            session.view = previousView;
            session.vertical = previousVertical;
            session.pressOffset = previousPressOffset;
            return;
        }
        session.cursorInArea = QPointF(x, y);
        const int slot =
            dragSlotIndex(session, dragLeadingPosition(session), m_tabs->pinnedCount(), m_tabs->rowCount(), true);
        if (!current->m_tabs->transferTabTo(session.currentIndex, m_tabs.get(), slot)) {
            session.area = previousArea;
            session.view = previousView;
            session.vertical = previousVertical;
            session.pressOffset = previousPressOffset;
            return;
        }
        session.current = this;
        session.currentIndex = slot;
        session.torn = false;
        session.hovering = true;
        if (m_window) {
            m_window->raise();
            m_window->requestActivate();
        }
        refreshTabDragVisuals();
    }

    void WindowController::finalizeTornDrag(bool canceled) {
        TabDragSession &session = tabDragSession();
        if (!session.active) {
            return;
        }
        session.active = false;
        WindowController *current = session.current;
        WindowController *origin = session.origin;
        WindowController *torn = session.tornWindow;
        const bool endedTorn = session.torn;
        const int currentIndex = session.currentIndex;
        const int originIndex = session.originIndex;
        session = {};
        if (canceled && current && current->m_tabs && origin && origin == current && !endedTorn && currentIndex >= 0) {
            current->m_tabs->moveTab(currentIndex, originIndex);
        } else if (
            canceled && current && current->m_tabs && origin && origin != current && origin->m_tabs && currentIndex >= 0
        ) {
            current->m_tabs->transferTabTo(currentIndex, origin->m_tabs.get(), originIndex);
            if (origin->m_window) {
                origin->m_window->raise();
                origin->m_window->requestActivate();
            }
        } else if (current && current->m_window) {
            if (endedTorn && torn == current) {
                current->m_window->requestActivate();
            }
        }
        if (WindowRegistry *registry = WindowRegistry::instance()) {
            const QList<WindowController *> controllers = registry->allControllers();
            for (WindowController *candidate : controllers) {
                if (candidate && candidate->m_closeDeferredForTabDrag) {
                    candidate->m_closeDeferredForTabDrag = false;
                    emit candidate->closeWindowRequested();
                }
            }
        }
        refreshTabDragVisuals();
    }

    void WindowController::endTabDrag(bool canceled) {
        TabDragSession &session = tabDragSession();
        if (!session.active || session.dnd) {
            return;
        }
        session.active = false;
        QCoreApplication::instance()->removeEventFilter(this);
        m_tabDragGuard.stop();
        WindowController *current = session.current;
        WindowController *origin = session.origin;
        if (canceled && current && current->m_tabs && origin == current && session.currentIndex >= 0) {
            current->m_tabs->moveTab(session.currentIndex, session.originIndex);
        }
        session = {};
        refreshTabDragVisuals();
    }

    qreal WindowController::tabDragTranslation(int index, qreal itemPosition) const {
        const TabDragSession &session = tabDragSession();
        if (!session.active || session.current != this || index != session.currentIndex || !session.view ||
            !session.area || !m_tabs) {
            return 0;
        }
        if (session.dnd && !session.torn && !session.hovering) {
            return 0;
        }
        const qreal extent = session.pinned ? session.pinnedExtent : session.normalExtent;
        qreal leading = dragLeadingPosition(session);
        if (session.torn) {
            QQuickItem *area = session.area;
            QQuickItem *view = session.view;
            const qreal freshExtent = area->property(session.pinned ? "dropPinnedExtent" : "dropNormalExtent").toReal();
            const QPointF areaOrigin = area->mapToItem(view, QPointF(0, 0));
            const qreal contentOffset =
                session.vertical ? view->property("contentY").toReal() : view->property("contentX").toReal();
            const qreal areaStart = (session.vertical ? areaOrigin.y() : areaOrigin.x()) + contentOffset;
            const qreal areaExtent = session.vertical ? area->height() : area->width();
            leading = areaExtent > freshExtent ? std::clamp(leading, areaStart, areaStart + areaExtent - freshExtent)
                                               : std::max(leading, areaStart);
        } else {
            const int count = m_tabs->rowCount();
            const int pinnedCount = m_tabs->pinnedCount();
            const qreal pinnedStep = session.pinnedExtent + session.spacing;
            const qreal normalStep = session.normalExtent + session.spacing;
            qreal minimum = session.pinned ? 0 : pinnedCount * pinnedStep;
            qreal maximum = session.pinned
                                ? std::max(0, pinnedCount - 1) * pinnedStep
                                : pinnedCount * pinnedStep + std::max(0, count - 1 - pinnedCount) * normalStep;
            leading = std::clamp(leading, minimum, std::max(minimum, maximum));
        }
        Q_UNUSED(extent)
        return leading - itemPosition;
    }

    int WindowController::tabDragIndex() const {
        const TabDragSession &session = tabDragSession();
        return session.active && session.current == this ? session.currentIndex : -1;
    }

    bool WindowController::tabDragTorn() const {
        const TabDragSession &session = tabDragSession();
        return session.active && session.torn && session.current == this;
    }

    void WindowController::back() {
        if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
            view->back();
        }
    }

    void WindowController::forward() {
        if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
            view->forward();
        }
    }

    QVariantList WindowController::navigationHistory(int direction, int maximumItems) const {
        if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
            QVariantList actions = view->navigationHistory(direction, maximumItems);
            for (QVariant &item : actions) {
                QVariantMap action = item.toMap();
                action.insert("icon", direction < 0 ? QStringLiteral("undo") : QStringLiteral("redo"));
                item = action;
            }
            return actions;
        }
        return {};
    }

    void WindowController::navigateHistory(int offset) {
        if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
            view->goToHistoryOffset(offset);
        }
    }

    void WindowController::reload() {
        if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
            view->reload();
        }
    }

    void WindowController::stop() {
        if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
            view->stop();
        }
    }

    void WindowController::find(const QString &text, bool backward, bool caseSensitive) {
        engine::EngineView::FindFlags flags;
        if (backward) {
            flags |= engine::EngineView::FindBackward;
        }
        if (caseSensitive) {
            flags |= engine::EngineView::FindCaseSensitive;
        }
        if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
            view->findInPage(text, flags);
        }
    }

    void WindowController::toggleBookmark() {
        if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
            if (BookmarkStore *store = bookmarks()) {
                store->toggle(view->url(), view->title());
                emit currentBookmarkedChanged();
            }
        }
    }

    bool WindowController::currentBookmarked() const {
        if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
            const BookmarkStore *store = bookmarks();
            return store && store->contains(view->url());
        }
        return false;
    }

    void WindowController::openNewWindow(bool privateWindow) {
        if (!m_context || m_context->signOutInProgress()) {
            return;
        }
        WindowController *controller =
            ProfileManager::instance()->createBrowserWindow(m_context, privateWindow, m_engineName, false, true);
        if (controller) {
            QMetaObject::invokeMethod(
                controller,
                [controller] { emit controller->focusOmniboxRequested(); },
                Qt::QueuedConnection
            );
        }
    }

    void WindowController::updateInternalPageUrl(const QUrl &url) {
        if (!m_tabs || m_activeIndex < 0 || !m_tabs->setInternalPageUrl(m_activeIndex, url)) {
            return;
        }
        if (!m_privateWindow && history()) {
            history()->recordVisit(url, m_tabs->data(m_tabs->index(m_activeIndex), TabModel::TitleRole).toString());
        }
        emit currentUrlChanged();
        scheduleSessionSave();
    }

    void WindowController::openSettingsTab() {
        openInternalTab(QUrl("eden://settings"));
    }

    void WindowController::openAboutTab() {
        openInternalTab(QUrl("eden://settings/about"));
    }

    void WindowController::openThemeEditorTab() {
        openInternalTab(QUrl("eden://theme-editor"));
    }

    QString WindowController::settingsPath(const QUrl &url) const {
        return url.path();
    }

    QString WindowController::settingsQueryValue(const QUrl &url, const QString &name) const {
        return QUrlQuery(url).queryItemValue(name, QUrl::FullyDecoded);
    }

    QUrl WindowController::siteSettingsUrl(const QUrl &origin) const {
        QUrl url("eden://settings/privacy/site-settings/site");
        QUrlQuery query;
        query.addQueryItem("origin", origin.toString());
        url.setQuery(query);
        return url;
    }

    QUrl WindowController::passwordSiteUrl(const QString &site) const {
        QUrl url("eden://settings/autofill/passwords/site");
        QUrlQuery query;
        query.addQueryItem("site", site);
        url.setQuery(query);
        return url;
    }

    void WindowController::openInternalTab(const QUrl &url) {
        if (!m_tabs) {
            return;
        }
        const QString page = TabModel::internalPageForUrl(url);
        for (int row = 0; row < m_tabs->rowCount(); ++row) {
            const QUrl rowUrl = m_tabs->data(m_tabs->index(row), TabModel::UrlRole).toUrl();
            if (rowUrl == url || (!page.isEmpty() && TabModel::internalPageForUrl(rowUrl) == page)) {
                setActiveIndex(row);
                if (rowUrl != url && m_tabs->setInternalPageUrl(row, url)) {
                    emit currentUrlChanged();
                    scheduleSessionSave();
                }
                return;
            }
        }
        newTab(url);
    }

    void WindowController::saveSession() {
        if (m_context && m_initialized && !m_privateWindow) {
            m_context->saveSessionNow();
        }
    }

    void WindowController::prepareToClose() {
        if (!m_registeredAsWindow) {
            return;
        }
        dismissPermissionRequest();
        dismissCredentialPrompt();
        WindowRegistry *registry = WindowRegistry::instance();
        const bool signingOut = m_context && m_context->signOutInProgress();
        if (m_context && !m_privateWindow && !signingOut && registry &&
            registry->normalWindowCountForProfile(m_context->idString()) == 1) {
            m_context->saveSessionNow();
        }
        if (registry) {
            registry->unregisterWindow(this);
        }
        if (m_context) {
            m_context->unregisterWindow(this);
        }
        m_registeredAsWindow = false;
        if (m_context && !m_privateWindow && !signingOut && registry &&
            registry->normalWindowCountForProfile(m_context->idString()) > 0) {
            m_context->saveSessionNow();
        }
        if (m_tabs) {
            for (int row = 0; row < m_tabs->rowCount(); ++row) {
                if (engine::EngineView *view = m_tabs->engineViewAt(row)) {
                    view->attach(nullptr);
                }
            }
        }
    }

    void WindowController::closeWindowNow() {
        prepareToClose();
        if (m_window) {
            m_window->removeEventFilter(this);
            m_window->setVisible(false);
            m_window->deleteLater();
        }
    }

    QJsonObject WindowController::sessionWindow() const {
        QJsonObject window;
        window.insert("activeIndex", m_activeIndex);
        window.insert("layout", m_context ? m_context->settings()->tabLayout() : QStringLiteral("horizontal"));
        window.insert("tabs", m_tabs ? m_tabs->sessionTabs() : QJsonArray());
        return window;
    }

    void WindowController::restoreWindow(const QJsonObject &window) {
        if (!m_tabs) {
            return;
        }
        m_tabs->restoreTabs(window.value("tabs").toArray());
        if (m_tabs->rowCount() > 0) {
            setActiveIndex(std::clamp(window.value("activeIndex").toInt(0), 0, m_tabs->rowCount() - 1));
        }
        const QString layout = window.value("layout").toString();
        if (!layout.isEmpty() && m_context) {
            m_context->settings()->setTabLayout(layout);
        }
    }

    void WindowController::scheduleSessionSave() {
        if (!m_privateWindow && m_context && !m_context->signOutInProgress()) {
            m_context->sessions()->requestSave();
        }
    }

    void WindowController::executePageContextMenuCommand(const QString &command) {
        if (command.startsWith("credential:") || command.startsWith("form:")) {
            fillAutofillSuggestion(command);
            dismissPageContextMenu();
            return;
        }
        if (command.startsWith("open_with:")) {
            if (m_tabs) {
                m_tabs->openWithTab(m_pageContextMenuTarget, command.sliced(10), m_activeIndex);
            }
            dismissPageContextMenu();
            return;
        }
        if (command == "open_media_new_tab") {
            newTab(m_pageContextMenuTarget, false, m_tabs ? m_tabs->backendIdAt(m_activeIndex) : QString());
            dismissPageContextMenu();
            return;
        }
        if (m_pageContextMenuEngine) {
            QPointer<engine::EngineView> engine = m_pageContextMenuEngine;
            m_pageContextMenuActions.clear();
            m_pageContextMenuEngine.clear();
            m_pageContextMenuTarget = QUrl();
            m_pageContextMenuSurface.clear();
            emit pageContextMenuChanged();
            if (engine) {
                engine->executeContextMenuCommand(command);
            }
            return;
        }
        dismissPageContextMenu();
    }

    void WindowController::dismissPageContextMenu() {
        if (m_pageContextMenuActions.isEmpty() && !m_pageContextMenuEngine) {
            return;
        }
        if (m_pageContextMenuEngine) {
            m_pageContextMenuEngine->dismissContextMenu();
        }
        m_pageContextMenuActions.clear();
        m_pageContextMenuEngine.clear();
        m_pageContextMenuTarget = QUrl();
        m_pageContextMenuSurface.clear();
        emit pageContextMenuChanged();
    }

    void WindowController::resolveJavaScriptDialog(bool accepted, const QString &text) {
        if (m_javaScriptDialog.isEmpty() && !m_javaScriptDialogEngine) {
            return;
        }
        QPointer<engine::EngineView> engine = m_javaScriptDialogEngine;
        const quint64 id = m_javaScriptDialog.value("id").toULongLong();
        m_javaScriptDialog.clear();
        m_javaScriptDialogEngine.clear();
        emit javaScriptDialogChanged();
        if (engine) {
            engine->resolveJavaScriptDialog(id, accepted, text);
        }
    }

    void WindowController::resolveFileDialog(bool accepted, const QList<QUrl> &files) {
        if (m_fileDialog.isEmpty() && !m_fileDialogEngine) {
            return;
        }
        QPointer<engine::EngineView> engine = m_fileDialogEngine;
        const quint64 id = m_fileDialog.value("id").toULongLong();
        m_fileDialog.clear();
        m_fileDialogEngine.clear();
        emit fileDialogChanged();
        if (engine) {
            engine->resolveFileDialog(id, accepted, files);
        }
    }

    void WindowController::acceptCredentialPrompt() {
        auto *vault = qobject_cast<passwords::CredentialVault *>(credentialVault());
        if (!vault || m_credentialPrompt.isEmpty() || m_pendingCredentialPassword.isEmpty()) {
            dismissCredentialPrompt();
            return;
        }
        vault->saveCredential(
            m_credentialPrompt.value("id").toLongLong(),
            m_credentialPrompt.value("site").toString(),
            credentialDraftUsername(),
            m_pendingCredentialPassword
        );
        dismissCredentialPrompt();
        refreshAutofillSuggestions();
    }

    void WindowController::dismissCredentialPrompt() {
        if (!m_pendingCredentialPassword.isEmpty()) {
            m_pendingCredentialPassword.fill(QChar(0));
            m_pendingCredentialPassword.clear();
        }
        if (!m_credentialPrompt.isEmpty()) {
            m_credentialPrompt.clear();
            emit credentialStateChanged();
        }
    }

    void WindowController::setCredentialDraftUsername(const QString &username) {
        if (m_credentialPrompt.isEmpty() || credentialDraftUsername() == username) {
            return;
        }
        m_credentialPrompt.insert("username", username);
        emit credentialStateChanged();
    }

    void WindowController::setCredentialDraftPassword(const QString &password) {
        if (m_credentialPrompt.isEmpty() || m_pendingCredentialPassword == password) {
            return;
        }
        if (!m_pendingCredentialPassword.isEmpty()) {
            m_pendingCredentialPassword.fill(QChar(0));
        }
        m_pendingCredentialPassword = password;
        emit credentialStateChanged();
    }

    void WindowController::fillSavedCredential(qint64 id) {
        if (id > 0) {
            requestCredentialFill(qobject_cast<engine::EngineView *>(currentEngine()), id);
        }
    }

    void WindowController::fillSavedForm(qint64 id) {
        auto *view = qobject_cast<engine::EngineView *>(currentEngine());
        if (!view || view->isLoading() || id <= 0) {
            return;
        }
        const QUrl expectedOrigin = engine::autofillOrigin(view->url());
        if (expectedOrigin.isEmpty()) {
            return;
        }
        const QPointer<WindowController> guard(this);
        const QPointer<engine::EngineView> targetView(view);
        view->requestAutofillTarget([guard, targetView, expectedOrigin, id](engine::AutofillTarget target) {
            if (!guard || !targetView || targetView != guard->currentEngine() || targetView->isLoading() ||
                !target.isValid() || target.origin != expectedOrigin ||
                engine::autofillOrigin(targetView->url()) != expectedOrigin) {
                return;
            }
            auto *vault = qobject_cast<passwords::CredentialVault *>(guard->credentialVault());
            if (!vault || !vault->available()) {
                return;
            }
            for (const QVariant &item : vault->formProfiles()) {
                const QVariantMap profile = item.toMap();
                if (profile.value("id").toLongLong() == id) {
                    targetView->fillForm(target, profile);
                    return;
                }
            }
        });
    }

    void WindowController::fillAutofillSuggestion(const QString &id) {
        const qsizetype separator = id.indexOf(':');
        if (separator <= 0) {
            return;
        }
        bool valid = false;
        const qint64 valueId = id.sliced(separator + 1).toLongLong(&valid);
        if (!valid || valueId <= 0) {
            return;
        }
        if (id.first(separator) == "credential") {
            fillSavedCredential(valueId);
        } else if (id.first(separator) == "form") {
            fillSavedForm(valueId);
        }
    }

    void WindowController::fillDefaultCredential(engine::EngineView *view) {
        if (!m_privateWindow) {
            requestCredentialFill(view, 0);
        }
    }

    void WindowController::requestCredentialFill(engine::EngineView *view, qint64 credentialId) {
        if (!view || view != qobject_cast<engine::EngineView *>(currentEngine()) || view->isLoading()) {
            return;
        }
        const QUrl expectedOrigin = engine::autofillOrigin(view->url());
        if (expectedOrigin.isEmpty()) {
            return;
        }
        auto *vault = qobject_cast<passwords::CredentialVault *>(credentialVault());
        if (!vault || !vault->available() || vault->credentialsForUrl(expectedOrigin).isEmpty()) {
            return;
        }
        const QPointer<WindowController> guard(this);
        const QPointer<engine::EngineView> targetView(view);
        view->requestAutofillTarget([guard, targetView, expectedOrigin, credentialId](engine::AutofillTarget target) {
            if (!guard || !targetView || targetView != guard->currentEngine() || targetView->isLoading() ||
                !target.isValid() || target.origin != expectedOrigin ||
                engine::autofillOrigin(targetView->url()) != expectedOrigin ||
                (credentialId == 0 && guard->m_privateWindow)) {
                return;
            }
            auto *vault = qobject_cast<passwords::CredentialVault *>(guard->credentialVault());
            if (!vault || !vault->available()) {
                return;
            }
            for (const QVariant &item : vault->credentialsForUrl(target.origin)) {
                const QVariantMap credential = item.toMap();
                const qint64 id = credential.value("id").toLongLong();
                if (credentialId > 0 && id != credentialId) {
                    continue;
                }
                QString password = vault->revealPassword(id);
                if (!password.isEmpty()) {
                    targetView->fillCredential(target, credential.value("username").toString(), password);
                    password.fill(QChar(0));
                }
                return;
            }
        });
    }

    void WindowController::refreshAutofillSuggestions() {
        QVariantList suggestions;
        auto *vault = qobject_cast<passwords::CredentialVault *>(credentialVault());
        if (!m_privateWindow && vault && vault->available()) {
            const QString type = m_autofillField.value("type").toString().toLower();
            const QString fieldIdentity =
                (m_autofillField.value("name").toString() + " " + m_autofillField.value("autocomplete").toString())
                    .toLower();
            const QString fieldValue = m_autofillField.value("value").toString();
            const bool focusedField = !m_autofillField.isEmpty();
            const bool credentialField = !focusedField || type == "password" || type == "email" ||
                                         fieldIdentity.contains("user") || fieldIdentity.contains("email") ||
                                         fieldIdentity.contains("login") || fieldIdentity.contains("identifier") ||
                                         fieldIdentity.contains("password") || fieldIdentity.contains("token") ||
                                         fieldIdentity.contains("secret") || fieldIdentity.contains("access");
            const bool formField =
                focusedField && (type == "email" || type == "tel" || fieldIdentity.contains("name") ||
                                 fieldIdentity.contains("email") || fieldIdentity.contains("tel") ||
                                 fieldIdentity.contains("phone") || fieldIdentity.contains("address") ||
                                 fieldIdentity.contains("city") || fieldIdentity.contains("state") ||
                                 fieldIdentity.contains("province") || fieldIdentity.contains("postal") ||
                                 fieldIdentity.contains("zip") || fieldIdentity.contains("country"));
            if (credentialField) {
                QVariantList credentialSuggestions;
                for (const QVariant &item : vault->credentialsForUrl(currentUrl())) {
                    const QVariantMap credential = item.toMap();
                    const QString username = credential.value("username").toString();
                    const int score = credentialMatchScore(username, fieldValue);
                    if (score < 0) {
                        continue;
                    }
                    const qint64 id = credential.value("id").toLongLong();
                    credentialSuggestions.append(
                        QVariantMap{
                            {"kind", "credential"},
                            {"id", "credential:" + QString::number(id)},
                            {"valueId", id},
                            {"title", username.isEmpty() ? QStringLiteral("Saved password") : username},
                            {"subtitle", currentUrl().host()},
                            {"icon", "password"},
                            {"score", score}
                        }
                    );
                }
                std::stable_sort(
                    credentialSuggestions.begin(),
                    credentialSuggestions.end(),
                    [](const QVariant &left, const QVariant &right) {
                        return left.toMap().value("score").toInt() > right.toMap().value("score").toInt();
                    }
                );
                suggestions.append(credentialSuggestions);
            }
            if (formField) {
                for (const QVariant &item : vault->formProfiles()) {
                    const QVariantMap profile = item.toMap();
                    const qint64 id = profile.value("id").toLongLong();
                    suggestions.append(
                        QVariantMap{
                            {"kind", "form"},
                            {"id", "form:" + QString::number(id)},
                            {"valueId", id},
                            {"title", profile.value("label")},
                            {"subtitle", profile.value("name")},
                            {"icon", "user-circle"}
                        }
                    );
                }
            }
        }
        if (m_autofillSuggestions != suggestions) {
            m_autofillSuggestions = std::move(suggestions);
            emit credentialStateChanged();
        }
    }

    void WindowController::copyCredentialUsername(qint64 id) {
        auto *vault = qobject_cast<passwords::CredentialVault *>(credentialVault());
        QClipboard *clipboard = QGuiApplication::clipboard();
        if (!vault || !clipboard || id <= 0) {
            return;
        }
        const QString username = vault->usernameForCredential(id);
        if (username.isEmpty()) {
            return;
        }
        clipboard->setText(username);
        emit transientMessageRequested(QStringLiteral("Username copied"));
    }

    void WindowController::copyCredentialPassword(qint64 id) {
        auto *vault = qobject_cast<passwords::CredentialVault *>(credentialVault());
        QClipboard *clipboard = QGuiApplication::clipboard();
        if (!vault || !clipboard || id <= 0) {
            return;
        }
        QString password = vault->revealPassword(id);
        if (password.isEmpty()) {
            return;
        }
        clipboard->setText(password);
        password.fill(QChar(0));
        emit transientMessageRequested(QStringLiteral("Password copied"));
    }

    void WindowController::resolvePermissionRequest(bool allowed) {
        finishPermissionRequest(allowed, true);
    }

    void WindowController::dismissPermissionRequest() {
        finishPermissionRequest(false, false);
    }

    void WindowController::finishPermissionRequest(bool allowed, bool persistDecision) {
        if (m_permissionRequest.isEmpty() && !m_permissionRequestEngine) {
            return;
        }
        QPointer<engine::EngineView> engine = m_permissionRequestEngine;
        const quint64 id = m_permissionRequest.value("id").toULongLong();
        const QVariantMap request = m_permissionRequest;
        m_permissionRequest.clear();
        m_permissionRequestEngine.clear();
        if (persistDecision && !m_privateWindow && m_context && m_context->permissions()) {
            const QUrl origin = request.value("origin").toUrl();
            const QStringList permissions = request.value("permissions").toStringList();
            for (const QString &permission : permissions) {
                m_context->permissions()->setPermission(origin, permission, allowed);
            }
        }
        emit permissionRequestChanged();
        if (engine) {
            if (persistDecision) {
                engine->resolvePermissionRequest(id, allowed);
            } else {
                engine->dismissPermissionRequest(id);
            }
        }
    }

    void WindowController::resolveDisplayCaptureRequest(const QString &source) {
        if (m_displayCaptureRequest.isEmpty() && !m_displayCaptureRequestEngine) {
            return;
        }
        const bool accepted = source == "window" || source == "screen";
        QPointer<engine::EngineView> engine = m_displayCaptureRequestEngine;
        const quint64 id = m_displayCaptureRequest.value("id").toULongLong();
        m_displayCaptureRequest.clear();
        m_displayCaptureRequestEngine.clear();
        emit displayCaptureRequestChanged();
        if (engine) {
            engine->resolveDisplayCaptureRequest(id, accepted ? source : QString());
        }
    }

    void WindowController::setOpenPane(const QString &pane) {
        if (pane != "" && pane != "history" && pane != "bookmarks" && pane != "downloads") {
            return;
        }
        if (m_openPane == pane) {
            return;
        }
        m_openPane = pane;
        emit openPaneChanged();
    }

    void WindowController::beginPaneResize() {
        m_paneResizeStartWidth = m_paneWidth;
    }

    void WindowController::resizePane(qreal horizontalDelta) {
        const int nextWidth = std::clamp(m_paneResizeStartWidth - qRound(horizontalDelta), 260, 520);
        if (m_paneWidth == nextWidth) {
            return;
        }
        m_paneWidth = nextWidth;
        emit paneWidthChanged();
    }

    int WindowController::devToolsPaneWidth() const {
        return m_devToolsPaneWidth;
    }

    int WindowController::devToolsPaneHeight() const {
        return m_devToolsPaneHeight;
    }

    void WindowController::beginDevToolsPaneResize() {
        m_devToolsResizeStartWidth = m_devToolsPaneWidth;
        m_devToolsResizeStartHeight = m_devToolsPaneHeight;
    }

    void WindowController::resizeDevToolsPane(qreal delta, bool horizontal) {
        if (horizontal) {
            const int nextWidth = std::clamp(m_devToolsResizeStartWidth - qRound(delta), 280, 1600);
            if (m_devToolsPaneWidth == nextWidth) {
                return;
            }
            m_devToolsPaneWidth = nextWidth;
        } else {
            const int nextHeight = std::clamp(m_devToolsResizeStartHeight - qRound(delta), 180, 1200);
            if (m_devToolsPaneHeight == nextHeight) {
                return;
            }
            m_devToolsPaneHeight = nextHeight;
        }
        emit devToolsPaneSizeChanged();
    }

    void WindowController::commitDevToolsPaneSize() {
        SettingsStore::instance()->setDevToolsPaneSize(m_devToolsPaneWidth, m_devToolsPaneHeight);
    }

    void WindowController::setSidebarExpanded(bool expanded) {
        if (m_sidebarExpanded == expanded) {
            return;
        }
        m_sidebarExpanded = expanded;
        emit sidebarExpandedChanged();
    }

    void WindowController::setFindVisible(bool visible) {
        if (m_findVisible == visible) {
            return;
        }
        m_findVisible = visible;
        emit findVisibleChanged();
    }

    void WindowController::setCommandPaletteVisible(bool visible) {
        if (m_commandPaletteVisible == visible) {
            return;
        }
        m_commandPaletteVisible = visible;
        emit commandPaletteVisibleChanged();
    }

    void WindowController::connectPrivateProfile(engine::EngineProfile *profile) {
        if (!profile || !m_privateDownloads) {
            return;
        }
        connect(
            profile,
            &engine::EngineProfile::downloadStarted,
            m_privateDownloads.get(),
            &DownloadManager::beginDownload,
            Qt::UniqueConnection
        );
        connect(
            profile,
            &engine::EngineProfile::downloadUpdated,
            m_privateDownloads.get(),
            &DownloadManager::updateDownload,
            Qt::UniqueConnection
        );
    }

    void WindowController::connectEngine(engine::EngineView *view) {
        if (!view) {
            return;
        }
        view->setDevToolsPlacement(
            SettingsStore::instance()->devToolsPlacement() == "bottom" ? engine::EngineView::DevToolsBottom
                                                                       : engine::EngineView::DevToolsRight
        );
        connect(view, &engine::EngineView::devToolsPlacementChanged, this, [view] {
            if (view->devToolsPlacement() == engine::EngineView::DevToolsRight) {
                SettingsStore::instance()->setDevToolsPlacement("right");
            } else if (view->devToolsPlacement() == engine::EngineView::DevToolsBottom) {
                SettingsStore::instance()->setDevToolsPlacement("bottom");
            }
        });
        connect(view, &engine::EngineView::loadingChanged, this, [this, view] {
            if (m_fullscreenEngine == view && view->isLoading()) {
                exitContentFullscreen();
            }
            if (!m_privateWindow && !view->isLoading() && history()) {
                history()->recordVisit(view->url(), view->title());
            }
            if (!view->isLoading() && view == qobject_cast<engine::EngineView *>(currentEngine())) {
                QTimer::singleShot(120, this, [this, view] { fillDefaultCredential(view); });
                QTimer::singleShot(0, this, [this, view] {
                    if (view == qobject_cast<engine::EngineView *>(currentEngine())) {
                        captureTabPreview(m_activeIndex);
                    }
                });
            }
        });
        connect(view, &engine::EngineView::urlChanged, this, [this, view] {
            if (m_fullscreenEngine == view) {
                exitContentFullscreen();
            }
            const auto pending = m_pendingCredentialUsernames.constFind(view);
            if (pending != m_pendingCredentialUsernames.cend()) {
                QUrl origin = view->url();
                origin.setPath({});
                origin.setQuery(QString());
                origin.setFragment({});
                if (origin.isValid() && !origin.host().isEmpty() && origin != pending->origin) {
                    m_pendingCredentialUsernames.remove(view);
                }
            }
            if (view == qobject_cast<engine::EngineView *>(currentEngine())) {
                m_autofillField.clear();
                emit currentUrlChanged();
                emit currentBookmarkedChanged();
                refreshAutofillSuggestions();
                if (!view->isLoading()) {
                    QTimer::singleShot(120, this, [this, view] { fillDefaultCredential(view); });
                }
            }
        });
        connect(
            view,
            &engine::EngineView::credentialSubmitted,
            this,
            [this, view](const engine::CredentialSubmissionInfo &info) {
                if (m_privateWindow || view != qobject_cast<engine::EngineView *>(currentEngine()) ||
                    !credentialVault()) {
                    return;
                }
                auto *vault = qobject_cast<passwords::CredentialVault *>(credentialVault());
                if (!vault || !vault->available()) {
                    return;
                }
                const QString reportedUsername = info.username.trimmed();
                if (info.password.isEmpty()) {
                    if (!reportedUsername.isEmpty()) {
                        m_pendingCredentialUsernames.insert(view, {info.origin, reportedUsername});
                    }
                    return;
                }
                QString username = reportedUsername;
                const auto pending = m_pendingCredentialUsernames.constFind(view);
                if (username.isEmpty() && pending != m_pendingCredentialUsernames.cend() &&
                    pending->origin == info.origin) {
                    username = pending->username;
                }
                m_pendingCredentialUsernames.remove(view);
                qint64 existingId = 0;
                const QVariantList existing = vault->credentialsForUrl(info.origin);
                for (const QVariant &item : existing) {
                    const QVariantMap credential = item.toMap();
                    if (credential.value("username").toString() == username) {
                        existingId = credential.value("id").toLongLong();
                        break;
                    }
                }
                if (existingId > 0) {
                    QString existingPassword = vault->revealPassword(existingId);
                    const bool unchanged = existingPassword == info.password;
                    existingPassword.fill(QChar(0));
                    if (unchanged) {
                        return;
                    }
                }
                if (!m_pendingCredentialPassword.isEmpty()) {
                    m_pendingCredentialPassword.fill(QChar(0));
                }
                m_pendingCredentialPassword = info.password;
                m_credentialPrompt = {
                    {"id", existingId},
                    {"site", info.origin.toString(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment)},
                    {"username", username},
                    {"mode", existingId > 0 ? "update" : "save"}
                };
                emit credentialStateChanged();
                emit credentialPromptRequested();
            }
        );
        connect(view, &engine::EngineView::formFieldFocused, this, [this, view](const QVariantMap &field) {
            if (m_privateWindow || view != qobject_cast<engine::EngineView *>(currentEngine())) {
                return;
            }
            m_autofillField = field;
            emit credentialStateChanged();
            refreshAutofillSuggestions();
            if (!m_autofillSuggestions.isEmpty()) {
                emit autofillRequested();
            }
        });
        connect(view, &engine::EngineView::shortcutRequested, this, [this](const QString &command) {
            m_shortcuts->execute(command);
        });
        connect(view, &engine::EngineView::fullscreenRequested, this, [this, view](bool fullscreen) {
            if (fullscreen && view == qobject_cast<engine::EngineView *>(currentEngine()) && m_window) {
                m_fullscreenEngine = view;
                const QUrl origin = engine::autofillOrigin(view->url());
                m_fullscreenOrigin =
                    origin.isEmpty() ? QStringLiteral("Local or embedded content") : origin.toDisplayString();
                setContentFullscreen(true);
            } else if (!fullscreen && m_fullscreenEngine == view) {
                setContentFullscreen(false);
            } else if (fullscreen) {
                view->exitFullscreen();
            }
        });
        connect(
            view,
            &engine::EngineView::contextMenuRequested,
            this,
            [this, view](const engine::ContextMenuInfo &info) {
                if (view != qobject_cast<engine::EngineView *>(currentEngine())) {
                    view->dismissContextMenu();
                    return;
                }
                QVariantList actions = info.actions;
                const bool providedActions = !actions.isEmpty();
                const auto append = [&actions](
                                        const QString &id,
                                        const QString &title,
                                        const QString &icon = {},
                                        bool enabled = true,
                                        const QVariantList &children = QVariantList()
                                    ) {
                    QVariantMap action;
                    action.insert("id", id);
                    action.insert("title", title);
                    action.insert("icon", icon);
                    action.insert("enabled", enabled);
                    if (!children.isEmpty()) {
                        action.insert("children", children);
                    }
                    actions.append(action);
                };
                if (!providedActions && !info.linkUrl.isEmpty()) {
                    append("open_link_new_tab", "Open link in new tab", "new-window");
                    append("copy_link", "Copy link address", "copy");
                    append("download_link", "Save link as", "download");
                }
                if (!providedActions && !info.mediaUrl.isEmpty() && info.linkUrl.isEmpty()) {
                    append("open_media_new_tab", "Open image in new tab", "new-window");
                }
                const QUrl contextTarget = !info.linkUrl.isEmpty() ? info.linkUrl : info.mediaUrl;
                if (!providedActions && !contextTarget.isEmpty()) {
                    QVariantList engines;
                    for (const engine::EngineDescriptor &descriptor :
                         engine::EngineRegistry::instance()->descriptors()) {
                        engines.append(
                            QVariantMap{
                                {"id", "open_with:" + descriptor.id},
                                {"title", descriptor.displayName},
                                {"icon", "globe"}
                            }
                        );
                    }
                    append("open_with", "Open with", "globe", true, engines);
                }
                if (!providedActions && info.editable) {
                    refreshAutofillSuggestions();
                    if (!m_autofillSuggestions.isEmpty()) {
                        append("autofill", "Auto-fill", "password", true, m_autofillSuggestions);
                    }
                    append("cut", "Cut", "cut");
                    append("copy", "Copy", "copy");
                    append("paste", "Paste", "clipboard");
                    append("select_all", "Select all", "text-selection");
                } else if (!providedActions && !info.selectedText.isEmpty()) {
                    append("copy", "Copy", "copy");
                }
                if (!providedActions) {
                    append("back", "Back", "arrow-left", view->canGoBack());
                    append("forward", "Forward", "arrow-right", view->canGoForward());
                    append("reload", "Reload", "refresh");
                    append("view_source", "View page source", "code");
                    if (view->capabilities() & engine::EngineView::DockedDevtools) {
                        append("inspect", "Inspect", "code");
                    }
                }
                m_pageContextMenuActions = std::move(actions);
                m_pageContextMenuPosition = info.position;
                m_pageContextMenuSurface = info.surface;
                m_pageContextMenuEngine = view;
                m_pageContextMenuTarget = contextTarget;
                emit pageContextMenuChanged();
                emit pageContextMenuRequested();
            }
        );
        connect(
            view,
            &engine::EngineView::javaScriptDialogRequested,
            this,
            [this, view](const engine::JavaScriptDialogInfo &info) {
                if (view != qobject_cast<engine::EngineView *>(currentEngine())) {
                    view->resolveJavaScriptDialog(info.id, false, {});
                    return;
                }
                resolveJavaScriptDialog(false);
                m_javaScriptDialog = {
                    {"id", QVariant::fromValue(info.id)},
                    {"origin", info.origin},
                    {"originLabel", info.origin.host()},
                    {"kind", info.kind},
                    {"message", info.message},
                    {"defaultText", info.defaultText}
                };
                m_javaScriptDialogEngine = view;
                emit javaScriptDialogChanged();
                emit javaScriptDialogRequested();
            }
        );
        connect(view, &engine::EngineView::javaScriptDialogClosed, this, [this, view](quint64 id) {
            if (m_javaScriptDialogEngine != view || m_javaScriptDialog.value("id").toULongLong() != id) {
                return;
            }
            m_javaScriptDialog.clear();
            m_javaScriptDialogEngine.clear();
            emit javaScriptDialogChanged();
        });
        connect(
            view,
            &engine::EngineView::permissionRequested,
            this,
            [this, view](const engine::PermissionRequestInfo &info) {
                if (view != qobject_cast<engine::EngineView *>(currentEngine())) {
                    view->dismissPermissionRequest(info.id);
                    return;
                }
                dismissPermissionRequest();
                PermissionStore *store = !m_privateWindow && m_context ? m_context->permissions() : nullptr;
                bool allGranted = store && !info.permissions.isEmpty();
                bool anyDenied = false;
                if (store) {
                    for (const QString &permission : info.permissions) {
                        store->noteRequested(info.origin, permission);
                        const PermissionStore::Verdict verdict = store->verdict(info.origin, permission);
                        allGranted = allGranted && verdict == PermissionStore::Granted;
                        anyDenied = anyDenied || verdict == PermissionStore::Denied;
                    }
                }
                if (anyDenied || allGranted) {
                    view->resolvePermissionRequest(info.id, allGranted);
                    return;
                }
                m_permissionRequest = {
                    {"id", QVariant::fromValue(info.id)},
                    {"origin", info.origin},
                    {"originLabel", info.origin.host()},
                    {"permissions", info.permissions}
                };
                m_permissionRequestEngine = view;
                emit permissionRequestChanged();
                emit permissionRequestRequested();
            }
        );
        connect(view, &engine::EngineView::permissionRequestClosed, this, [this, view](quint64 id) {
            if (m_permissionRequestEngine != view || m_permissionRequest.value("id").toULongLong() != id) {
                return;
            }
            m_permissionRequest.clear();
            m_permissionRequestEngine.clear();
            emit permissionRequestChanged();
        });
        connect(
            view,
            &engine::EngineView::displayCaptureRequested,
            this,
            [this, view](const engine::DisplayCaptureRequestInfo &info) {
                if (view != qobject_cast<engine::EngineView *>(currentEngine())) {
                    view->resolveDisplayCaptureRequest(info.id, {});
                    return;
                }
                resolveDisplayCaptureRequest();
                PermissionStore *store = !m_privateWindow && m_context ? m_context->permissions() : nullptr;
                if (store) {
                    store->noteRequested(info.origin, "screen sharing");
                    if (info.audioRequested) {
                        store->noteRequested(info.origin, "screen audio");
                    }
                    if (store->verdict(info.origin, "screen sharing") == PermissionStore::Denied ||
                        (info.audioRequested &&
                         store->verdict(info.origin, "screen audio") == PermissionStore::Denied)) {
                        view->resolveDisplayCaptureRequest(info.id, {});
                        return;
                    }
                }
                m_displayCaptureRequest = {
                    {"id", QVariant::fromValue(info.id)},
                    {"origin", info.origin},
                    {"originLabel", info.origin.host()},
                    {"audioRequested", info.audioRequested}
                };
                m_displayCaptureRequestEngine = view;
                emit displayCaptureRequestChanged();
                emit displayCaptureRequestRequested();
            }
        );
        connect(view, &engine::EngineView::displayCaptureRequestClosed, this, [this, view](quint64 id) {
            if (m_displayCaptureRequestEngine != view || m_displayCaptureRequest.value("id").toULongLong() != id) {
                return;
            }
            m_displayCaptureRequest.clear();
            m_displayCaptureRequestEngine.clear();
            emit displayCaptureRequestChanged();
        });
        connect(view, &engine::EngineView::fileDialogRequested, this, [this, view](const engine::FileDialogInfo &info) {
            if (view != qobject_cast<engine::EngineView *>(currentEngine())) {
                view->resolveFileDialog(info.id, false, {});
                return;
            }
            resolveFileDialog(false);
            m_fileDialog = {
                {"id", QVariant::fromValue(info.id)},
                {"title", info.title},
                {"mode", info.mode},
                {"defaultPath", info.defaultPath},
                {"nameFilters", info.nameFilters}
            };
            m_fileDialogEngine = view;
            emit fileDialogChanged();
            emit fileDialogRequested();
        });
        connect(view, &engine::EngineView::fileDialogClosed, this, [this, view](quint64 id) {
            if (m_fileDialogEngine != view || m_fileDialog.value("id").toULongLong() != id) {
                return;
            }
            m_fileDialog.clear();
            m_fileDialogEngine.clear();
            emit fileDialogChanged();
        });
        connect(view, &QObject::destroyed, this, [this, view] {
            m_pendingCredentialUsernames.remove(view);
            if (!m_pageContextMenuEngine && !m_pageContextMenuActions.isEmpty()) {
                m_pageContextMenuActions.clear();
                emit pageContextMenuChanged();
            }
            if (!m_javaScriptDialogEngine && !m_javaScriptDialog.isEmpty()) {
                m_javaScriptDialog.clear();
                emit javaScriptDialogChanged();
            }
            if (!m_permissionRequestEngine && !m_permissionRequest.isEmpty()) {
                m_permissionRequest.clear();
                emit permissionRequestChanged();
            }
            if (!m_displayCaptureRequestEngine && !m_displayCaptureRequest.isEmpty()) {
                m_displayCaptureRequest.clear();
                emit displayCaptureRequestChanged();
            }
        });
    }

    QQuickItem *WindowController::visibleDropArea() const {
        if (!m_window) {
            return nullptr;
        }
        const QList<QQuickItem *> areas = m_window->findChildren<QQuickItem *>("tabDropArea");
        for (QQuickItem *area : areas) {
            if (area->isVisible() && area->opacity() > 0) {
                return area;
            }
        }
        return nullptr;
    }

    void WindowController::refreshTabDragVisuals() {
        WindowRegistry *registry = WindowRegistry::instance();
        if (!registry) {
            return;
        }
        const QList<WindowController *> controllers = registry->allControllers();
        for (WindowController *controller : controllers) {
            if (!controller) {
                continue;
            }
            ++controller->m_tabDragRevision;
            emit controller->tabDragRevisionChanged();
        }
    }

    void WindowController::executeCommand(const QString &id) {
        if (id == "new_tab") {
            newTabAndFocusOmnibox();
        } else if (id == "close_tab") {
            closeTab(m_activeIndex);
        } else if (id == "restore_tab") {
            if (m_tabs && m_tabs->undoClose()) {
                setActiveIndex(std::max(0, m_tabs->rowCount() - 1));
            }
        } else if (id == "next_tab" && m_tabs && m_tabs->rowCount() > 0) {
            setActiveIndex((m_activeIndex + 1) % m_tabs->rowCount());
        } else if (id == "previous_tab" && m_tabs && m_tabs->rowCount() > 0) {
            setActiveIndex((m_activeIndex + m_tabs->rowCount() - 1) % m_tabs->rowCount());
        } else if (id == "focus_omnibox") {
            emit focusOmniboxRequested();
        } else if (id == "bookmark") {
            toggleBookmark();
        } else if (id == "find") {
            setFindVisible(true);
        } else if (id == "private_window") {
            openNewWindow(true);
        } else if (id == "fullscreen") {
            toggleWindowFullscreen();
        } else if (id == "devtools") {
            if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
                view->openDevTools();
            }
        } else if (id == "command_palette") {
            setCommandPaletteVisible(true);
        } else if (id == "settings") {
            openSettingsTab();
        } else if (id == "history") {
            setOpenPane("history");
        } else if (id == "downloads") {
            setOpenPane("downloads");
        } else if (id == "quit") {
            QCoreApplication::quit();
        }
    }

    QString WindowController::fullscreenOrigin() const {
        return m_fullscreenOrigin;
    }

    void WindowController::exitContentFullscreen() {
        QPointer<engine::EngineView> view = m_fullscreenEngine;
        setContentFullscreen(false);
        if (view) {
            view->exitFullscreen();
        }
    }

    void WindowController::setContentFullscreen(bool fullscreen) {
        if (m_contentFullscreen == fullscreen || !m_window) {
            return;
        }
        m_contentFullscreen = fullscreen;
        if (fullscreen) {
            m_visibilityBeforeContentFullscreen = static_cast<int>(m_window->visibility());
            m_window->showFullScreen();
        } else if (m_visibilityBeforeContentFullscreen == static_cast<int>(QWindow::FullScreen)) {
            m_window->showFullScreen();
        } else if (m_visibilityBeforeContentFullscreen == static_cast<int>(QWindow::Maximized)) {
            m_window->showMaximized();
        } else {
            m_window->showNormal();
        }
        if (!fullscreen) {
            m_fullscreenEngine.clear();
            m_fullscreenOrigin.clear();
        }
        emit contentFullscreenChanged();
    }

    void WindowController::toggleWindowFullscreen() {
        if (!m_window || m_contentFullscreen) {
            return;
        }
        if (m_window->visibility() == QWindow::FullScreen) {
            if (m_visibilityBeforeWindowFullscreen == static_cast<int>(QWindow::Maximized)) {
                m_window->showMaximized();
            } else {
                m_window->showNormal();
            }
            return;
        }
        m_visibilityBeforeWindowFullscreen = static_cast<int>(m_window->visibility());
        m_window->showFullScreen();
    }

}
