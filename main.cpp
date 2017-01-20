#include "baseapplication.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    BaseApplication w;
    w.show();

    return a.exec();
}
