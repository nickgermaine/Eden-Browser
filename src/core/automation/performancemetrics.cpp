#include "core/automation/performancemetrics.h"

#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QVector>

#include <chrono>

namespace eden::core {

    struct PerformanceSample {
        quint64 sequence;
        QString name;
        double value;
    };

    static QMutex &metricsMutex() {
        static QMutex mutex;
        return mutex;
    }

    static QVector<PerformanceSample> &metricsSamples() {
        static QVector<PerformanceSample> samples;
        return samples;
    }

    static quint64 &metricsSequence() {
        static quint64 sequence = 0;
        return sequence;
    }

    static QHash<QString, std::chrono::steady_clock::time_point> &metricStarts() {
        static QHash<QString, std::chrono::steady_clock::time_point> starts;
        return starts;
    }

    static QSet<QString> &readyMetrics() {
        static QSet<QString> metrics;
        return metrics;
    }

    static bool &collectFrameMetrics() {
        static bool enabled = false;
        return enabled;
    }

    quint64 PerformanceMetrics::record(const QString &name, double value) {
        QMutexLocker lock(&metricsMutex());
        const quint64 sequence = ++metricsSequence();
        QVector<PerformanceSample> &samples = metricsSamples();
        samples.append({sequence, name, value});
        if (samples.size() > 16384) {
            samples.remove(0, samples.size() - 8192);
        }
        return sequence;
    }

    void PerformanceMetrics::begin(const QString &name) {
        QMutexLocker lock(&metricsMutex());
        metricStarts().insert(name, std::chrono::steady_clock::now());
    }

    std::optional<double> PerformanceMetrics::complete(const QString &name) {
        QMutexLocker lock(&metricsMutex());
        auto found = metricStarts().find(name);
        if (found == metricStarts().end()) {
            return std::nullopt;
        }
        const double milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - found.value()).count();
        metricStarts().erase(found);
        const quint64 sequence = ++metricsSequence();
        QVector<PerformanceSample> &samples = metricsSamples();
        samples.append({sequence, name, milliseconds});
        if (samples.size() > 16384) {
            samples.remove(0, samples.size() - 8192);
        }
        return milliseconds;
    }

    void PerformanceMetrics::markReady(const QString &name) {
        QMutexLocker lock(&metricsMutex());
        if (metricStarts().contains(name)) {
            readyMetrics().insert(name);
        }
    }

    std::optional<double> PerformanceMetrics::completeIfReady(const QString &name) {
        QMutexLocker lock(&metricsMutex());
        if (!readyMetrics().remove(name)) {
            return std::nullopt;
        }
        auto found = metricStarts().find(name);
        if (found == metricStarts().end()) {
            return std::nullopt;
        }
        const double milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - found.value()).count();
        metricStarts().erase(found);
        const quint64 sequence = ++metricsSequence();
        QVector<PerformanceSample> &samples = metricsSamples();
        samples.append({sequence, name, milliseconds});
        if (samples.size() > 16384) {
            samples.remove(0, samples.size() - 8192);
        }
        return milliseconds;
    }

    void PerformanceMetrics::cancel(const QString &name) {
        QMutexLocker lock(&metricsMutex());
        metricStarts().remove(name);
        readyMetrics().remove(name);
    }

    void PerformanceMetrics::setFrameCollectionEnabled(bool enabled) {
        QMutexLocker lock(&metricsMutex());
        collectFrameMetrics() = enabled;
    }

    bool PerformanceMetrics::frameCollectionEnabled() {
        QMutexLocker lock(&metricsMutex());
        return collectFrameMetrics();
    }

    quint64 PerformanceMetrics::sequence() {
        QMutexLocker lock(&metricsMutex());
        return metricsSequence();
    }

    QJsonArray PerformanceMetrics::samplesAfter(quint64 sequence) {
        QMutexLocker lock(&metricsMutex());
        QJsonArray result;
        for (const PerformanceSample &sample : std::as_const(metricsSamples())) {
            if (sample.sequence <= sequence) {
                continue;
            }
            QJsonObject value;
            value.insert("sequence", static_cast<qint64>(sample.sequence));
            value.insert("name", sample.name);
            value.insert("value", sample.value);
            result.append(value);
        }
        return result;
    }

    void PerformanceMetrics::clear() {
        QMutexLocker lock(&metricsMutex());
        metricsSamples().clear();
        metricStarts().clear();
        readyMetrics().clear();
        collectFrameMetrics() = false;
        metricsSequence() = 0;
    }

}
