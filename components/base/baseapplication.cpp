/****************************************************************************
 *
 * Base Application
 * Author: Nick Germaine
 *
 * This is the top level of each browser window.  We use this to control the
 * toolbar, tabbar, and also loop back to the core application to create new
 * windows.
 *
 *
 *
 *
 * **************************************************************************/



#include "baseapplication.h"
#include <QSize>
#include <QWidget>
#include <iostream>
#include <QApplication>
#include <QDesktopWidget>
#include <QGraphicsEffect>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QRect>
#include <QDebug>
#include <QStringListModel>
#include <QListData>
#include <QToolButton>


BaseApplication::BaseApplication(QString mode)
    : QFrame()
{

    // Always create a new Eden Window
    // Check window mode, and set
    Mode = mode;
    if(mode.toStdString() == "normal"){
        ic = QString("black");

    }else{
        ic = QString("white");
    }

    PinnedTabCount = 0;

    ECreateWindow();

    this->center();
    this->setContextMenuPolicy(Qt::CustomContextMenu);

    // Special top level context menu
    connect(this, SIGNAL(customContextMenuRequested(const QPoint &)),
            this, SLOT(ShowContextMenu(const QPoint &)));

    // Tab bar also has its own custom context menu
    TabBar.setContextMenuPolicy(Qt::CustomContextMenu);
    connect(&TabBar, &EdenTabBar::customContextMenuRequested, this, &BaseApplication::ShowTabContextMenu);




}

BaseApplication::~BaseApplication()
{

}

