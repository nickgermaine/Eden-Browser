#pragma once

#include <QJsonArray>
#include <QString>

#include <optional>

namespace eden::core {

    class PerformanceMetrics {
      public:
        static quint64 record(const QString &name, double value);
        static void begin(const QString &name);
        static std::optional<double> complete(const QString &name);
        static void markReady(const QString &name);
        static std::optional<double> completeIfReady(const QString &name);
        static void cancel(const QString &name);
        static void setFrameCollectionEnabled(bool enabled);
        static bool frameCollectionEnabled();
        static quint64 sequence();
        static QJsonArray samplesAfter(quint64 sequence);
        static void clear();
    };

}
