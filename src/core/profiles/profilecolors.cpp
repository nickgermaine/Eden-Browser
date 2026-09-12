#include "core/profiles/profilecolors.h"

#include <QTextBoundaryFinder>

#include <array>
#include <cmath>

namespace eden::core {

    static const std::array<QColor, 12> &profilePalette() {
        static const std::array<QColor, 12> palette = {
            QColor(0x00, 0x5A, 0xC1),
            QColor(0x8E, 0x24, 0xAA),
            QColor(0x00, 0x69, 0x5C),
            QColor(0xB3, 0x26, 0x1E),
            QColor(0x2E, 0x7D, 0x32),
            QColor(0xC7, 0x51, 0x00),
            QColor(0x37, 0x47, 0xA6),
            QColor(0x88, 0x1D, 0x5E),
            QColor(0x00, 0x60, 0x7A),
            QColor(0x5C, 0x3C, 0xC7),
            QColor(0x6D, 0x4C, 0x41),
            QColor(0x37, 0x5A, 0x08),
        };
        return palette;
    }

    QColor profileColorForSeed(quint32 seed) {
        const auto &palette = profilePalette();
        return palette.at(seed % palette.size());
    }

    static double channelLuminance(double value) {
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    }

    static double relativeLuminance(const QColor &color) {
        return 0.2126 * channelLuminance(color.redF()) + 0.7152 * channelLuminance(color.greenF()) +
               0.0722 * channelLuminance(color.blueF());
    }

    static double contrastRatio(const QColor &first, const QColor &second) {
        const double firstLuminance = relativeLuminance(first);
        const double secondLuminance = relativeLuminance(second);
        const double lighter = std::max(firstLuminance, secondLuminance);
        const double darker = std::min(firstLuminance, secondLuminance);
        return (lighter + 0.05) / (darker + 0.05);
    }

    QColor profileForegroundFor(const QColor &background) {
        const QColor white(Qt::white);
        const QColor black(0x1C, 0x1B, 0x1F);
        return contrastRatio(background, white) >= contrastRatio(background, black) ? white : black;
    }

    QString firstGrapheme(const QString &displayName) {
        const QString trimmed = displayName.trimmed();
        if (trimmed.isEmpty()) {
            return {};
        }
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, trimmed);
        finder.setPosition(0);
        const qsizetype end = finder.toNextBoundary();
        if (end <= 0) {
            return trimmed.left(1).toUpper();
        }
        return trimmed.left(end).toUpper();
    }

}
