#ifndef BASEAPPLICATION_H
#define BASEAPPLICATION_H

#include <QMainWindow>
#include <QFrame>
#include <QFile>
#include <QVBoxLayout>
#include <QShortcut>
#include <QPushButton>
#include <QTabBar>
#include <QWidget>

class BaseApplication : public QMainWindow
{
    Q_OBJECT

public:
    BaseApplication(QWidget *parent = 0);
    void CreateWindow();
    ~BaseApplication();

    // vars
    QString title;
    QFile jquery;
    QVBoxLayout layout;

    // Shortcuts
    QShortcut sc_refresh;
    QShortcut sc_refresh2;
    QShortcut sc_refresh3;
    QShortcut sc_newTab;
    QShortcut sc_devTools;

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
    QTabBar TabBar;

    // Window Border
    QWidget WindowBorder;


};

#endif // BASEAPPLICATION_H