void BaseApplication::ECreateWindow(){
    // No Margin Margins()
    QMargins no_margins;
    no_margins.setBottom(0);
    no_margins.setLeft(0);
    no_margins.setRight(0);
    no_margins.setTop(0);

    // Create Main Window

    /*
     *
     *      EDEN WINDOWBORDER
     *          Includes the TabBar, min, max, and close buttons.
     *
     */


    // Set some important properties
    WindowBorderLayout.setContentsMargins(no_margins);
    WindowBorderLayout.setSpacing(0);
    WindowBorderLayout.setObjectName("windowControls");

    // Same for layout
    layout.setContentsMargins(no_margins);
    layout.setSpacing(0);
    this->setObjectName("BrowserWindow");

    // Close/Max/Min Buttons
    CloseBrowserButton.setIcon(QIcon(":/resources/icons/ic_close_" + ic + "_24px.svg"));
    CloseBrowserButton.setFixedWidth(48);

    MaxBrowserButton.setIcon(QIcon(":/resources/icons/ic_aspect_ratio_" + ic + "_24px.svg"));
    MaxBrowserButton.setFixedWidth(48);

    MinBrowserButton.setIcon(QIcon(":/resources/icons/ic_remove_" + ic + "_24px.svg"));
    MinBrowserButton.setFixedWidth(48);

    // Connect Window Buttons
    connect(&MinBrowserButton, &QPushButton::clicked, this, &BaseApplication::showMinimized);
    connect(&MaxBrowserButton, &QPushButton::clicked, this, &BaseApplication::toggleMax);
    QObject::connect(&CloseBrowserButton, &QPushButton::clicked, [this](){
        destroy();
        //QApplication::instance()->quit();
    });


    // And begin the tabbar
    this->TabBar.setObjectName(QString("TabBar"));
    TabCount = 0;

    // Need to connect signals so the webviews can update the titles/icons on tabs
    connect(this, &BaseApplication::titleChanged, &TabBar, &EdenTabBar::updateTitle);
    connect(this, &BaseApplication::iconChanged, &TabBar, &EdenTabBar::updateIcon);
    connect(&TabBar, &EdenTabBar::tabBarClicked, this, &BaseApplication::selectCurrentTab);
    connect(&TabBar, &EdenTabBar::tabCloseRequested, this, &BaseApplication::CloseTab);

    // New tab button goes directly right of the tabs list
    NewTabButton.setIcon(QIcon(":/resources/icons/ic_add_" + ic + "_24px.svg"));
    connect(&NewTabButton, &QPushButton::clicked, [this](){
        AddTab(Mode);
    });

    QPushButton *privateWindow = new QPushButton();
    privateWindow->setIcon(QIcon(":/resources/icons/ic_security_white_24px.svg"));

    // Build window border
    if(ic.toStdString() == "white"){
        WindowBorderLayout.addWidget(privateWindow);
    }
    //WindowBorderLayout.addWidget(&PinnedTabBar);
    WindowBorderLayout.addWidget(&TabBar);
    WindowBorderLayout.addWidget(&NewTabButton);
    WindowBorderLayout.addStretch();
    WindowBorderLayout.addWidget(&MinBrowserButton);
    WindowBorderLayout.addWidget(&MaxBrowserButton);
    WindowBorderLayout.addWidget(&CloseBrowserButton);
    WindowBorder.setLayout(&WindowBorderLayout);








    /*
     *
     *      EDEN TOOLBAR
     *          Basically, just the back/forward/addressbar/menu buttons... yeah.
     *
     */


    // Some important properties for toolbar
    ToolBar.setObjectName("TabControls");
    ToolBar.setLayout(&ToolBarLayout);

    // Build the widgets
    QPushButton *BackButton = new QPushButton();
    BackButton->setIcon(QIcon(":/resources/icons/ic_keyboard_arrow_left_" + ic + "_24px.svg"));

    QPushButton *ForwardButton = new QPushButton();
    ForwardButton->setIcon(QIcon(":/resources/icons/ic_keyboard_arrow_right_" + ic + "_24px.svg"));

    QPushButton *RefreshButton = new QPushButton();
    RefreshButton->setIcon(QIcon(":/resources/icons/ic_refresh_" + ic + "_24px.svg"));


    // Experimental notification button.  I say "experimental" because for some reason,
    // a webview will die if you set visibility to false and then true.  So our dropdown
    // That should show the notifications essentially dies as soon as its born.

    // If anyone knows how to prevent this, let me know.  I might get as dirty as setting
    // display:none in css, if that's possible...
    NotificationButton.setIcon(QIcon(":/resources/icons/ic_notifications_none_" + ic + "_24px.svg"));

    // Connect the buttons preceding the address bar
    connect(BackButton, &QPushButton::clicked, this, &BaseApplication::goBack);
    connect(ForwardButton, &QPushButton::clicked, this, &BaseApplication::goForward);
    connect(RefreshButton, &QPushButton::clicked, this, &BaseApplication::refresh);

    // Build Address Bar
    BookmarkButton.setIcon(QIcon(":/resources/icons/ic_bookmark_border_" + ic + "_24px.svg"));
    LockButton.setIcon(QIcon(":/resources/icons/ic_language_" + ic + "_24px.svg"));

    connect(&AddressBar, &EdenAddressBar::returnPressed, this, &BaseApplication::loadPage);

    AddressBarContainer.setLayout(&AddressBarLayout);
    AddressBarLayout.setContentsMargins(0,0,0,0);
    AddressBarContainer.setObjectName("AddressBarContainer");
    AddressBarLayout.addWidget(&LockButton);
    AddressBarLayout.addWidget(&AddressBar);
    AddressBarLayout.addWidget(&BookmarkButton);

    // Create menu button
    MenuButton.setIcon(QIcon(":/resources/icons/ic_more_vert_" + ic + "_24px.svg"));
    connect(&MenuButton, &QToolButton::clicked, this, &BaseApplication::ShowMainMenu);

    // Add Objects to ToolBar
    ToolBarLayout.addWidget(BackButton);
    ToolBarLayout.addWidget(ForwardButton);
    ToolBarLayout.addWidget(RefreshButton);
    ToolBarLayout.addWidget(&AddressBarContainer);
    ToolBarLayout.addWidget(&NotificationButton);
    ToolBarLayout.addWidget(&MenuButton);




    /*
     *
     *      EDEN MAINVIEW AREA
     *          Just the webview/devtools container
     *
     */


    // Some important properties
    Container.setContentsMargins(no_margins);
    Container.setLayout(&ContainerLayout);



    // Set some final properties and show this window
    this->title = "Eden Browser";
    this->setWindowTitle(this->title);
    this->setWindowFlags(Qt::FramelessWindowHint);

    QSize BaseSize;

    BaseSize.setHeight(768);
    BaseSize.setWidth(1366);
    this->setBaseSize(BaseSize);
    this->setMinimumSize(1366,768);

    layout.addWidget(&WindowBorder);
    layout.addWidget(&ToolBar);
    layout.addWidget(&Container);

    window.setLayout(&layout);

    show();

    // Create new tab immediately!
    this->AddTab(Mode);



    // Instantiate some keyboard shortcuts because all the cool kids are doing it
    QShortcut *shortcut = new QShortcut(QKeySequence(tr("Ctrl+T", "New Tab")), this);
    QShortcut *devtools = new QShortcut(QKeySequence(tr("Ctrl+Shift+I", "Open Developer Tools")), this);

    connect(shortcut, &QShortcut::activated, [this](){
        this->AddTab(Mode);
    });

    connect(devtools, &QShortcut::activated, [this](){
       CurrentTab->OpenDevTools();
    });


    // Finish building our window by adding the window to the top level
    windowLayout.addWidget(&window, 0,0,1,1);
    windowLayout.setSpacing(0);
    windowLayout.setContentsMargins(0, 0, 0, 0);

    // Notifications... experimental
    NotificationView.setUrl(NotificationUrl.fromUserInput(QString("https://plus.google.com/notifications/all")));

    QGraphicsDropShadowEffect *dropshadow = new QGraphicsDropShadowEffect;
    dropshadow->setBlurRadius(20);
    dropshadow->setColor(QColor(0, 0, 0, 150));
    dropshadow->setOffset(QPointF(2, 2));


    //NotificationShadow.setGraphicsEffect(dropshadow);
    //NotificationLayout.addWidget(&Notification);
    NotificationLayout.addWidget(&NotificationView);
    NotificationView.raise();
    NotificationContainer.setLayout(&NotificationLayout);
    NotificationContainer.setObjectName("NotificationDropdown");
    windowLayout.addWidget(&NotificationContainer, 0, 0, 1, 1);
    NotificationContainer.raise();
    NotificationContainer.setContentsMargins(0, 0, 0, 0);

    NotificationContainer.setFixedHeight(560);
    NotificationContainer.setFixedWidth(400);
    NotificationContainer.setGraphicsEffect(dropshadow);
    NotificationLayout.setContentsMargins(0, 0, 0, 0);
    //NotificationContainer.setVisible(false);

    int offsetX = this->geometry().width() + this->geometry().left() - 255;
    int offsetY = this->geometry().top() + 85;

    NotificationContainer.move(QPoint(offsetX, offsetY));
    NotificationContainer.setHidden(true);

    connect(&NotificationButton, &QPushButton::clicked, this, &BaseApplication::ShowNotifications);

    setLayout(&windowLayout);



}


