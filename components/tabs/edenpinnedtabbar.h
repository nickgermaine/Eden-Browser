#ifndef EDENPINNEDTABBAR_H
#define EDENPINNEDTABBAR_H

#include <QWidget>
#include <QTabBar>
#include <QPushButton>
#include <QHBoxLayout>

class EdenPinnedTabBar : public QWidget
{
    Q_OBJECT
public:
    explicit EdenPinnedTabBar(QWidget *parent = 0);

    void AddTab(QString);
    QHBoxLayout PinnedTabsLayout;
    QTabBar PinnedTabBar;


signals:


public slots:



};

#endif // EDENPINNEDTABBAR_H
