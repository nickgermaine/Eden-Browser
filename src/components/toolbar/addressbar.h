#ifndef ADDRESSBAR_H
#define ADDRESSBAR_H
#include <QLineEdit>
#include <QCompleter>
#include <QFocusEvent>
#include <QMouseEvent>
#include <iostream>
#include <QObject>


class EdenAddressBar : public QLineEdit
{

public:
    explicit EdenAddressBar(QLineEdit *parent = 0);

protected:
    void focusInEvent(QFocusEvent *e);
    void mousePressEvent(QMouseEvent *me);

    bool _selectOnMousePress;
};

#endif // ADDRESSBAR_H
