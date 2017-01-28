/*****************************************************
 *
 * This is the core of the application.
 *
 * As soon as the app is initialized, this class is instantiated
 * and creates its initial browser window in normal mode.
 *
 * That window can trigger new windows (normal and private) being created
 * which calls this createWindow() function again.
 *
 * */

#include "core.h"
#include <QDebug>
#include <QFrame>


Core::Core(QWidget *parent, QString mode) : QWidget(parent)
{
    // Always start with one window.
    createWindow();

}

void Core::createWindow(QString mode){
    // Open the material and material-dark stylesheets
    QFile File(":/resources/stylesheets/material.css");
    File.open(QFile::ReadOnly);
    QString StyleSheet = QLatin1String(File.readAll());

    QFile incognito(":/resources/stylesheets/material-dark.css");
    incognito.open(QFile::ReadOnly);
    QString PrivateStylesheet = QLatin1String(incognito.readAll());

    // If a normal window is requested, open in normal mode, otherwise, open in Private/dark mode.
    if(mode == QString("normal")){
        BaseApplication *window = new BaseApplication(QString("normal"));
        window->setStyleSheet(StyleSheet);
        connect(window, &BaseApplication::createNewWindow, [this](QString mode){
            qDebug() << "requesting " << mode;
            createWindow(mode);
        });

    }else{
        BaseApplication *window = new BaseApplication(QString("private"));
        window->setStyleSheet(PrivateStylesheet);
        connect(window, &BaseApplication::createNewWindow, [this](QString mode){
            qDebug() << "requesting " << mode;
            createWindow(mode);
        });

    }
}
