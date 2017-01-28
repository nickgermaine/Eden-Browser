#ifndef DEVTOOLSCONTAINER_H
#define DEVTOOLSCONTAINER_H

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QIcon>
#include <QtWebEngineWidgets/qwebengineview.h>
#include <QtWebEngineWidgets/qwebenginepage.h>

#include <QWebEngineProfile>

class DevToolsContainer : public QWidget
{
    Q_OBJECT
public:
    explicit DevToolsContainer(QWidget *parent = 0);
    QVBoxLayout DevToolsLayout;
    QWebEngineView DevTools;
    QPushButton DevToolsQuit;

    QWidget DevToolsControls;
    QHBoxLayout DevToolsControlsLayout;


    void PrintDevList();

signals:

public slots:
};

#endif // DEVTOOLSCONTAINER_H
