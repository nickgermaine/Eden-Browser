#include "baseapplication.h"

BaseApplication::BaseApplication(QWidget *parent)
    : QMainWindow(parent)
{
    this->title = "Eden Browser";
    this->setWindowTitle(this->title);
}

BaseApplication::~BaseApplication()
{

}
