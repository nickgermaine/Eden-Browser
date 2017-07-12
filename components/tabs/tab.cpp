#include "tab.h"
#include <iostream>
#include <QDebug>

Tab::Tab(int *tc, QStackedLayout *c, QString mode, QWidget *parent) : QWidget(parent)
{

    // Create Objects
    QVBoxLayout *TabLayout = new QVBoxLayout;
    TabLayout->setContentsMargins(QMargins(0,0,0,0));
    QUrl *TabUrl = new QUrl;

    tabcount = *tc;

    setObjectName(QString("tab") + QString::number(*tc));

    // SetNames
    TabLayout->setObjectName("TabView");

    if(mode.toStdString() == "private"){
        QWebEngineProfile *pf = new QWebEngineProfile();
        QWebEnginePage *page = new QWebEnginePage(pf, &TabContent);
        TabContent.setPage(page);

        // FINALLY!!
        // I screwed around with this null webengineprofile last month
        // And could NOT get it working...
        // Working now ;)

        qDebug() << " is OTR? " << TabContent.page()->profile()->isOffTheRecord();

    }

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

    connect(devtools, &DevToolsContainer::orientation_switched, [this](const QString &mode){
        if(SplitView.orientation() == Qt::Vertical){
            SplitView.setOrientation(Qt::Horizontal);
        }else{
            SplitView.setOrientation(Qt::Vertical);
        }
        qDebug() << "Mode";
    });

}



void Tab::destroyTab(){

    this->deleteLater();
}