void BaseApplication::mousePressEvent(QMouseEvent *event) {

    oldPos = event->globalPos();
}

void BaseApplication::mouseMoveEvent(QMouseEvent *event) {
    QPoint delta = QPoint(event->globalPos() - oldPos);
    move(this->x() + delta.x(), this->y() + delta.y());
    oldPos = event->globalPos();
}

void BaseApplication::center(){
    QRect qr = frameGeometry();
    QPoint cp = QDesktopWidget().availableGeometry().center();
    qr.moveCenter(cp);
    move(qr.topLeft());
}

void BaseApplication::AddTab(QString mode){

    /***************************************************************
     *
     *
     * Ok.  So,we create a tab, put it into Tabs[] and then add it to
     * the stack layout.  Seems simple.
     *
     *
     *
     *
     * */


    Tab *newtab = new Tab(&TabCount, &ContainerLayout, Mode);
    Tabs.append(newtab);

    CurrentTab = Tabs[TabCount];

    TabBar.addTab(QString("New Tab"));
    int t = TabBar.count() - 1;

    TabBar.setTabData(t, QVariant(QString("tab" + QString::number(TabCount))));

    connect(newtab, &Tab::titleChanged, [this, newtab](const QString &title, const QString &tab_name){

        emit titleChanged(title, tab_name);
    });

    connect(newtab, &Tab::iconChanged, [this, newtab](const QIcon &icon, const QString &tab_name){
        emit iconChanged(icon, tab_name);
    });

    connect(newtab, &Tab::urlChanged, [this, newtab](const QUrl &url, const QString &tab_name){
        if(TabBar.tabData(TabBar.currentIndex()) == tab_name){
            AddressBar.setText(url.toString());
        }
    });


    ContainerLayout.addWidget(Tabs[TabCount]);
    ContainerLayout.setCurrentWidget(Tabs[TabCount]);
    TabBar.setCurrentIndex(t);
    TabCount++;
}

