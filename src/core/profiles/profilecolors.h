#pragma once

#include <QColor>

namespace eden::core {

    QColor profileColorForSeed(quint32 seed);
    QColor profileForegroundFor(const QColor &background);
    QString firstGrapheme(const QString &displayName);

}
