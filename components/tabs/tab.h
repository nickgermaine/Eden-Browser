#ifndef TAB_H
#define TAB_H

#include <QWidget>
#include <QVBoxLayout>
#include <QtWebEngineWidgets/qwebengineview.h>
#include <QtWebEngineWidgets/qwebenginepage.h>
#include <QSplitter>
#include <QStackedLayout>
#include <QObject>
#include <QWebEngineProfile>
#include "components/networkmanager/urlintercepter.h"
#include "components/devtools/devtoolscontainer.h"

class Tab : public QWidget
{
    Q_OBJECT
public:
    explicit Tab(int *tc = 0, QStackedLayout *c = 0, QString mode = QString("normal"), QWidget *parent = 0);
    ~Tab();
    QVBoxLayout TabLayout;
    QWebEngineView TabContent;
    QWebEnginePage TabContentPage;

    QSplitter SplitView;
    QUrl TabUrl;
    int tabcount;
    void webTitleChanged();
    void OpenDevTools();


signals:
    void titleChanged(const QString &title, const QString &tab_name);
    void loadFinished(const bool &ok);
    void urlChanged(const QUrl &url, const QString &tab_name);
    void iconChanged(const QIcon &icon, const QString &tab_name);


public slots:
        void destroyTab();



};

#endif // TAB_H
