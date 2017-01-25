#include "baseapplication.h"
#include <QSize>
#include <QWidget>
#include <iostream>
#include <QApplication>
#include <QDesktopWidget>
#include <QGraphicsEffect>
#include <QGraphicsDropShadowEffect>


BaseApplication::BaseApplication(QFrame *parent)
    : QFrame(parent)
{

    ECreateWindow();
    this->center();

    this->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(this, SIGNAL(customContextMenuRequested(const QPoint &)),
            this, SLOT(ShowContextMenu(const QPoint &)));


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
    QWidget *window = new QWidget;



    /*
     *
     *      EDEN WINDOWBORDER
     *          Includes the TabBar, min, max, and close buttons.
     *
     */


    WindowBorder;
    WindowBorderLayout;

    WindowBorderLayout.setContentsMargins(no_margins);
    WindowBorderLayout.setSpacing(0);

    WindowBorderLayout.setObjectName("windowControls");

    TabBar;
    layout;

    Tabs;


    layout.setContentsMargins(no_margins);
    layout.setSpacing(0);
    this->setObjectName("BrowserWindow");

    // Close/Max/Min Buttons
    CloseBrowserButton;
    CloseBrowserButton.setIcon(QIcon(":/resources/icons/ic_close_black_24px.svg"));
    CloseBrowserButton.setFixedWidth(48);

    MaxBrowserButton;
    MaxBrowserButton.setIcon(QIcon(":/resources/icons/ic_aspect_ratio_black_24px.svg"));
    MaxBrowserButton.setFixedWidth(48);

    MinBrowserButton;
    MinBrowserButton.setIcon(QIcon(":/resources/icons/ic_remove_black_24px.svg"));
    MinBrowserButton.setFixedWidth(48);

    // Connect Window Buttons
    connect(&MinBrowserButton, &QPushButton::clicked, this, &BaseApplication::showMinimized);
    connect(&MaxBrowserButton, &QPushButton::clicked, this, &BaseApplication::toggleMax);
    QObject::connect(&CloseBrowserButton, &QPushButton::clicked, [this](){
        destroy();
        QApplication::instance()->quit();
    });

    this->TabBar.setObjectName(QString("TabBar"));
    TabCount = 0;

    connect(this, &BaseApplication::titleChanged, &TabBar, &EdenTabBar::updateTitle);
    connect(this, &BaseApplication::iconChanged, &TabBar, &EdenTabBar::updateIcon);
    connect(&TabBar, &EdenTabBar::tabBarClicked, this, &BaseApplication::selectCurrentTab);

    NewTabButton;
    NewTabButton.setIcon(QIcon(":/resources/icons/ic_add_black_24px.svg"));

    WindowBorderLayout.addWidget(&TabBar);
    WindowBorderLayout.addWidget(&NewTabButton);
    WindowBorderLayout.addStretch();
    WindowBorderLayout.addWidget(&MinBrowserButton);
    WindowBorderLayout.addWidget(&MaxBrowserButton);
    WindowBorderLayout.addWidget(&CloseBrowserButton);
    WindowBorder.setLayout(&WindowBorderLayout);




    QAction newtab("New tab", this);
    QAction newwindow("New window", this);
    QAction newprivatewindow("New incognito window", this);

    QAction bookmarks("Bookmarks", this);
    QAction history("History", this);
    QAction reading("Reading list", this);

    QAction savepage("Save Page", this);
    QAction settings("Settings", this);
    QAction about("About Eden Browser", this);
    QAction help("Help", this);

    QAction quit("Quit", this);
    contextMenu.setObjectName("MainMenu");

    QPoint localPoint;
    localPoint.setX(0);
    localPoint.setY(0);

    contextMenu.mapToParent(localPoint);
    contextMenu.setGeometry(0, 0, 300, 500);

    contextMenu.addAction(&newtab);
    contextMenu.addAction(&newwindow);
    contextMenu.addAction(&newprivatewindow);

    contextMenu.addAction(&bookmarks);
    contextMenu.addAction(&history);
    contextMenu.addAction(&reading);
    contextMenu.addAction(&savepage);

    contextMenu.addAction(&settings);
    contextMenu.addAction(&about);
    contextMenu.addAction(&help);

    contextMenu.addAction(&quit);

    QGraphicsDropShadowEffect* effect = new QGraphicsDropShadowEffect();
    effect->setBlurRadius(100);
    qreal *offset = new qreal();

    effect->setOffset(0.00);
    contextMenu.setGraphicsEffect(effect);
    contextMenu.setVisible(false);
    WindowBorderLayout.addWidget(&contextMenu);









    /*
     *
     *      EDEN TOOLBAR
     *          Basically, just the back/forward/addressbar/menu buttons... yeah.
     *
     */

    ToolBar;
    ToolBarLayout;
    ToolBar.setObjectName("TabControls");

    ToolBar.setLayout(&ToolBarLayout);

    QPushButton *BackButton = new QPushButton();
    BackButton->setIcon(QIcon(":/resources/icons/ic_keyboard_arrow_left_black_24px.svg"));

    QPushButton *ForwardButton = new QPushButton();
    ForwardButton->setIcon(QIcon(":/resources/icons/ic_keyboard_arrow_right_black_24px.svg"));


    QPushButton *RefreshButton = new QPushButton();
    RefreshButton->setIcon(QIcon(":/resources/icons/ic_refresh_black_24px.svg"));


    QPushButton *MenuButton = new QPushButton();
    MenuButton->setIcon(QIcon(":/resources/icons/ic_more_vert_black_24px.svg"));

    AddressBar;

    connect(&AddressBar, &EdenAddressBar::returnPressed, this, &BaseApplication::loadPage);

    connect(BackButton, &QPushButton::clicked, this, &BaseApplication::goBack);
    connect(ForwardButton, &QPushButton::clicked, this, &BaseApplication::goForward);
    connect(RefreshButton, &QPushButton::clicked, this, &BaseApplication::refresh);


    // Add Objects to ToolBar
    ToolBarLayout.addWidget(BackButton);
    ToolBarLayout.addWidget(ForwardButton);
    ToolBarLayout.addWidget(RefreshButton);
    ToolBarLayout.addWidget(&AddressBar);
    ToolBarLayout.addWidget(MenuButton);

    connect(MenuButton, &QPushButton::clicked, this, &BaseApplication::ShowMainMenu);


    /*
     *
     *      EDEN MAINVIEW AREA
     *          Just the webview/devtools container
     *
     */

    Container;
    Container.setContentsMargins(no_margins);
    ContainerLayout;

    Container.setLayout(&ContainerLayout);

    this->title = "Eden Browser";
    this->setWindowTitle(this->title);
    this->setWindowFlags(this->windowFlags() | Qt::FramelessWindowHint);

    QSize BaseSize;

    BaseSize.setHeight(768);
    BaseSize.setWidth(1366);
    this->setBaseSize(BaseSize);
    this->setMinimumSize(1366,768);

    layout.addWidget(&WindowBorder);
    layout.addWidget(&ToolBar);
    layout.addWidget(&Container);



    this->setLayout(&layout);
    show();
    this->AddTab(&ContainerLayout, &TabBar);

    connect(&NewTabButton, &QPushButton::clicked, [this](){
        AddTab(&ContainerLayout, &TabBar);
    });

    QShortcut *shortcut = new QShortcut(QKeySequence(tr("Ctrl+T", "New Tab")),
                             this);

    connect(shortcut, &QShortcut::activated, [this](){
        this->AddTab(&ContainerLayout, &TabBar);
    });




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

void BaseApplication::AddTab(QStackedLayout *stack, EdenTabBar *tb){


    Tab *newtab = new Tab(&TabCount, &ContainerLayout);
    Tabs.append(newtab);

    EdenAddressBar *topAddressBar = &AddressBar;

    CurrentTab = Tabs[TabCount];
    QUrl *nu = new QUrl;

    tb->addTab(QString("New Tab"));
    tb->setTabData(TabCount, QVariant(QString("tab" + QString::number(TabCount))));

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
    TabBar.setCurrentIndex(TabCount);
    TabCount++;
}


void BaseApplication::selectCurrentTab(int i){

    QString tab_name = this->TabBar.tabData(i).toString();
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


    //contextMenu.setVisible(true);
}
