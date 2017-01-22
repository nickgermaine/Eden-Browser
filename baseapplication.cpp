#include "baseapplication.h"
#include <QSize>
#include <QWidget>
#include <iostream>
#include <QApplication>
#include <QDesktopWidget>

BaseApplication::BaseApplication(QFrame *parent)
    : QFrame(parent)
{

    this->CreateWindow();
    this->center();


}

BaseApplication::~BaseApplication()
{

}

void BaseApplication::CreateWindow(){
    // No Margin Margins()
    QMargins no_margins;
    no_margins.setBottom(0);
    no_margins.setLeft(0);
    no_margins.setRight(0);
    no_margins.setTop(0);

    // Create Main Window
    QWidget *window = this;



    /*
     *
     *      EDEN WINDOWBORDER
     *          Includes the TabBar, min, max, and close buttons.
     *
     */


    QWidget *WindowBorder = new QWidget;
    QHBoxLayout *WindowBorderLayout = new QHBoxLayout;

    WindowBorderLayout->setContentsMargins(no_margins);
    WindowBorderLayout->setSpacing(0);

    EdenTabBar *TabBar = new EdenTabBar;
    QVBoxLayout *layout = new QVBoxLayout;

    QList<QWidget*> tabs;


    layout->setContentsMargins(no_margins);
    layout->setSpacing(0);
    this->setObjectName("BrowserWindow");

    // Close/Max/Min Buttons
    QPushButton *CloseBrowserButton = new QPushButton();
    CloseBrowserButton->setIcon(QIcon(":/resources/icons/ic_close_black_24px.svg"));
    CloseBrowserButton->setFixedWidth(48);

    QPushButton *MaxBrowserButton = new QPushButton();
    MaxBrowserButton->setIcon(QIcon(":/resources/icons/ic_aspect_ratio_black_24px.svg"));
    MaxBrowserButton->setFixedWidth(48);

    QPushButton *MinBrowserButton = new QPushButton();
    MinBrowserButton->setIcon(QIcon(":/resources/icons/ic_remove_black_24px.svg"));
    MinBrowserButton->setFixedWidth(48);

    // Connect Window Buttons
    QObject::connect(CloseBrowserButton, SIGNAL(clicked()), QApplication::instance(), SLOT(quit()));

    TabBar->setObjectName(QString("TabBar"));
    int TabCount = 0;


    WindowBorderLayout->addWidget(TabBar);
    WindowBorderLayout->addStretch();
    WindowBorderLayout->addWidget(MinBrowserButton);
    WindowBorderLayout->addWidget(MaxBrowserButton);
    WindowBorderLayout->addWidget(CloseBrowserButton);
    WindowBorder->setLayout(WindowBorderLayout);










    /*
     *
     *      EDEN TOOLBAR
     *          Basically, just the back/forward/addressbar/menu buttons... yeah.
     *
     */

    QWidget *ToolBar = new QWidget;
    QHBoxLayout *ToolBarLayout = new QHBoxLayout;
    ToolBar->setObjectName("TabControls");

    ToolBar->setLayout(ToolBarLayout);

    QPushButton *BackButton = new QPushButton();
    BackButton->setIcon(QIcon(":/resources/icons/ic_keyboard_arrow_left_black_24px.svg"));

    QPushButton *ForwardButton = new QPushButton();
    ForwardButton->setIcon(QIcon(":/resources/icons/ic_keyboard_arrow_right_black_24px.svg"));


    QPushButton *RefreshButton = new QPushButton();
    RefreshButton->setIcon(QIcon(":/resources/icons/ic_refresh_black_24px.svg"));


    QPushButton *MenuButton = new QPushButton();
    MenuButton->setIcon(QIcon(":/resources/icons/ic_more_vert_black_24px.svg"));

    EdenAddressBar *AddressBar = new EdenAddressBar();

    // Add Objects to ToolBar
    ToolBarLayout->addWidget(BackButton);
    ToolBarLayout->addWidget(ForwardButton);
    ToolBarLayout->addWidget(RefreshButton);
    ToolBarLayout->addWidget(AddressBar);
    ToolBarLayout->addWidget(MenuButton);


    /*
     *
     *      EDEN MAINVIEW AREA
     *          Just the webview/devtools container
     *
     */

    QWidget *Container = new QWidget();
    Container->setContentsMargins(no_margins);
    QStackedLayout *ContainerLayout = new QStackedLayout();

    Container->setLayout(ContainerLayout);

    this->title = "Eden Browser";
    this->setWindowTitle(this->title);
    this->setWindowFlags(Qt::FramelessWindowHint);

    QSize BaseSize;

    BaseSize.setHeight(768);
    BaseSize.setWidth(1366);
    this->setBaseSize(BaseSize);
    this->setMinimumSize(1366,768);

    layout->addWidget(WindowBorder);
    layout->addWidget(ToolBar);
    layout->addWidget(Container);


    this->setLayout(layout);
    show();
    this->AddTab(ContainerLayout, TabBar);



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

    Tab *newtab = new Tab(&TabCount, stack);
    Tabs.append(newtab);

    QObject::connect(newtab, &Tab::titleChanged, this, &BaseApplication::AddTabContent);


    std::cout << "counting" << ContainerLayout.count() << std::endl;
    TabCount++;
}

void BaseApplication::AddTabContent(){
    std::cout << "adding tab content" << std::endl;
}
