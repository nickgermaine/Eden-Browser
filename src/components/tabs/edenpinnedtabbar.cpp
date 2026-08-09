#include "edenpinnedtabbar.h"

EdenPinnedTabBar::EdenPinnedTabBar(QWidget *parent) : QWidget(parent)
{

    setLayout(&PinnedTabsLayout);


    PinnedTabsLayout.setContentsMargins(0,0,0,0);

    PinnedTabBar.setObjectName(QString("PinnedTabBar"));
    PinnedTabBar.setExpanding(false);
    PinnedTabBar.setDrawBase(false);
    PinnedTabBar.setElideMode(Qt::ElideLeft);
    PinnedTabBar.setMovable(true);
    PinnedTabBar.setTabsClosable(false);
    setBaseSize(24, 24);


    PinnedTabBar.setContextMenuPolicy(Qt::CustomContextMenu);

    PinnedTabsLayout.addWidget(&PinnedTabBar);

}
