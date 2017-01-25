#include "tab.h"
#include <iostream>

Tab::Tab(int *tc, QStackedLayout *c, QWidget *parent) : QWidget(parent)
{

    // Create Objects
    QVBoxLayout *TabLayout = new QVBoxLayout;
    TabLayout->setContentsMargins(QMargins(0,0,0,0));
    TabContent;
    QUrl *TabUrl = new QUrl;

    tabcount = *tc;

    setObjectName(QString("tab") + QString::number(*tc));

    // SetNames
    TabLayout->setObjectName("TabView");

    TabContent.load(TabUrl->fromUserInput("http://google.com"));

    this->setLayout(TabLayout);
    TabLayout->addWidget(&SplitView);
    SplitView.addWidget(&TabContent);

    SplitView.setOrientation(Qt::Vertical);
    //this->show();

    // Load finished
    connect(&TabContent, &QWebEngineView::loadFinished, [this](const bool &ok){
        emit loadFinished(ok);
    });

    // Emit title changed to BaseApplication so that can chage the tabs text
    connect(&TabContent, &QWebEngineView::titleChanged, [this](const QString &title) {

        emit titleChanged(title, objectName());
    });

    // Emit url changed to parent
    connect(&TabContent, &QWebEngineView::urlChanged, [this](const QUrl &url){
        emit urlChanged(url, objectName());
    });

    // Emit icon changed
    connect(&TabContent, &QWebEngineView::iconChanged, [this](const QIcon &icon){
        emit iconChanged(icon, objectName());
    });



}

Tab::~Tab(){

}

void Tab::webTitleChanged(){

}

void Tab::OpenDevTools(){

    DevToolsContainer *devtools = new DevToolsContainer;
    SplitView.addWidget(devtools);


}


void Tab::destroyTab(){
    qDebug() << "destroying " << this->TabContent.title();
    this->destroy();
}
