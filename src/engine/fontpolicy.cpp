#include <gio/gio.h>

#include "engine/fontpolicy.h"

#include <QByteArray>
#include <QFont>
#include <QGuiApplication>
#include <QHash>
#include <QMutexLocker>
#include <QRecursiveMutex>
#include <QStringList>

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <optional>

namespace eden::engine {

    namespace {

        QRecursiveMutex &policyMutex() {
            static QRecursiveMutex mutex;
            return mutex;
        }

        QHash<QString, QString> &familyCache() {
            static QHash<QString, QString> cache;
            return cache;
        }

        std::optional<QString> &systemDescriptionCache() {
            static std::optional<QString> cache;
            return cache;
        }

        std::optional<bool> &antialiasingCache() {
            static std::optional<bool> cache;
            return cache;
        }

        QString desktopInterfaceSetting(const char *key) {
            GSettingsSchemaSource *source = g_settings_schema_source_get_default();
            GSettingsSchema *schema =
                source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE) : nullptr;
            if (!schema) {
                return {};
            }
            QString value;
            if (g_settings_schema_has_key(schema, key)) {
                GSettings *settings = g_settings_new("org.gnome.desktop.interface");
                gchar *text = g_settings_get_string(settings, key);
                if (text && *text) {
                    value = QString::fromUtf8(text);
                }
                g_free(text);
                g_object_unref(settings);
            }
            g_settings_schema_unref(schema);
            return value;
        }

        const QStringList &styleKeywords() {
            static const QStringList keywords = {
                "thin",
                "ultra-light",
                "extra-light",
                "light",
                "semi-light",
                "book",
                "regular",
                "roman",
                "medium",
                "semi-bold",
                "demi-bold",
                "bold",
                "ultra-bold",
                "extra-bold",
                "heavy",
                "black",
                "italic",
                "oblique",
                "ultra-condensed",
                "extra-condensed",
                "condensed",
                "semi-condensed",
                "semi-expanded",
                "expanded",
                "extra-expanded",
                "ultra-expanded"
            };
            return keywords;
        }

    }

    QString FontPolicy::familyFromDescription(const QString &description) {
        QStringList tokens = description.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        while (tokens.size() > 1) {
            bool numeric = false;
            tokens.constLast().toDouble(&numeric);
            if (!numeric && !styleKeywords().contains(tokens.constLast().toLower())) {
                break;
            }
            tokens.removeLast();
        }
        if (tokens.size() == 1) {
            bool numeric = false;
            tokens.constFirst().toDouble(&numeric);
            if (numeric || styleKeywords().contains(tokens.constFirst().toLower())) {
                return {};
            }
        }
        return tokens.join(QLatin1Char(' '));
    }

    QString FontPolicy::resolveGenericFamily(const QString &generic) {
        if (generic.isEmpty()) {
            return {};
        }
        const QMutexLocker locker(&policyMutex());
        const auto cached = familyCache().constFind(generic);
        if (cached != familyCache().constEnd()) {
            return *cached;
        }
        QString resolved;
        if (FcInit()) {
            const QByteArray request = generic.toUtf8();
            FcPattern *pattern = FcNameParse(reinterpret_cast<const FcChar8 *>(request.constData()));
            if (pattern) {
                if (FcConfigSubstitute(nullptr, pattern, FcMatchPattern)) {
                    FcDefaultSubstitute(pattern);
                    FcResult result = FcResultNoMatch;
                    FcPattern *match = FcFontMatch(nullptr, pattern, &result);
                    if (match) {
                        FcChar8 *family = nullptr;
                        if (FcPatternGetString(match, FC_FAMILY, 0, &family) == FcResultMatch && family) {
                            resolved = QString::fromUtf8(reinterpret_cast<const char *>(family));
                        }
                        FcPatternDestroy(match);
                    }
                }
                FcPatternDestroy(pattern);
            }
        }
        if (resolved.isEmpty()) {
            resolved = generic;
        }
        familyCache().insert(generic, resolved);
        return resolved;
    }

    QString FontPolicy::systemFamily() {
        const QString described = familyFromDescription(systemDescription());
        return described.isEmpty() ? sansSerifFamily() : described;
    }

    QString FontPolicy::systemDescription() {
        const QMutexLocker locker(&policyMutex());
        if (systemDescriptionCache()) {
            return *systemDescriptionCache();
        }
        QString description = desktopInterfaceSetting("font-name").simplified();
        if (familyFromDescription(description).isEmpty()) {
            description.clear();
            if (QGuiApplication::instance()) {
                const QFont font = QGuiApplication::font();
                if (!font.family().isEmpty()) {
                    description =
                        font.family() + QLatin1Char(' ') + QString::number(std::max(1, font.pointSize()));
                }
            }
            if (description.isEmpty()) {
                description = sansSerifFamily() + QStringLiteral(" 11");
            }
        }
        systemDescriptionCache() = description;
        return description;
    }

    QString FontPolicy::standardFamily() {
        return serifFamily();
    }

    QString FontPolicy::sansSerifFamily() {
        return resolveGenericFamily(QStringLiteral("sans-serif"));
    }

    QString FontPolicy::serifFamily() {
        return resolveGenericFamily(QStringLiteral("serif"));
    }

    QString FontPolicy::monospaceFamily() {
        return resolveGenericFamily(QStringLiteral("monospace"));
    }

    QString FontPolicy::cursiveFamily() {
        return resolveGenericFamily(QStringLiteral("cursive"));
    }

    QString FontPolicy::fantasyFamily() {
        return resolveGenericFamily(QStringLiteral("fantasy"));
    }

    bool FontPolicy::antialiasing() {
        const QMutexLocker locker(&policyMutex());
        if (!antialiasingCache()) {
            antialiasingCache() = desktopInterfaceSetting("font-antialiasing") != QLatin1String("none");
        }
        return *antialiasingCache();
    }

    bool FontPolicy::subpixelAntialiasing() {
        return false;
    }

    FontHinting FontPolicy::hinting() {
        return FontHinting::Slight;
    }

    void FontPolicy::resetCache() {
        const QMutexLocker locker(&policyMutex());
        familyCache().clear();
        systemDescriptionCache().reset();
        antialiasingCache().reset();
    }

}
