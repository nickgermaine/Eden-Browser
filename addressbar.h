#ifndef ADDRESSBAR_H
#define ADDRESSBAR_H
#include <QLineEdit>

class AddressBar : public QLineEdit
{
public:
    AddressBar(QWidget *parent = 0);
    void mousePressEvent();
    QString title;
};

#endif // ADDRESSBAR_H
