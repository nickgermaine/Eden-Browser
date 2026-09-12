#pragma once

#include "core/profiles/profileerror.h"

#include <QString>
#include <QUrl>

#include <functional>

class QObject;

namespace eden::core {

    class AvatarProcessor {
      public:
        static constexpr qint64 maximumSourceBytes = 20 * 1024 * 1024;
        static constexpr int maximumSourceDimension = 8192;
        static constexpr int normalizedDimension = 512;

        using Callback = std::function<void(ProfileError)>;

        static ProfileError processBlocking(const QUrl &sourceUrl, const QString &targetPath);
        static void process(const QUrl &sourceUrl, const QString &targetPath, QObject *receiver, Callback callback);
    };

}
