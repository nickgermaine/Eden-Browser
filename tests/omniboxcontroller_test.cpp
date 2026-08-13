#include "core/bookmarks/bookmarkstore.h"
#include "core/history/historystore.h"
#include "core/omni/omniboxcontroller.h"
#include "core/window/tabmodel.h"
#include "engine/engineview.h"

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

class OmniboxControllerTest final : public QObject {
    Q_OBJECT

  private slots:
    void strongHistoryPrefixOutranksWeakOpenTabMatch();
};

void OmniboxControllerTest::strongHistoryPrefixOutranksWeakOpenTabMatch() {
    eden::core::TabModel tabs([] { return std::make_unique<OmniboxEngineView>(); }, false);
    tabs.addTab(QUrl("https://forging.it/help"));
    eden::core::HistoryStore history;
    eden::core::BookmarkStore bookmarks;
    const QUrl github("https://github.com/");
    history.recordVisit(github, "GitHub");

    eden::core::OmniboxController controller(&tabs, &history, &bookmarks);
    controller.setQuery("gith");
    QTRY_VERIFY_WITH_TIMEOUT(controller.rowCount() > 0, 200);
    QCOMPARE(controller.data(controller.index(0), eden::core::OmniboxController::UrlRole).toUrl(), github);
    QCOMPARE(controller.completionSuffix(), QString("ub.com/"));

    controller.setQuery("different words");
    QCOMPARE(controller.completionSuffix(), QString());
    QCOMPARE(controller.suggestionUrl(0), controller.destination("different words"));

    controller.setQuery({});
    QCOMPARE(controller.rowCount(), 0);
    QCOMPARE(controller.completionSuffix(), QString());
}

QTEST_GUILESS_MAIN(OmniboxControllerTest)

#include "omniboxcontroller_test.moc"
