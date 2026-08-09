#include "components/base/baseapplication.h"
#include <QApplication>
#include <QFile>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include "components/core/core.h"



int main(int argc, char *argv[])
{
    qputenv("QTWEBENGINE_REMOTE_DEBUGGING", "127.0.0.1:9222");
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--enable-smooth-scrolling");

    QApplication a(argc, argv);
    a.setQuitOnLastWindowClosed(true);

    QWebEngineProfile::defaultProfile()->settings()->setAttribute(QWebEngineSettings::ScrollAnimatorEnabled, true);

    Core *w = new Core();

    return a.exec();
}
