#include "baseapplication.h"
#include <QApplication>
#include <QFile>
#include <QWebEngineProfile>
#include "urlintercepter.h"
#include <QWebEngineSettings>




int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    qputenv("QTWEBENGINE_REMOTE_DEBUGGING", "0.0.0.0:667");
    BaseApplication w;

    /*
    QFile *stylesheet = new QFile(QString(":/resources/stylesheets/material.css"));

    if(stylesheet->open(QFile::ReadWrite)){
        a.setStyleSheet(stylesheet->readAll());
    }
    */

    QFile File(":/resources/stylesheets/material.css");
    File.open(QFile::ReadOnly);
    QString StyleSheet = QLatin1String(File.readAll());

    a.setStyleSheet(StyleSheet);




    //RequestInterceptor *ads = new RequestInterceptor;

    //QWebEngineProfile::defaultProfile()->setRequestInterceptor(ads);

    return a.exec();
}
