#ifndef ADDRESSBAR_H
#define ADDRESSBAR_H
#include <QLineEdit>
#include <QCompleter>
#include <QFocusEvent>
#include <QMouseEvent>
#include <iostream>


class EdenAddressBar : public QLineEdit
{
public:
    explicit EdenAddressBar(QLineEdit *parent = 0);

protected:
    void focusInEvent(QFocusEvent *e){
        QLineEdit::focusInEvent(e);
    }
};

#endif // ADDRESSBAR_H