void BaseApplication::CloseOtherTabs(int i){
    QVariant tab_name = TabBar.tabData(i);

    for(int a = 0;a < TabBar.count(); a++){
        if(TabBar.tabData(a) != tab_name){
            this->CloseTab(a);
        }
    }
}

void BaseApplication::CloseTab(const int &i = -1){
    if(i == -1){
        int a = TabUnderMouse;

        QString tab_name = TabBar.tabData(a).toString();
        Tabs[a]->destroyTab();
        TabBar.removeTab(a);

    }else{
        QString tab_name = TabBar.tabData(i).toString();
        Tabs[i]->destroyTab();
        TabBar.removeTab(i);

    }

}


void BaseApplication::selectCurrentTab(int i){

    QString tab_name = this->TabBar.tabData(i).toString();
    qDebug() << "clicked Tab " << tab_name << " at index " << i;
    CurrentTab = Container.findChild<Tab *>(tab_name);

    ContainerLayout.setCurrentWidget(CurrentTab);
    AddressBar.setText(CurrentTab->TabContent.page()->url().toString());

}


void BaseApplication::loadPage(){

    QUrl *newUrl = new QUrl;
    CurrentTab->TabContent.load(newUrl->fromUserInput(AddressBar.text()));

}

void BaseApplication::goBack(){
    CurrentTab->TabContent.back();
}

void BaseApplication::goForward(){
    CurrentTab->TabContent.forward();
}

void BaseApplication::refresh(){
    CurrentTab->TabContent.reload();
}

void BaseApplication::toggleMax(){
    if(isMaximized()){
        showNormal();
    }else{
        showMaximized();
    }
}

void BaseApplication::ShowContextMenu(const QPoint &pos)
{
   QMenu contextMenu(tr("Context menu"), this);

   QAction action1("Remove Data Point", this);
   connect(&action1, SIGNAL(triggered()), this, SLOT(removeDataPoint()));
   contextMenu.addAction(&action1);

   contextMenu.exec(mapToGlobal(pos));
}

