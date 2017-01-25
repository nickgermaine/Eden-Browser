#include "tabbar.h"
#include <iostream>
#include <QVariant>
#include <QMenu>
#include <QDebug>

EdenTabBar::EdenTabBar(QTabBar *parent) : QTabBar(parent)
{

    this->setObjectName(QString("TabBar"));
    this->setExpanding(false);
    this->setDrawBase(false);
    this->setElideMode(Qt::ElideLeft);
    this->setMovable(true);
    this->setTabsClosable(true);

    this->setContextMenuPolicy(Qt::CustomContextMenu);
    //connect(this, &EdenTabBar::customContextMenuRequested, this, &EdenTabBar::ShowContextMenu);

}

EdenTabBar::~EdenTabBar(){

}

void EdenTabBar::AddTab(QString tabText){
    this->addTab(tabText);
}

void EdenTabBar::updateTitle(const QString &title, const QString &tab_name){
    qDebug() << "actually changing " << tab_name << " to " << title;
    qDebug() << "tabbar length " << this->count();
    for(int i = 0;i <= this->count();i++){
        qDebug() << " current tab is " << tabData(i).toString();
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


void EdenTabBar::ShowContextMenu(const QPoint &pos)
{
    if (pos.isNull()){
            return;
    }

    int tabIndex = this->tabAt(pos);


   QMenu contextMenu(tr("Context menu"), this);

   QAction action1("New tab", this);
   QAction action2("Reload", this);
   QAction action3("Duplicate", this);
   QAction action4("Pin tab", this);
   QAction action5("Mute", this);

   QAction action6("Close tab", this);
   QAction action7("Close other tabs", this);
   QAction action8("Add to bookmarks", this);
   QAction action9("Add to reading list", this);

   connect(&action1, SIGNAL(triggered()), this, SLOT(removeDataPoint()));
   contextMenu.addAction(&action1);
   contextMenu.addSeparator();
   contextMenu.addAction(&action2);
   contextMenu.addAction(&action3);
   contextMenu.addAction(&action4);
   contextMenu.addAction(&action5);
   contextMenu.addSeparator();
   contextMenu.addAction(&action6);
   contextMenu.addAction(&action7);
   contextMenu.addAction(&action8);
   contextMenu.addAction(&action9);


   contextMenu.exec(mapToGlobal(pos));
}


