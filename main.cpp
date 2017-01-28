#include "components/base/baseapplication.h"
#include <QApplication>
#include <QFile>
#include <QWebEngineProfile>
#include "components/networkmanager/urlintercepter.h"
#include <QWebEngineSettings>
#include "components/core/core.h"



int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setQuitOnLastWindowClosed(true);
    qputenv("QTWEBENGINE_REMOTE_DEBUGGING", "0.0.0.0:667");

    Core *w = new Core();



    /*
    QFile *stylesheet = new QFile(QString(":/resources/stylesheets/material.css"));

    if(stylesheet->open(QFile::ReadWrite)){
        a.setStyleSheet(stylesheet->readAll());
    }
    */





    //RequestInterceptor *ads = new RequestInterceptor;

    //QWebEngineProfile::defaultProfile()->setRequestInterceptor(ads);

    return a.exec();
}
