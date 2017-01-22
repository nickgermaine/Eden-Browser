#ifndef BASEAPPLICATION_H
#define BASEAPPLICATION_H

#include <QMainWindow>
#include <QFrame>
#include <QFile>
#include <QVBoxLayout>
#include <QPushButton>
#include <QTabBar>
#include <QWidget>
#include <QStackedLayout>
#include <QMouseEvent>
#include <QPoint>
#include <QList>
#include <tab.h>

// Eden Includes
#include <tabbar.h>
#include <addressbar.h>


class BaseApplication : public QFrame
{
    Q_OBJECT

public:
    BaseApplication(QFrame *parent = 0);
    void CreateWindow();
    void AddTab(QStackedLayout *stack, EdenTabBar *tb);

    ~BaseApplication();


    // Shortcuts

private:

    int tabCount;

    // vars
    QString title;
    QFile jquery;
    QVBoxLayout layout;
    QHBoxLayout WindowBorderLayout;


    // Buttons
    QPushButton AddTabButton;
    QPushButton CloseBrowserButton;
    QPushButton MaximizeBrowserButton;
    QPushButton MinimizeBrowserButton;
    QPushButton BackButton;
    QPushButton ForwardButton;
    QPushButton RefreshButton;
    QPushButton MenuButton;

    // Tab Bar
    EdenTabBar TabBar;
    QList<QWidget*> Tabs;
    int TabCount;

    // Toolbar
    QWidget ToolBar;
    QHBoxLayout ToolBarLayout;
    EdenAddressBar AddressBar;

    // Window Border
    QWidget WindowBorder;
    QWidget window;

    // Other window stuff
    QSize BaseSize;
    QPoint oldPos;


    // Main View
    QWidget Container;
    QVBoxLayout ContainerLayout;

    QVBoxLayout TabLayout;
    QWebEngineView* TabContent;
    QWebEnginePage TabContentPage;
    QSplitter SplitView;
    QUrl TabUrl;
    QWidget newtab;

protected:
    virtual void mousePressEvent(QMouseEvent*);
    virtual void mouseMoveEvent(QMouseEvent*);
    void center();


public slots:
    void AddTabContent();



};

#endif // BASEAPPLICATION_H
