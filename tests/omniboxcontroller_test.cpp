#include "core/bookmarks/bookmarkstore.h"
#include "core/history/historystore.h"
#include "core/omni/omniboxcontroller.h"
#include "core/profiles/profiledatabase.h"
#include "core/profiles/profilesettings.h"
#include "core/settings/settingsstore.h"
#include "core/settings/theme/thememanager.h"
#include "core/window/tabmodel.h"
#include "engine/engineview.h"

#include <QAbstractItemModelTester>
#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QInputMethodEvent>
#include <QNetworkReply>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUrlQuery>
#include <QtTest>

class OmniboxEngineView final : public eden::engine::EngineView {
    Q_OBJECT

  public:
    QUrl url() const override {
        return m_url;
    }
    QString title() const override {
        return m_url.host();
    }
    QUrl faviconUrl() const override {
        return {};
    }
    int loadProgress() const override {
        return 100;
    }
    bool isLoading() const override {
        return false;
    }
    bool canGoBack() const override {
        return false;
    }
    bool canGoForward() const override {
        return false;
    }
    bool isAudible() const override {
        return false;
    }
    bool isMuted() const override {
        return false;
    }
    QString securityState() const override {
        return "secure";
    }
    QString backendName() const override {
        return "Fake";
    }
    Capabilities capabilities() const override {
        return {};
    }
    void load(const QUrl &url) override {
        m_url = url;
        emit urlChanged();
        emit titleChanged();
    }
    void back() override {}
    void forward() override {}
    QVariantList navigationHistory(int, int) const override {
        return {};
    }
    void goToHistoryOffset(int) override {}
    void reload() override {}
    void stop() override {}
    void openDevTools() override {}
    void findInPage(const QString &, FindFlags) override {}
    void attach(QQuickItem *) override {}
    void setMuted(bool) override {}

  private:
    QUrl m_url;
};

class SuggestionReply final : public QNetworkReply {
  public:
    explicit SuggestionReply(const QNetworkRequest &request, QObject *parent)
        : QNetworkReply(parent) {
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly);
    }
    void abort() override {
        aborted = true;
    }
    void complete(const QByteArray &payload) {
        m_payload = payload;
        emit readyRead();
        setFinished(true);
        emit finished();
    }
    qint64 bytesAvailable() const override {
        return m_payload.size() + QNetworkReply::bytesAvailable();
    }
    bool aborted = false;

  protected:
    qint64 readData(char *data, qint64 maximum) override {
        const qint64 size = std::min<qint64>(maximum, m_payload.size());
        if (size == 0) {
            return -1;
        }
        memcpy(data, m_payload.constData(), size);
        m_payload.remove(0, size);
        return size;
    }

  private:
    QByteArray m_payload;
};

class SuggestionNetwork final : public QNetworkAccessManager {
  public:
    QList<QPointer<SuggestionReply>> replies;
    QList<QNetworkRequest> requests;

  protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        requests.append(request);
        auto *reply = new SuggestionReply(request, this);
        replies.append(reply);
        return reply;
    }
};

class OmniboxTestWindow final : public QObject {
    Q_OBJECT
    Q_PROPERTY(eden::core::OmniboxController *omnibox MEMBER omnibox CONSTANT)
    Q_PROPERTY(QObject *currentEngine READ currentEngine CONSTANT)
    Q_PROPERTY(QUrl currentUrl MEMBER currentUrl NOTIFY currentUrlChanged)
    Q_PROPERTY(QString displayUrl MEMBER displayUrl NOTIFY currentUrlChanged)
    Q_PROPERTY(bool credentialKeyVisible MEMBER flag CONSTANT)
    Q_PROPERTY(bool currentBookmarked MEMBER flag CONSTANT)
    Q_PROPERTY(QVariantMap credentialPrompt MEMBER credentialPrompt CONSTANT)
    Q_PROPERTY(QVariantList autofillSuggestions MEMBER autofillSuggestions CONSTANT)
    Q_PROPERTY(QString credentialDraftUsername MEMBER empty CONSTANT)
    Q_PROPERTY(QString credentialDraftPassword MEMBER empty CONSTANT)
    Q_PROPERTY(int activeIndex MEMBER activeIndex)

  public:
    explicit OmniboxTestWindow(eden::core::OmniboxController *model)
        : omnibox(model) {}
    QObject *currentEngine() const {
        return nullptr;
    }
    Q_INVOKABLE void navigate(const QUrl &url) {
        navigated = url;
    }
    eden::core::OmniboxController *omnibox;
    QUrl currentUrl{"eden://newtab"};
    QString displayUrl{"eden://newtab"};
    bool flag = false;
    QVariantMap credentialPrompt;
    QVariantList autofillSuggestions;
    QString empty;
    int activeIndex = -1;
    QUrl navigated;

