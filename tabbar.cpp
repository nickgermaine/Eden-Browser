#include "tabbar.h"

EdenTabBar::EdenTabBar(QTabBar *parent) : QTabBar(parent)
{

    this->setObjectName(QString("TabBar"));
    this->setExpanding(false);
    this->setDrawBase(false);
    this->setElideMode(Qt::ElideLeft);
    this->setMovable(true);
    this->setTabsClosable(true);

    this->addTab(QString("this"));



}

void EdenTabBar::AddTab(QString tabText){
    this->addTab(tabText);
}

