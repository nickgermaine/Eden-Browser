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
    ~BaseApplication();

    // vars
    QString title;
    QFile jquery;
    QVBoxLayout layout;
    QHBoxLayout WindowBorderLayout;

    // Shortcuts


    int tabCount;

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
    QStackedLayout ContainerLayout;

protected:
    virtual void mousePressEvent(QMouseEvent*);
    virtual void mouseMoveEvent(QMouseEvent*);
    void center();
    void AddTab();

public slots:




};

#endif // BASEAPPLICATION_H