  signals:
    void currentUrlChanged();
    void focusOmniboxRequested();
    void credentialPromptRequested();
};

class OmniboxUiFixture {
  public:
    explicit OmniboxUiFixture(
        QQmlEngine &engine,
        eden::core::ProfileSettings *settings = nullptr,
        SuggestionNetwork *network = nullptr
    )
        : tabs([] { return std::make_unique<OmniboxEngineView>(); }, false),
          controller(&tabs, nullptr, nullptr, settings, nullptr, network),
          shell(&controller),
          component(&engine) {
        tabs.addTab(QUrl("https://github.com/Team/docs?old=search"));
        tabs.addTab(QUrl("https://gitlab.com/projects"));
        component.loadFromModule("Eden.Ui", "Omnibox");
        object.reset(component.createWithInitialProperties({{"controller", QVariant::fromValue(&shell)}}));
        if (!object) {
            return;
        }
        auto *item = qobject_cast<QQuickItem *>(object.get());
        item->setParentItem(window.contentItem());
        item->setWidth(780);
        item->setHeight(40);
        window.resize(800, 600);
        window.show();
        field = object->findChild<QQuickItem *>("omniboxField");
        popup = object->findChild<QObject *>("omniboxSuggestions");
        field->forceActiveFocus();
    }
    ~OmniboxUiFixture() {
        if (object) {
            qobject_cast<QQuickItem *>(object.get())->setParentItem(nullptr);
        }
    }
    void type(const QString &text) {
        for (const QChar c : text) {
            QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
            QCoreApplication::sendEvent(field, &press);
            QKeyEvent release(QEvent::KeyRelease, 0, Qt::NoModifier, QString(c));
            QCoreApplication::sendEvent(field, &release);
            QCoreApplication::processEvents();
        }
    }
    void key(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::keyClick(&window, key, modifiers);
        QCoreApplication::processEvents();
    }
    QString text() const {
        return field->property("text").toString();
    }
    QString selected() const {
        return field->property("selectedText").toString();
    }
    eden::core::TabModel tabs;
    eden::core::OmniboxController controller;
    OmniboxTestWindow shell;
    QQuickWindow window;
    QQmlComponent component;
    std::unique_ptr<QObject> object;
    QQuickItem *field = nullptr;
    QObject *popup = nullptr;
};

class OmniboxControllerTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanup();
    void enterFlushesPendingEdits();
    void hoverDoesNotSelectAndClickActivates();
    void controlEnterUsesTypedPrefix();
    void refreshPreservesCursorAndSelection();
    void remotePayloadValidation();
    void pathSlashDestinationsStayDistinct();
    void cleanupTestCase();
    void selectedInlineCompletionAndEnter();
    void exactHostAndPathEditing();
    void deletionAndPastePreserveLiteralInput();
    void keyboardSelectionAndEscape();
    void middleEditsAndUndo();
    void inputMethodComposition();
    void focusLossCancelsSuggestions();
    void remoteSuggestionsStaySearchesAndPreserveSelection();
    void remoteSuggestionsRejectStaleReplies();
    void privateInputDoesNotReachSuggestApi_data();
    void privateInputDoesNotReachSuggestApi();
    void strongHistoryPrefixOutranksWeakOpenTabMatch();
    void typedAddressBeatsSearchHistory();
    void searchWordsDoNotAcceptTitleMatches();
    void urlClassification_data();
    void urlClassification();
    void completionBoundaries_data();
    void completionBoundaries();
    void duplicateDestinationsCollapse();
    void explicitTabSelectionTracksMoves();
    void plaintextDatabaseMigratesToCiphertext();
    void corruptDatabaseIsNotReplaced();

  private:
    std::unique_ptr<QQmlEngine> m_uiEngine;
    std::unique_ptr<QSignalSpy> m_warnings;
};

void OmniboxControllerTest::initTestCase() {
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Settings", eden::core::SettingsStore::instance());
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Themes", eden::core::ThemeManager::instance());
    m_uiEngine = std::make_unique<QQmlEngine>();
    m_uiEngine->addImportPath(QCoreApplication::applicationDirPath());
    m_warnings = std::make_unique<QSignalSpy>(m_uiEngine.get(), &QQmlEngine::warnings);
}

void OmniboxControllerTest::cleanup() {
    QCOMPARE(m_warnings->count(), 0);
    m_warnings->clear();
}

