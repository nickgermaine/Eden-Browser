#include "components/base/baseapplication.h"
#include <QApplication>
#include <QFile>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include "components/core/core.h"



int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setQuitOnLastWindowClosed(true);
    qputenv("QTWEBENGINE_REMOTE_DEBUGGING", "0.0.0.0:667");

    Core *w = new Core();

    return a.exec();
}
