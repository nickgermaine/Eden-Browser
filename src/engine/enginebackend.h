#pragma once

#include <QMetaType>

namespace eden::engine {

    enum class Backend { QtWebEngine, Cef, Wpe, Servo };

}

Q_DECLARE_METATYPE(eden::engine::Backend)
