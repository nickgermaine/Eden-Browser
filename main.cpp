#include "baseapplication.h"
#include <QApplication>
#include <QFile>
#include <QWebEngineProfile>
#include "urlintercepter.h"


int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
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


    RequestInterceptor *ads = new RequestInterceptor;

        QWebEngineProfile::defaultProfile()->setRequestInterceptor(ads);

    return a.exec();
}
