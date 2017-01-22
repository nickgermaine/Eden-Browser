#ifndef TAB_H
#define TAB_H

#include <QWidget>
#include <QVBoxLayout>
#include <QtWebEngineWidgets/QtWebEngineWidgets>
#include <QSplitter>
#include <QStackedLayout>
#include <QObject>


class Tab : public QWidget
{
    Q_OBJECT
public:
    explicit Tab(int *tc = 0, QStackedLayout *c = 0, QWidget *parent = 0);
    QVBoxLayout TabLayout;
    QWebEngineView TabContent;
    QWebEnginePage TabContentPage;
    QSplitter SplitView;
    QUrl TabUrl;
    void webTitleChanged();

signals:
    void titleChanged();

public slots:
};

#endif // TAB_H