void BaseApplication::ShowMainMenu(){



    QMenu MainMenu(tr("Context menu"), this);

    QAction newtab("New tab", this);
    newtab.setIcon(QIcon(QString(":/resources/icons/ic_add_" + ic + "_24px.svg")));

    QAction newwindow("New window", this);
    newwindow.setIcon(QIcon(QString(":/resources/icons/ic_launch_" + ic + "_24px.svg")));

    connect(&newwindow, &QAction::triggered, [this](){
        emit createNewWindow(QString("normal"));
    });

    QAction newpwindow("New private window", this);
    newpwindow.setIcon(QIcon(QString(":/resources/icons/ic_lock_" + ic + "_24px.svg")));

    connect(&newpwindow, &QAction::triggered,[this](){
        emit createNewWindow(QString("private"));
    });

    QAction bookmarks("Bookmarks", this);
    bookmarks.setIcon(QIcon(QString(":/resources/icons/ic_bookmark_border_" + ic + "_24px.svg")));

    QAction reading("Reading list", this);
    reading.setIcon(QIcon(QString(":/resources/icons/ic_chrome_reader_mode_" + ic + "_24px.svg")));

    QAction history("History", this);
    history.setIcon(QIcon(QString(":/resources/icons/ic_restore_" + ic + "_24px.svg")));


    QAction downloads("Downloads", this);
    downloads.setIcon(QIcon(QString(":/resources/icons/ic_file_download_" + ic + "_24px.svg")));


    QAction settings("Settings", this);
    settings.setIcon(QIcon(QString(":/resources/icons/ic_settings_" + ic + "_24px.svg")));


    QAction about("About Eden Browser", this);
    about.setIcon(QIcon(QString(":/resources/icons/ic_add_" + ic + "_24px.svg")));


    QAction help("Help", this);
    help.setIcon(QIcon(QString(":/resources/icons/ic_add_" + ic + "_24px.svg")));


    QAction quit("Quit", this);
    quit.setIcon(QIcon(QString(":/resources/icons/ic_add_" + ic + "_24px.svg")));

    MainMenu.addAction(&newtab);
    MainMenu.addAction(&newwindow);
    MainMenu.addAction(&newpwindow);
    MainMenu.addAction(&bookmarks);
    MainMenu.addAction(&reading);
    MainMenu.addAction(&history);
    MainMenu.addAction(&downloads);
    MainMenu.addAction(&settings);
    MainMenu.addAction(&about);
    MainMenu.addAction(&help);
    MainMenu.addAction(&quit);




    int offsetX = this->geometry().width() + this->geometry().left() - 255;
    int offsetY = this->geometry().top() + 85;


    MainMenu.exec(QPoint(offsetX, offsetY));
    MainMenu.move(QPoint(offsetX, offsetY));
    QGraphicsDropShadowEffect *e = new QGraphicsDropShadowEffect;
    e->setColor(QColor(40,40,40,245));
    e->setOffset(0,10);
    e->setBlurRadius(50);

    MainMenu.setGraphicsEffect(e);
    qDebug() << "populate x: " << offsetX << " and y: " << offsetY;
}

void BaseApplication::ShowTabContextMenu(const QPoint &pos)
{
    if (pos.isNull()){
            return;
    }

    int tabIndex = TabBar.tabAt(pos);
    TabUnderMouse = tabIndex;

   QMenu contextMenu(tr("Context menu"), this);

   QAction action1("New tab", this);
   connect(&action1, &QAction::triggered, [this](){
       this->AddTab(Mode);
   });

   QAction action2("Reload", this);
   connect(&action2, &QAction::triggered, this, &BaseApplication::refresh);

   QAction action3("Duplicate", this);
   QAction action4("Pin tab", this);

   connect(&action4, &QAction::triggered, [this](){
       this->ConvertToPinnedTab(TabUnderMouse);
   });
   QAction action6("Close tab", this);
   connect(&action6, &QAction::triggered, [this](){
       this->CloseTab();
   });

   QAction action7("Close other tabs", this);
   connect(&action7, &QAction::triggered, [this](){
       this->CloseOtherTabs(TabUnderMouse);
   });

   QAction action8("Add to bookmarks", this);
   QAction action9("Add to reading list", this);

   connect(&action1, SIGNAL(triggered()), this, SLOT(removeDataPoint()));
   contextMenu.addAction(&action1);
   contextMenu.addSeparator();
   contextMenu.addAction(&action2);
   contextMenu.addAction(&action3);
   contextMenu.addAction(&action4);

   contextMenu.addSeparator();
   contextMenu.addAction(&action6);
   contextMenu.addAction(&action7);
   contextMenu.addAction(&action8);
   contextMenu.addAction(&action9);


   contextMenu.exec(mapToGlobal(pos));
}

void BaseApplication::ShowNotifications(){


    int offsetX = this->geometry().width() + this->geometry().left() - 525;
    int offsetY = this->geometry().top() + 47;


    if(NotificationContainer.isVisible() == true){
        NotificationContainer.setVisible(false);
    }else{

        NotificationContainer.setVisible(true);
        NotificationContainer.move(QPoint(offsetX, offsetY));
    }
}

void BaseApplication::ConvertToPinnedTab(int &tabIndex){
    QString tab_name = TabBar.tabData(tabIndex).toString();
    QIcon tab_icon = TabBar.tabIcon(tabIndex);


    PinnedTabBar.PinnedTabBar.addTab(QString(""));
    PinnedTabBar.PinnedTabBar.setTabIcon(PinnedTabCount, tab_icon);
    PinnedTabCount++;
    TabBar.removeTab(tabIndex);

}


