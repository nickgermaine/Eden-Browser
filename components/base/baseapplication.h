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
#include <components/tabs/tab.h>
#include <components/toolbar/addressbar.h>
#include <QtWebEngineWidgets/qwebengineview.h>
#include <QtWebEngineWidgets/qwebenginepage.h>
#include <QApplication>
#include <QUrl>
#include <QShortcut>
#include <QContextMenuEvent>
// Eden Includes
#include <components/tabs/tabbar.h>
#include <components/tabs/edenpinnedtabbar.h>
#include <components/toolbar/addressbar.h>
#include <QListWidget>
#include <QGraphicsWidget>
#include <QGraphicsLayout>
#include <QGraphicsLayoutItem>
#include <QGraphicsScene>

class BaseApplication : public QFrame
{
    Q_OBJECT

public:
    explicit BaseApplication(QString mode);
    void ECreateWindow();
    void AddTab(QString mode= QString("normal"));
    void ConvertToPinnedTab(int &tabIndex);

    ~BaseApplication();
    int TabCount;

    // Top level stuff
    QString Mode;

    // Tab Bar
    EdenTabBar TabBar;
    EdenPinnedTabBar PinnedTabBar;
    QList<Tab*> PinnedTabs;
    int PinnedTabCount;
    QList<Tab*> Tabs;
    Tab* CurrentTab;
    EdenAddressBar AddressBar;
    QPushButton NewTabButton;
    QMenu contextMenu;
    int TabUnderMouse;

    // For icon color in windows
    QString ic;

    // vars
    QString title;
    QFile jquery;
    QVBoxLayout layout;
    QHBoxLayout WindowBorderLayout;

    QMenu* MainMenu;
    QGridLayout windowLayout;
    QWidget window;

    // Notifications... experimental:
    QPushButton NotificationButton;
    QWidget NotificationContainer;
    QWidget NotificationShadow;
    QVBoxLayout NotificationLayout;
    QWidget Notification;
    QWebEngineView NotificationView;
    QUrl NotificationUrl;

    QGraphicsView NS;



    // Buttons
    QPushButton AddTabButton;
    QPushButton CloseBrowserButton;
    QPushButton MaxBrowserButton;
    QPushButton MinBrowserButton;
    QPushButton BackButton;
    QPushButton ForwardButton;
    QPushButton RefreshButton;
    QPushButton MenuButton;

    QPushButton GNotifications;
    QPushButton ShareButton;
    QPushButton BookmarkButton;
    QPushButton LockButton;
    QWidget AddressBarContainer;
    QHBoxLayout AddressBarLayout;




    // Toolbar
    QWidget ToolBar;
    QHBoxLayout ToolBarLayout;


    // Window Border
    QWidget WindowBorder;


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
    void createNewWindow(QString mode);


public slots:

    void selectCurrentTab(int i);
    void loadPage();
    void toggleMax();
    void goBack();
    void goForward();
    void refresh();
    void ShowContextMenu(const QPoint &pos);
    void ShowMainMenu();
    void CloseTab(const int &i);
    void ShowTabContextMenu(const QPoint &post);
    void CloseOtherTabs(int i);
    void ShowNotifications();



};

#endif // BASEAPPLICATION_H
