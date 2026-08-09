#ifndef CORE_H
#define CORE_H

#include <QWidget>
#include "components/base/baseapplication.h"

class Core : public QWidget
{
    Q_OBJECT
public:
    explicit Core(QWidget *parent = 0, QString mode = QString("normal"));
    void createWindow(QString mode = QString("normal"));
signals:

public slots:
};

#endif // CORE_H
