#include "tabbar.h"
#include <iostream>
#include <QVariant>

EdenTabBar::EdenTabBar(QTabBar *parent) : QTabBar(parent)
{

    this->setObjectName(QString("TabBar"));
    this->setExpanding(false);
    this->setDrawBase(false);
    this->setElideMode(Qt::ElideLeft);
    this->setMovable(true);
    this->setTabsClosable(true);


}

EdenTabBar::~EdenTabBar(){

}

void EdenTabBar::AddTab(QString tabText){
    this->addTab(tabText);
}

void EdenTabBar::updateTitle(const QString &title, const QString &tab_name){

    for(int i = 0;i <= this->count();i++){
        std::cout << tabData(i).toString().toStdString() << std::endl;
        if(tabData(i).toString() == tab_name){
            this->setTabText(i, title);
        }
    }
    //setTabText(tabcount, title);


}

void EdenTabBar::updateIcon(const QIcon &icon, const QString &tab_name){
    for(int i = 0;i <= this->count();i++){
        std::cout << tabData(i).toString().toStdString() << std::endl;
        if(tabData(i).toString() == tab_name){
            this->setTabIcon(i, icon);
        }
    }
}
