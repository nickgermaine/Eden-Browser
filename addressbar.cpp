#include "addressbar.h"

AddressBar::AddressBar(QWidget *parent)
    : QLineEdit(parent)
{

}

void AddressBar::mousePressEvent(){
    this->selectAll();
}
