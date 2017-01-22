#ifndef TABBAR_H
#define TABBAR_H

#include <QWidget>
#include <QHBoxLayout>
#include <QTabBar>
#include <QPushButton>
#include <QIcon>

class EdenTabBar : public QTabBar
{
    Q_OBJECT
public:
    explicit EdenTabBar(QTabBar *parent = 0);
    void AddTab(QString);
    QTabBar getTabBar();
    QHBoxLayout WindowBorderLayout;
    QTabBar TabBarView;
    QPushButton CloseBrowserButton;
    QPushButton MaxBrowserButton;
    QPushButton MinBrowserButton;

signals:

public slots:
};

#endif // TABBAR_H