void OmniboxControllerTest::enterFlushesPendingEdits() {
    OmniboxUiFixture ui(*m_uiEngine);
    QVERIFY(ui.field);
    QTRY_VERIFY(ui.field->hasActiveFocus());
    ui.type("gith");
    QKeyEvent paste(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
    QGuiApplication::clipboard()->setText("other.com");
    QMetaObject::invokeMethod(ui.field, "selectAll");
    QCoreApplication::sendEvent(ui.field, &paste);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(ui.field, &enter);
    QCOMPARE(ui.shell.navigated, QUrl("https://other.com"));
}

void OmniboxControllerTest::hoverDoesNotSelectAndClickActivates() {
    OmniboxUiFixture ui(*m_uiEngine);
    QVERIFY(ui.field);
    QTRY_VERIFY(ui.field->hasActiveFocus());
    ui.type("gith");
    QTRY_VERIFY(ui.popup->property("visible").toBool());
    const QString before = ui.text();
    QTest::mouseMove(&ui.window, QPoint(200, 120));
    QCoreApplication::processEvents();
    QCOMPARE(ui.controller.editor()->selectedIndex(), 0);
    QCOMPARE(ui.text(), before);
    QTest::mouseClick(&ui.window, Qt::LeftButton, Qt::NoModifier, QPoint(200, 120));
    QCOMPARE(ui.shell.navigated, ui.controller.searchDestination("gith"));
}

void OmniboxControllerTest::controlEnterUsesTypedPrefix() {
    OmniboxUiFixture ui(*m_uiEngine);
    QVERIFY(ui.field);
    QTRY_VERIFY(ui.field->hasActiveFocus());
    ui.type("gith");
    ui.key(Qt::Key_Return, Qt::ControlModifier);
    QCOMPARE(ui.shell.navigated, QUrl("https://www.gith.com"));
}

void OmniboxControllerTest::refreshPreservesCursorAndSelection() {
    eden::core::ProfileSettings settings("unused");
    SuggestionNetwork network;
    OmniboxUiFixture ui(*m_uiEngine, &settings, &network);
    QVERIFY(ui.field);
    QTRY_VERIFY(ui.field->hasActiveFocus());
    ui.type("coffee");
    ui.key(Qt::Key_Left);
    QCOMPARE(ui.field->property("cursorPosition").toInt(), 5);
    QTRY_COMPARE(network.replies.size(), 1);
    network.replies.last()->complete(R"(["coffee",["coffee beans","coffee shop"]])");
    QCOMPARE(ui.text(), QString("coffee"));
    QCOMPARE(ui.field->property("cursorPosition").toInt(), 5);
    ui.key(Qt::Key_Down);
    const QString selected = ui.text();
    ui.tabs.addTab(QUrl("https://coffee.example/"));
    QTest::qWait(40);
    auto *list = ui.object->findChild<QObject *>("omniboxSuggestionList");
    QVERIFY(list);
    QCOMPARE(list->property("currentIndex").toInt(), ui.controller.editor()->selectedIndex());
    QCOMPARE(ui.text(), selected);
}

void OmniboxControllerTest::remotePayloadValidation() {
    eden::core::ProfileSettings settings("unused");
    SuggestionNetwork network;
    eden::core::OmniboxController controller(nullptr, nullptr, nullptr, &settings, nullptr, &network);
    const QList<QByteArray> payloads{
        QByteArray("invalid JSON"),
        QByteArray(R"(["wrong query",["injected"]])"),
        QByteArray(R"(["query"] )"),
        QByteArray(65537, 'x'),
        QByteArray(R"(["query",["bad\u0001value"]])")
    };
    for (const QByteArray &payload : payloads) {
        controller.setQuery({});
        controller.setQuery("query");
        const int count = network.replies.size();
        QTRY_COMPARE(network.replies.size(), count + 1);
        network.replies.last()->complete(payload);
        QCOMPARE(controller.rowCount(), 1);
    }
}

void OmniboxControllerTest::pathSlashDestinationsStayDistinct() {
    eden::core::TabModel tabs([] { return std::make_unique<OmniboxEngineView>(); }, false);
    tabs.addTab(QUrl("https://example.com/path"));
    tabs.addTab(QUrl("https://example.com/path/"));
    eden::core::OmniboxController controller(&tabs, nullptr, nullptr);
    controller.updateQuery("path", false);
    QCOMPARE(controller.rowCount(), 3);
    QVERIFY(controller.suggestionUrl(1) != controller.suggestionUrl(2));
}

void OmniboxControllerTest::cleanupTestCase() {
    m_uiEngine.reset();
}

void OmniboxControllerTest::selectedInlineCompletionAndEnter() {
    QSignalSpy warnings(m_uiEngine.get(), &QQmlEngine::warnings);
    OmniboxUiFixture ui(*m_uiEngine);
    QVERIFY2(ui.field, qPrintable(ui.component.errorString()));
    QTRY_VERIFY(ui.field->hasActiveFocus());
    ui.type("gith");
    QCOMPARE(ui.controller.query(), QString("gith"));
    QCOMPARE(ui.text(), QString("github.com/Team/docs"));
    QCOMPARE(ui.selected(), QString("ub.com/Team/docs"));
    QCOMPARE(ui.field->property("cursorPosition").toInt(), 4);
    QTRY_VERIFY(ui.popup->property("visible").toBool());
    QVERIFY(ui.window.grabWindow().save("/tmp/eden-omnibox-inline.png"));
    ui.key(Qt::Key_Return);
    QCOMPARE(ui.shell.navigated, QUrl("https://github.com/Team/docs"));
    QTRY_VERIFY(!ui.field->hasActiveFocus());
    QTRY_VERIFY(!ui.popup->property("visible").toBool());
    QCOMPARE(warnings.count(), 0);
}

void OmniboxControllerTest::exactHostAndPathEditing() {
    OmniboxUiFixture ui(*m_uiEngine);
    QVERIFY(ui.field);
    QTRY_VERIFY(ui.field->hasActiveFocus());
    ui.type("github.com");
    QCOMPARE(ui.text(), QString("github.com"));
    QCOMPARE(ui.selected(), QString());
    ui.type("/T");
    QCOMPARE(ui.selected(), QString("eam/docs"));
    ui.type("wo");
    QCOMPARE(ui.text(), QString("github.com/Two"));
    QCOMPARE(ui.selected(), QString());
    ui.key(Qt::Key_Return);
    QCOMPARE(ui.shell.navigated, QUrl("https://github.com/Two"));
}

void OmniboxControllerTest::deletionAndPastePreserveLiteralInput() {
    OmniboxUiFixture ui(*m_uiEngine);
    QVERIFY(ui.field);
    QTRY_VERIFY(ui.field->hasActiveFocus());
    ui.type("gith");
    ui.key(Qt::Key_Backspace);
    QCOMPARE(ui.text(), QString("gith"));
    QCOMPARE(ui.selected(), QString());
    QTest::qWait(40);
    QCOMPARE(ui.text(), QString("gith"));
    ui.key(Qt::Key_Backspace);
    QCOMPARE(ui.text(), QString("git"));
    QCOMPARE(ui.selected(), QString());
    ui.key(Qt::Key_A, Qt::ControlModifier);
    QGuiApplication::clipboard()->setText("github.com/Team/d");
    ui.key(Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(ui.text(), QString("github.com/Team/d"));
    QCOMPARE(ui.selected(), QString());
    ui.key(Qt::Key_Return);
    QCOMPARE(ui.shell.navigated, QUrl("https://github.com/Team/d"));
}

void OmniboxControllerTest::keyboardSelectionAndEscape() {
    OmniboxUiFixture ui(*m_uiEngine);
    QVERIFY(ui.field);
    QTRY_VERIFY(ui.field->hasActiveFocus());
    ui.type("git");
    const QString query = ui.controller.query();
    ui.key(Qt::Key_Down);
    QCOMPARE(ui.controller.editor()->selectedIndex(), 1);
    const QUrl selected = ui.controller.suggestionUrl(1);
    QCOMPARE(ui.text(), ui.controller.suggestionText(ui.controller.editor()->selectedIndex()));
    ui.key(Qt::Key_Up);
    QCOMPARE(ui.controller.query(), query);
    QCOMPARE(ui.controller.editor()->selectedIndex(), 0);
    ui.key(Qt::Key_Escape);
    QCOMPARE(ui.text(), query);
    QVERIFY(ui.field->hasActiveFocus());
    QVERIFY(!ui.controller.editor()->popupOpen());
    ui.key(Qt::Key_Return);
    QCOMPARE(ui.shell.navigated, ui.controller.destination(query));
}

void OmniboxControllerTest::middleEditsAndUndo() {
    OmniboxUiFixture ui(*m_uiEngine);
    QVERIFY(ui.field);
    QTRY_VERIFY(ui.field->hasActiveFocus());
    ui.type("gith");
    ui.key(Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(ui.text(), QString("git"));
    QCOMPARE(ui.selected(), QString());
    ui.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(ui.text(), QString("gith"));
    ui.type("u");
    QCOMPARE(ui.selected(), QString("b.com/Team/docs"));
    ui.key(Qt::Key_Right);
    QCOMPARE(ui.selected(), QString());
    QCOMPARE(ui.text(), QString("https://github.com/Team/docs"));
    ui.key(Qt::Key_Home);
    ui.field->setProperty("cursorPosition", 8);
    ui.type("a");
    QCOMPARE(ui.text(), QString("https://agithub.com/Team/docs"));
    QCOMPARE(ui.selected(), QString());
    ui.key(Qt::Key_Return);
    QCOMPARE(ui.shell.navigated, QUrl("https://agithub.com/Team/docs"));
}

void OmniboxControllerTest::inputMethodComposition() {
    OmniboxUiFixture ui(*m_uiEngine);
    QVERIFY(ui.field);
    QTRY_VERIFY(ui.field->hasActiveFocus());
    ui.type("gith");
    QInputMethodEvent preedit(QString::fromUtf8("に"), {});
    QCoreApplication::sendEvent(ui.field, &preedit);
    QVERIFY(ui.field->property("inputMethodComposing").toBool());
    QVERIFY(!ui.controller.editor()->popupOpen());
    ui.key(Qt::Key_Return);
    QCOMPARE(ui.shell.navigated, QUrl());
    QInputMethodEvent commit;
    commit.setCommitString(QString::fromUtf8("日本"));
    QCoreApplication::sendEvent(ui.field, &commit);
    QCoreApplication::processEvents();
    QCOMPARE(ui.text(), QString::fromUtf8("gith日本"));
    QCOMPARE(ui.selected(), QString());
}

void OmniboxControllerTest::focusLossCancelsSuggestions() {
    OmniboxUiFixture ui(*m_uiEngine);
    QVERIFY(ui.field);
    QTRY_VERIFY(ui.field->hasActiveFocus());
    ui.type("gith");
    ui.field->setFocus(false);
    QTRY_VERIFY(!ui.controller.editor()->popupOpen());
    QCOMPARE(ui.controller.rowCount(), 0);
    QCOMPARE(ui.controller.query(), QString());
    QCOMPARE(ui.text(), ui.shell.displayUrl);
}

void OmniboxControllerTest::remoteSuggestionsStaySearchesAndPreserveSelection() {
    eden::core::ProfileSettings settings("unused");
    SuggestionNetwork network;
    OmniboxUiFixture ui(*m_uiEngine, &settings, &network);
    QVERIFY(ui.field);
    QTRY_VERIFY(ui.field->hasActiveFocus());
    ui.type("git");
    ui.key(Qt::Key_Down);
    const QUrl selected = ui.controller.suggestionUrl(ui.controller.editor()->selectedIndex());
    QTRY_COMPARE(network.replies.size(), 1);
    network.replies.first()->complete(R"(["git",["git tips","https://other.example"," git tips "]])");
    QCOMPARE(ui.controller.suggestionUrl(ui.controller.editor()->selectedIndex()), selected);
    QCOMPARE(ui.text(), ui.controller.suggestionText(ui.controller.editor()->selectedIndex()));
    int remoteRow = -1;
    int remoteCount = 0;
    for (int row = 0; row < ui.controller.rowCount(); ++row) {
        remoteCount +=
            ui.controller.data(ui.controller.index(row), eden::core::OmniboxController::KindRole).toString() ==
            "remote";
        if (ui.controller.data(ui.controller.index(row), eden::core::OmniboxController::TitleRole).toString() ==
            "https://other.example") {
            remoteRow = row;
        }
    }
    QVERIFY(remoteRow > 0);
    QCOMPARE(remoteCount, 2);
    QCOMPARE(ui.controller.suggestionUrl(remoteRow), ui.controller.searchDestination("https://other.example"));
    ui.controller.editor()->activate(remoteRow);
    QCOMPARE(ui.shell.navigated, ui.controller.searchDestination("https://other.example"));
}

void OmniboxControllerTest::remoteSuggestionsRejectStaleReplies() {
    eden::core::ProfileSettings settings("unused");
    SuggestionNetwork network;
    eden::core::OmniboxController controller(nullptr, nullptr, nullptr, &settings, nullptr, &network);
    controller.setQuery("old query");
    QTRY_COMPARE(network.replies.size(), 1);
    controller.setQuery("new query");
    QVERIFY(network.replies.first()->aborted);
    network.replies.first()->complete(R"(["old query",["old result"]])");
    QCOMPARE(controller.rowCount(), 1);
    QCOMPARE(controller.suggestionUrl(0), controller.destination("new query"));
    QTRY_COMPARE(network.replies.size(), 2);
    settings.setSearchSuggestions(false);
    QVERIFY(network.replies.last()->aborted);
    network.replies.last()->complete(R"(["new query",["new result"]])");
    QCOMPARE(controller.rowCount(), 1);
}

void OmniboxControllerTest::privateInputDoesNotReachSuggestApi_data() {
    QTest::addColumn<QString>("input");
    for (const QString &input :
         {QString("google.com"),
          QString("localhost:3000/token"),
          QString("person@example.com"),
          QString("https://user:password@example.com"),
          QString("router/admin"),
          QString("/tmp/file"),
          QString("example.com/?token=value"),
          QString("[::1]:8080")}) {
        QTest::newRow(qPrintable(input)) << input;
    }
}

void OmniboxControllerTest::privateInputDoesNotReachSuggestApi() {
    QFETCH(QString, input);
    eden::core::ProfileSettings settings("unused");
    SuggestionNetwork network;
    eden::core::OmniboxController controller(nullptr, nullptr, nullptr, &settings, nullptr, &network);
    controller.setQuery(input);
    QTest::qWait(130);
    QCOMPARE(network.requests.size(), 0);
}

void OmniboxControllerTest::strongHistoryPrefixOutranksWeakOpenTabMatch() {
    QTemporaryDir dataDirectory;
    QVERIFY(dataDirectory.isValid());
    const QString databasePath = dataDirectory.path() + "/eden.sqlite";
    const QByteArray databaseKey(32, 'k');
    eden::core::ProfileDatabase database(databasePath);
    QVERIFY(database.initialize(databaseKey));
    QFile encryptedDatabase(databasePath);
    QVERIFY(encryptedDatabase.open(QIODevice::ReadOnly));
    QVERIFY(!encryptedDatabase.read(16).startsWith("SQLite format 3"));
    QVERIFY(!QFileInfo::exists(databasePath + ".plaintext-migration"));
    QVERIFY(!QFileInfo::exists(databasePath + ".encrypting"));
    eden::core::TabModel tabs([] { return std::make_unique<OmniboxEngineView>(); }, false);
    tabs.addTab(QUrl("https://forging.it/help"));
    eden::core::HistoryStore history(databasePath);
    history.initialize(databaseKey);
    eden::core::BookmarkStore bookmarks(databasePath);
    bookmarks.initialize(databaseKey);
    const QUrl github("https://github.com/");
    history.recordVisit(github, "GitHub");

    eden::core::OmniboxController controller(&tabs, &history, &bookmarks);
    controller.setQuery("gith");
    QTRY_VERIFY_WITH_TIMEOUT(controller.rowCount() > 0, 200);
    QTRY_COMPARE(controller.data(controller.index(0), eden::core::OmniboxController::UrlRole).toUrl(), github);
    QCOMPARE(controller.completionSuffix(), QString("ub.com/"));

    controller.setQuery("different words");
    QCOMPARE(controller.completionSuffix(), QString());
    QCOMPARE(controller.suggestionUrl(0), controller.destination("different words"));

    controller.setQuery({});
    QCOMPARE(controller.rowCount(), 0);
    QCOMPARE(controller.completionSuffix(), QString());
}

void OmniboxControllerTest::typedAddressBeatsSearchHistory() {
    QTemporaryDir directory;
    const QString path = directory.filePath("history.sqlite");
    const QByteArray key(32, 'o');
    eden::core::ProfileDatabase database(path);
    QVERIFY(database.initialize(key));
    eden::core::HistoryStore history(path);
    history.initialize(key);
    history.recordVisit(QUrl("https://duckduckgo.com/?q=google.com"), "google.com");
    history.recordVisit(QUrl("https://google.com/old/search"), "Google");
    QTRY_COMPARE(history.rowCount(), 2);
    eden::core::OmniboxController controller(nullptr, &history, nullptr);
    QAbstractItemModelTester modelTester(&controller, QAbstractItemModelTester::FailureReportingMode::QtTest);
    controller.setQuery("google.com");
    QCOMPARE(controller.suggestionUrl(0), QUrl("https://google.com"));
    QCOMPARE(controller.completionSuffix(), QString());
    QCOMPARE(controller.data(controller.index(0), eden::core::OmniboxController::KindRole).toString(), QString("url"));
    controller.setQuery("google.com/");
    QCOMPARE(controller.suggestionUrl(0), QUrl("https://google.com/old/search"));
    QCOMPARE(controller.completionSuffix(), QString("old/search"));
    controller.setQuery("google.com/new");
    QCOMPARE(controller.suggestionUrl(0), QUrl("https://google.com/new"));
}

void OmniboxControllerTest::searchWordsDoNotAcceptTitleMatches() {
    QTemporaryDir directory;
    const QString path = directory.filePath("history.sqlite");
    const QByteArray key(32, 'o');
    eden::core::ProfileDatabase database(path);
    QVERIFY(database.initialize(key));
    eden::core::BookmarkStore bookmarks(path);
    bookmarks.initialize(key);
    bookmarks.add(QUrl("https://unrelated.example/page"), "best coffee");
    eden::core::OmniboxController controller(nullptr, nullptr, &bookmarks);
    controller.setQuery("best coffee");
    QCOMPARE(controller.suggestionUrl(0), controller.destination("best coffee"));
    QCOMPARE(controller.completionSuffix(), QString());
    QCOMPARE(controller.rowCount(), 2);
}

void OmniboxControllerTest::urlClassification_data() {
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected");
    QTest::newRow("domain") << "google.com" << "https://google.com";
    QTest::newRow("domain-path") << "google.com/maps" << "https://google.com/maps";
    QTest::newRow("explicit-http") << "http://example.com/a" << "http://example.com/a";
    QTest::newRow("localhost") << "localhost" << "http://localhost";
    QTest::newRow("localhost-port") << "localhost:3000/path" << "http://localhost:3000/path";
    QTest::newRow("intranet-port") << "printer:8080" << "http://printer:8080";
    QTest::newRow("intranet-path") << "router/admin" << "http://router/admin";
    QTest::newRow("ipv4") << "127.0.0.1:8080" << "http://127.0.0.1:8080";
    QTest::newRow("ipv6") << "[::1]:8080/path" << "http://[::1]:8080/path";
    QTest::newRow("bare-ipv6") << "fe80::1" << "http://[fe80::1]";
    QTest::newRow("idn") << "bücher.de" << "https://bücher.de";
    QTest::newRow("internal") << "eden://settings/search" << "eden://settings/search";
    QTest::newRow("blank") << "about:blank" << "about:blank";
    QTest::newRow("local-file") << "/tmp/a file.html" << "file:///tmp/a%20file.html";
    QTest::newRow("url-with-space") << "https://example.com/a b" << "https://example.com/a%20b";
    QTest::newRow("trimmed") << "  google.com  " << "https://google.com";
    QTest::newRow("single-search") << "coffee" << "https://duckduckgo.com/?q=coffee";
    QTest::newRow("search-dots") << "google.com alternatives" << "https://duckduckgo.com/?q=google.com%20alternatives";
    QTest::newRow("search-encoding") << "fish & chips + tea"
                                     << "https://duckduckgo.com/?q=fish%20%26%20chips%20%2B%20tea";
    QTest::newRow("search-email") << "person@example.com" << "https://duckduckgo.com/?q=person%40example.com";
    QTest::newRow("empty") << "   " << "";
}

void OmniboxControllerTest::urlClassification() {
    QFETCH(QString, input);
    QFETCH(QString, expected);
    eden::core::OmniboxController controller(nullptr, nullptr, nullptr);
    QCOMPARE(controller.destination(input), QUrl(expected));
    QCOMPARE(controller.destination("google", true), QUrl("https://www.google.com"));
    QCOMPARE(controller.destination("", true), QUrl());
    QCOMPARE(controller.destination("google.com", true), QUrl("https://google.com"));
    QCOMPARE(controller.destination("two words", true), controller.destination("two words"));
}

void OmniboxControllerTest::completionBoundaries_data() {
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("suffix");
    QTest::newRow("hostname-prefix") << "gith" << "ub.com/Team/docs";
    QTest::newRow("incomplete-tld") << "github.c" << "om/Team/docs";
    QTest::newRow("complete-tld") << "github.co" << "";
    QTest::newRow("complete-host") << "github.com" << "";
    QTest::newRow("host-slash") << "github.com/" << "Team/docs";
    QTest::newRow("path-prefix") << "github.com/Team/d" << "ocs";
    QTest::newRow("path-case") << "github.com/team/d" << "";
    QTest::newRow("path-different") << "github.com/other" << "";
    QTest::newRow("query-boundary") << "github.com/Team/docs?" << "";
    QTest::newRow("fragment-boundary") << "github.com/Team/docs#" << "";
    QTest::newRow("trailing-space") << "gith " << "";
    QTest::newRow("leading-space") << " gith" << "";
    QTest::newRow("explicit-scheme") << "https://gith" << "ub.com/Team/docs";
    QTest::newRow("host-case") << "GITH" << "ub.com/Team/docs";
}

void OmniboxControllerTest::completionBoundaries() {
    QFETCH(QString, input);
    QFETCH(QString, suffix);
    eden::core::TabModel tabs([] { return std::make_unique<OmniboxEngineView>(); }, false);
    tabs.addTab(QUrl("https://github.com/Team/docs?old=search#section"));
    eden::core::OmniboxController controller(&tabs, nullptr, nullptr);
    controller.setQuery(input);
    QCOMPARE(controller.completionSuffix(), suffix);
    if (!suffix.isEmpty()) {
        QCOMPARE(controller.suggestionUrl(0), QUrl("https://github.com/Team/docs"));
    }
    controller.updateQuery(input, false);
    QCOMPARE(controller.completionSuffix(), QString());
    QCOMPARE(controller.suggestionUrl(0), controller.destination(input));
}

void OmniboxControllerTest::duplicateDestinationsCollapse() {
    eden::core::TabModel tabs([] { return std::make_unique<OmniboxEngineView>(); }, false);
    tabs.addTab(QUrl("https://github.com/"));
    tabs.addTab(QUrl("https://github.com/"));
    eden::core::OmniboxController controller(&tabs, nullptr, nullptr);
    controller.setQuery("gith");
    QCOMPARE(controller.rowCount(), 2);
}

void OmniboxControllerTest::explicitTabSelectionTracksMoves() {
    eden::core::TabModel tabs([] { return std::make_unique<OmniboxEngineView>(); }, false);
    tabs.addTab(QUrl("https://first.example/"));
    tabs.addTab(QUrl("https://github.com/"));
    eden::core::OmniboxController controller(&tabs, nullptr, nullptr);
    controller.updateQuery("gith", false);
    QCOMPARE(controller.suggestionTabIndex(1), 1);
    tabs.moveTab(1, 0);
    QCOMPARE(controller.suggestionTabIndex(1), 0);
    tabs.closeTab(0);
    QCOMPARE(controller.suggestionTabIndex(1), -1);
}

void OmniboxControllerTest::plaintextDatabaseMigratesToCiphertext() {
    QTemporaryDir dataDirectory;
    QVERIFY(dataDirectory.isValid());
    const QString databasePath = dataDirectory.path() + "/eden.sqlite";
    {
        QSqlDatabase plaintext = QSqlDatabase::addDatabase("QSQLITE", "plaintext-migration");
        plaintext.setDatabaseName(databasePath);
        QVERIFY(plaintext.open());
        QSqlQuery query(plaintext);
        QVERIFY(query.exec(
            "CREATE TABLE history(url TEXT PRIMARY KEY,title TEXT NOT NULL,visited_at INTEGER NOT NULL,"
            "visit_count INTEGER NOT NULL DEFAULT 1)"
        ));
        QVERIFY(query.exec("INSERT INTO history VALUES('https://work.example','Work',42,3)"));
        plaintext.close();
    }
    QSqlDatabase::removeDatabase("plaintext-migration");
    const QByteArray databaseKey(32, 'm');
    eden::core::ProfileDatabase database(databasePath);
    QVERIFY(database.initialize(databaseKey));
    QFile encryptedDatabase(databasePath);
    QVERIFY(encryptedDatabase.open(QIODevice::ReadOnly));
    QVERIFY(!encryptedDatabase.read(16).startsWith("SQLite format 3"));
    QVERIFY(!QFileInfo::exists(databasePath + ".plaintext-migration"));
    QVERIFY(!QFileInfo::exists(databasePath + ".encrypting"));
    eden::core::HistoryStore history(databasePath);
    history.initialize(databaseKey);
    QCOMPARE(history.rowCount(), 1);
    QCOMPARE(history.data(history.index(0), eden::core::HistoryStore::TitleRole).toString(), QString("Work"));
}

void OmniboxControllerTest::corruptDatabaseIsNotReplaced() {
    QTemporaryDir dataDirectory;
    QVERIFY(dataDirectory.isValid());
    const QString databasePath = dataDirectory.path() + "/eden.sqlite";
    const QByteArray original("not-a-database");
    QFile file(databasePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(original), original.size());
    file.close();
    eden::core::ProfileDatabase database(databasePath);
    QVERIFY(!database.initialize(QByteArray(32, 'x')));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);
}

QTEST_MAIN(OmniboxControllerTest)

#include "omniboxcontroller_test.moc"
