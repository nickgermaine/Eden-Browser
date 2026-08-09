#ifndef DEVTOOLSCONTAINER_H
#define DEVTOOLSCONTAINER_H

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QIcon>
#include <QWebEngineView>
#include <QWebEnginePage>

#include <QWebEngineProfile>

class DevToolsContainer : public QWidget
{
    Q_OBJECT
public:
    explicit DevToolsContainer(QWidget *parent = 0);
    QVBoxLayout DevToolsLayout;
    QWebEngineView DevTools;
    QPushButton DevToolsQuit;
    QPushButton DevToolsOrient;

    QWidget DevToolsControls;
    QHBoxLayout DevToolsControlsLayout;


    void PrintDevList();

signals:
    void orientation_switched(const QString &mode);

public slots:
};

#endif // DEVTOOLSCONTAINER_H
