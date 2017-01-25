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
    ~EdenTabBar();
    void AddTab(QString);
    QTabBar getTabBar();
    QHBoxLayout WindowBorderLayout;
    QTabBar TabBarView;
    QPushButton CloseBrowserButton;
    QPushButton MaxBrowserButton;
    QPushButton MinBrowserButton;

signals:


public slots:
    void updateTitle(const QString &title, const QString &tab_name);
    void updateIcon(const QIcon &icon, const QString &tab_name);
    void ShowContextMenu(const QPoint &pos);

};

#endif // TABBAR_H
