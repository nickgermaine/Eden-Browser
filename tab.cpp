#include "tab.h"
#include <iostream>

Tab::Tab(int *tc, QStackedLayout *c, QWidget *parent) : QWidget(parent)
{

    std::cout << *tc << " counted" << std::endl;

    // Create Objects
    QVBoxLayout *TabLayout = new QVBoxLayout;
    QWebEngineView *TabContent = new QWebEngineView;
    QUrl *TabUrl = new QUrl;

    // SetNames
    TabLayout->setObjectName("TabView");

    TabContent->load(TabUrl->fromUserInput("http://google.com"));

    this->setLayout(TabLayout);
    TabLayout->addWidget(TabContent);
    this->show();

    c->addWidget(this);
    c->setCurrentWidget(this);

    emit Tab::titleChanged();
}

void Tab::webTitleChanged(){
    emit titleChanged();
}
