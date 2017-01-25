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
#include <tabs/tab.h>
#include <toolbar/addressbar.h>
#include <QtWebEngineWidgets/qwebengineview.h>
#include <QtWebEngineWidgets/qwebenginepage.h>
#include <QApplication>
#include <QUrl>
#include <QShortcut>
#include <QContextMenuEvent>
// Eden Includes
#include <tabs/tabbar.h>
#include <toolbar/addressbar.h>


class BaseApplication : public QFrame
{
    Q_OBJECT

public:
    BaseApplication(QFrame *parent = 0);
    void ECreateWindow();
    void AddTab(QStackedLayout *stack, EdenTabBar *tb);

    ~BaseApplication();
    int TabCount;
    // Tab Bar
    EdenTabBar TabBar;
    QList<Tab*> Tabs;
    Tab* CurrentTab;
    EdenAddressBar AddressBar;
    QPushButton NewTabButton;
    QListView contextMenu;

    // vars
    QString title;
    QFile jquery;
    QVBoxLayout layout;
    QHBoxLayout WindowBorderLayout;


    // Buttons
    QPushButton AddTabButton;
    QPushButton CloseBrowserButton;
    QPushButton MaxBrowserButton;
    QPushButton MinBrowserButton;
    QPushButton BackButton;
    QPushButton ForwardButton;
    QPushButton RefreshButton;
    QPushButton MenuButton;




    // Toolbar
    QWidget ToolBar;
    QHBoxLayout ToolBarLayout;


    // Window Border
    QWidget WindowBorder;
    QWidget window;

    // Other window stuff
    QSize BaseSize;
    QPoint oldPos;


    // Main View
    QWidget Container;
    QStackedLayout ContainerLayout;

    QVBoxLayout TabLayout;
    QWebEngineView* TabContent;
    QWebEnginePage TabContentPage;
    QSplitter SplitView;
    QUrl TabUrl;
    QWidget newtab;


    // Shortcuts

private:





protected:
    virtual void mousePressEvent(QMouseEvent*);
    virtual void mouseMoveEvent(QMouseEvent*);
    void center();


signals:
    void titleChanged(const QString &title, const QString &tab_name);
    void iconChanged(const QIcon &icon, const QString &tab_name);
    void currentUrlChanged(const QString &newurl);


public slots:

    void selectCurrentTab(int i);
    void loadPage();
    void toggleMax();
    void goBack();
    void goForward();
    void refresh();
    void ShowContextMenu(const QPoint &pos);
    void ShowMainMenu();



};

#endif // BASEAPPLICATION_H
