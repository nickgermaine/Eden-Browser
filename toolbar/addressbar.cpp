#include <iostream>
#include "addressbar.h"

EdenAddressBar::EdenAddressBar(QLineEdit *parent) : QLineEdit(parent)
{
    //connect(this, SIGNAL(mousePressEvent()), this, SLOT(selectAll()));
    //connect(this, &EdenAddressBar::focusInEvent, this, &EdenAddressBar::selectAll);

    _selectOnMousePress = false;
}

void EdenAddressBar::focusInEvent(QFocusEvent *e)
{
QLineEdit::focusInEvent(e);
selectAll();
_selectOnMousePress = true;
}

void EdenAddressBar::mousePressEvent(QMouseEvent *me)
{
QLineEdit::mousePressEvent(me);
if(_selectOnMousePress) {
selectAll();
_selectOnMousePress = false;
}


}
