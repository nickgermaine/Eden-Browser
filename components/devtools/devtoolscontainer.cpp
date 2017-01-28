#include "devtoolscontainer.h"
#include <QDebug>
#include "components/json/json.hpp"

// for convenience
using json = nlohmann::json;

DevToolsContainer::DevToolsContainer(QWidget *parent) : QWidget(parent)
{
    QUrl *devtoolsurl = new QUrl;
    DevTools.load(devtoolsurl->fromUserInput(QString("http://localhost:667/")));

    DevToolsQuit.setIcon(QIcon(QString(":/resources/icons/ic_clear_black_24px.svg")));
    DevToolsQuit.setFixedHeight(24);
    DevToolsQuit.setFixedWidth(24);

    DevToolsControlsLayout.setContentsMargins(QMargins(0,0,0,0));
    DevToolsControlsLayout.setSpacing(0);

    DevToolsControls.setLayout(&DevToolsControlsLayout);
    DevToolsControlsLayout.addStretch();
    DevToolsControlsLayout.addWidget(&DevToolsQuit);

    DevToolsLayout.addWidget(&DevToolsControls);
    DevToolsLayout.addWidget(&DevTools);
    DevToolsLayout.setContentsMargins(QMargins(0,0,0,0));
    setLayout(&DevToolsLayout);
    DevToolsLayout.setSpacing(0);

    connect(&DevToolsQuit, &QPushButton::clicked, [this](){
        qDebug() << "destroying " << this;
        this->setParent(0);
        this->destroy();
    });

    connect(&DevTools, &QWebEngineView::loadFinished, [this](){

        this->PrintDevList();
    });
}

void DevToolsContainer::PrintDevList(){
    qDebug() << this->DevTools.page();

}


