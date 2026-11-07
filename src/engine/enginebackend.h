#pragma once

#include <QMetaType>

namespace eden::engine {

    enum class Backend { Cef, Wpe, Servo };

}

Q_DECLARE_METATYPE(eden::engine::Backend)
