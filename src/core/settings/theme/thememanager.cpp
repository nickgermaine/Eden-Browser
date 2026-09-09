#include "core/settings/theme/thememanager.h"
#include "core/settings/settingsstore.h"
#include "core/settings/theme/themeeditormodel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <limits>

namespace eden::core {

    ThemeManager::ThemeManager(QObject *parent)
        : QAbstractListModel(parent) {
        QFile defaultFile(":/qt/qml/Eden/Ui/resources/themes/eden-default.json");
        if (!defaultFile.open(QIODevice::ReadOnly)) {
            qFatal("The built-in Eden theme is unavailable");
        }
        QString error;
        std::optional<ThemeDefinition> theme = parseTheme(defaultFile.readAll(), defaultFile.fileName(), true, error);
        if (!theme) {
            qFatal("The built-in Eden theme is invalid: %s", qPrintable(error));
        }
        m_themes.append(std::move(*theme));
        m_editorModel = new ThemeEditorModel(this);
        SettingsStore *settings = SettingsStore::instance();
        connect(settings, &SettingsStore::themeChanged, this, &ThemeManager::themeChanged);
        connect(settings, &SettingsStore::systemDarkChanged, this, &ThemeManager::themeChanged);
        connect(settings, &SettingsStore::themeIdChanged, this, [this] { selectConfiguredTheme(); });
    }

    ThemeManager::~ThemeManager() = default;

    ThemeManager *ThemeManager::instance() {
        static ThemeManager manager;
        return &manager;
    }

    int ThemeManager::rowCount(const QModelIndex &parent) const {
        return parent.isValid() ? 0 : m_themes.size();
    }

    QVariant ThemeManager::data(const QModelIndex &index, int role) const {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_themes.size()) {
            return {};
        }
        const ThemeDefinition &theme = m_themes.at(index.row());
        if (role == IdRole) {
            return theme.id;
        }
        if (role == NameRole) {
            return theme.name;
        }
        if (role == AuthorRole) {
            return theme.author;
        }
        if (role == BuiltInRole) {
            return theme.builtIn;
        }
        if (role == ActiveRole) {
            return index.row() == m_activeIndex;
        }
        return {};
    }

    QHash<int, QByteArray> ThemeManager::roleNames() const {
        return {
            {IdRole, "themeId"},
            {NameRole, "themeName"},
            {AuthorRole, "themeAuthor"},
            {BuiltInRole, "builtIn"},
            {ActiveRole, "active"}
        };
    }

    QString ThemeManager::activeThemeId() const {
        return activeTheme().id;
    }

    QString ThemeManager::themeDirectory() const {
        return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/eden/themes";
    }

    QString ThemeManager::lastError() const {
        return m_lastError;
    }

    QAbstractItemModel *ThemeManager::editorTokens() const {
        return m_editorModel;
    }

    bool ThemeManager::editing() const {
        return m_editing;
    }

    bool ThemeManager::editorBuiltIn() const {
        return m_editorBuiltIn;
    }

    bool ThemeManager::editorDirty() const {
        return m_editorDirty;
    }

    QString ThemeManager::editorSourceId() const {
        return m_editorSourceId;
    }

    QString ThemeManager::editorId() const {
        return m_editorRoot.value("id").toString();
    }

    QString ThemeManager::editorName() const {
        return m_editorRoot.value("name").toString();
    }

    QString ThemeManager::editorAuthor() const {
        return m_editorRoot.value("author").toString();
    }

    void ThemeManager::setEditorId(const QString &id) {
        setEditorIdentity("id", id);
    }

    void ThemeManager::setEditorName(const QString &name) {
        setEditorIdentity("name", name);
    }

    void ThemeManager::setEditorAuthor(const QString &author) {
        setEditorIdentity("author", author);
    }

    QColor ThemeManager::primary() const {
        return paletteColor("primary");
    }

    QColor ThemeManager::primaryText() const {
        return paletteColor("onPrimary");
    }

    QColor ThemeManager::primaryContainer() const {
        return paletteColor("primaryContainer");
    }

    QColor ThemeManager::primaryContainerText() const {
        return paletteColor("onPrimaryContainer");
    }

    QColor ThemeManager::surface() const {
        return paletteColor("background");
    }

    QColor ThemeManager::surfaceContainerLow() const {
        return toolbarGradientStart();
    }

    bool ThemeManager::toolbarGradientEnabled() const {
        return schemeValue("chrome", "mode").toString() == "linear";
    }

    qreal ThemeManager::toolbarGradientAngle() const {
        return std::clamp(schemeValue("chrome", "angle").toDouble(90), 0.0, 360.0);
    }

    QColor ThemeManager::toolbarGradientStart() const {
        return gradientColor("startColor");
    }

    QColor ThemeManager::toolbarGradientMiddle() const {
        return gradientColor("middleColor");
    }

    QColor ThemeManager::toolbarGradientEnd() const {
        return gradientColor("endColor");
    }

    qreal ThemeManager::toolbarGradientMiddlePosition() const {
        return std::clamp(schemeValue("chrome", "middlePosition").toDouble(0.5), 0.0, 1.0);
    }

    QColor ThemeManager::tabBarBackground() const {
        const QColor color = paletteColor("tabBarBackground");
        return color.isValid() ? color : QColor(Qt::transparent);
    }

    QColor ThemeManager::surfaceContainer() const {
        return paletteColor("panelBackground");
    }

    QColor ThemeManager::surfaceContainerHigh() const {
        return paletteColor("hoverBackground");
    }

    QColor ThemeManager::surfaceContainerHighest() const {
        return paletteColor("activeBackground");
    }

    QColor ThemeManager::surfaceText() const {
        return paletteColor("foreground");
    }

    QColor ThemeManager::surfaceVariantText() const {
        return paletteColor("mutedForeground");
    }

    QColor ThemeManager::disabledText() const {
        return paletteColor("disabledForeground");
    }

    QColor ThemeManager::outline() const {
        return paletteColor("outline");
    }

    QColor ThemeManager::windowBorder() const {
        return paletteColor("windowBorder");
    }

    QColor ThemeManager::contentBorder() const {
        return paletteColor("contentBorder");
    }

    QColor ThemeManager::paneBorder() const {
        return paletteColor("paneBorder");
    }

    QColor ThemeManager::menuBorder() const {
        return paletteColor("menuBorder");
    }

    QColor ThemeManager::focusBorder() const {
        return paletteColor("focusBorder");
    }

    QColor ThemeManager::error() const {
        return paletteColor("error");
    }

    QColor ThemeManager::privatePrimary() const {
        return paletteColor("privatePrimary");
    }

    QColor ThemeManager::privateBackground() const {
        return paletteColor("privateBackground");
    }

    QColor ThemeManager::completionHint() const {
        return paletteColor("completionHint");
    }

    QColor ThemeManager::iconColor() const {
        return paletteColor("icon");
    }

    QColor ThemeManager::disabledIconColor() const {
        return iconColor();
    }

    QColor ThemeManager::windowShadowColor() const {
        return shadowColor();
    }

    QColor ThemeManager::overlayShadowColor() const {
        return colorValue(schemeValue("shadow", "overlayColor"));
    }

    int ThemeManager::windowRadius() const {
        return integerToken("metrics", "windowRadius", 0, 48);
    }

    int ThemeManager::contentRadius() const {
        return integerToken("metrics", "contentRadius", 0, 48);
    }

    int ThemeManager::cardRadius() const {
        return integerToken("metrics", "cardRadius", 0, 48);
    }

    int ThemeManager::controlRadius() const {
        return integerToken("metrics", "controlRadius", 0, 48);
    }

    int ThemeManager::menuRadius() const {
        return integerToken("metrics", "menuRadius", 0, 48);
    }

    int ThemeManager::suggestionRadius() const {
        return menuRadius();
    }

    int ThemeManager::workspaceInset() const {
        return integerToken("metrics", "workspaceInset", 0, 48);
    }

    int ThemeManager::workspaceGap() const {
        return integerToken("metrics", "workspaceGap", 0, 48);
    }

    int ThemeManager::tabMinimumWidth() const {
        return integerToken("metrics", "tabMinimumWidth", 64, 240);
    }

    int ThemeManager::tabMaximumWidth() const {
        return integerToken("metrics", "tabMaximumWidth", 96, 400);
    }

    int ThemeManager::pinnedTabWidth() const {
        return integerToken("metrics", "pinnedTabWidth", 36, 96);
    }

    int ThemeManager::windowBorderWidth() const {
        return integerToken("metrics", "windowBorderWidth", 0, 8);
    }

    int ThemeManager::contentBorderWidth() const {
        return integerToken("metrics", "contentBorderWidth", 0, 8);
    }

    int ThemeManager::paneBorderWidth() const {
        return integerToken("metrics", "paneBorderWidth", 0, 8);
    }

    int ThemeManager::menuBorderWidth() const {
        return integerToken("metrics", "menuBorderWidth", 0, 8);
    }

    int ThemeManager::focusBorderWidth() const {
        return integerToken("metrics", "focusBorderWidth", 0, 8);
    }

    int ThemeManager::windowShadowExtent() const {
        return integerToken("shadow", "extent", 0, 64);
    }

    qreal ThemeManager::windowShadowBlur() const {
        return realToken("shadow", "blur", 0, 128);
    }

    qreal ThemeManager::windowShadowSpread() const {
        return realToken("shadow", "spread", -32, 32);
    }

    qreal ThemeManager::windowShadowVerticalOffset() const {
        return realToken("shadow", "verticalOffset", -64, 64);
    }

    qreal ThemeManager::windowShadowHorizontalOffset() const {
        return realToken("shadow", "horizontalOffset", -64, 64);
    }

    QColor ThemeManager::windowShadowAmbientColor() const {
        return colorValue(schemeValue("shadow", "ambientColor"));
    }

    qreal ThemeManager::windowShadowAmbientBlur() const {
        return realToken("shadow", "ambientBlur", 0, 128);
    }

    qreal ThemeManager::windowShadowAmbientVerticalOffset() const {
        return realToken("shadow", "ambientVerticalOffset", -64, 64);
    }

    qreal ThemeManager::overlayShadowBlur() const {
        return realToken("shadow", "overlayBlur", 0, 128);
    }

    qreal ThemeManager::overlayShadowSpread() const {
        return realToken("shadow", "overlaySpread", -32, 32);
    }

    qreal ThemeManager::overlayShadowVerticalOffset() const {
        return realToken("shadow", "overlayVerticalOffset", -64, 64);
    }

    QString ThemeManager::iconStyle() const {
        return stringToken("icons", "style");
    }

    QString ThemeManager::activeIconStyle() const {
        return stringToken("icons", "activeStyle");
    }

    int ThemeManager::iconSize() const {
        return integerToken("icons", "size", 12, 32);
    }

    qreal ThemeManager::disabledIconOpacity() const {
        return realToken("icons", "disabledOpacity", 0, 1);
    }

    int ThemeManager::shortDuration() const {
        return integerToken("motion", "shortDuration", 0, 2000);
    }

    int ThemeManager::mediumDuration() const {
        return integerToken("motion", "mediumDuration", 0, 3000);
    }

    int ThemeManager::longDuration() const {
        return integerToken("motion", "longDuration", 0, 5000);
    }

    QString ThemeManager::fontFamily() const {
        return stringToken("typography", "fontFamily");
    }

    int ThemeManager::bodyFontSize() const {
        return integerToken("typography", "bodySize", 8, 40);
    }

    int ThemeManager::labelFontSize() const {
        return integerToken("typography", "labelSize", 8, 40);
    }

    int ThemeManager::titleFontSize() const {
        return integerToken("typography", "titleSize", 8, 64);
    }

    QVariantMap ThemeManager::themePreview(const QString &id, bool dark) const {
        const auto theme = std::find_if(m_themes.cbegin(), m_themes.cend(), [&id](const ThemeDefinition &candidate) {
            return candidate.id == id;
        });
        if (theme == m_themes.cend()) {
            return {};
        }
        const QString scheme = dark ? "dark" : "light";
        const QJsonObject &fallback = defaultTheme().root;
        const auto token = [&](const QString &scope, const QString &group, const QString &key) -> QJsonValue {
            const QJsonValue resolved = theme->root.value(scope).toObject().value(group).toObject().value(key);
            if (!resolved.isUndefined()) {
                return resolved;
            }
            return fallback.value(scope).toObject().value(group).toObject().value(key);
        };
        const auto schemeColor = [&](const QString &key) {
            return colorValue(token(scheme, "colors", key));
        };
        const auto baseColor = [&](const QString &key) {
            return colorValue(token("base", "colors", key));
        };
        const auto metric = [&](const QString &key, int minimum, int maximum) {
            return std::clamp(token("base", "metrics", key).toInt(), minimum, maximum);
        };
        QVariantMap preview;
        preview.insert("surface", schemeColor("background"));
        preview.insert("surfaceText", schemeColor("foreground"));
        preview.insert("surfaceVariantText", schemeColor("mutedForeground"));
        preview.insert("surfaceContainer", schemeColor("panelBackground"));
        preview.insert("surfaceContainerHigh", schemeColor("hoverBackground"));
        preview.insert("surfaceContainerHighest", schemeColor("activeBackground"));
        preview.insert("outline", schemeColor("outline"));
        preview.insert("iconColor", schemeColor("icon"));
        preview.insert("tabBarBackground", schemeColor("tabBarBackground"));
        preview.insert("primary", baseColor("primary"));
        preview.insert("primaryText", baseColor("onPrimary"));
        preview.insert("toolbarGradientEnabled", token(scheme, "chrome", "mode").toString() == "linear");
        preview.insert("toolbarGradientAngle", std::clamp(token(scheme, "chrome", "angle").toDouble(90), 0.0, 360.0));
        preview.insert("toolbarGradientStart", colorValue(token(scheme, "chrome", "startColor")));
        preview.insert("toolbarGradientMiddle", colorValue(token(scheme, "chrome", "middleColor")));
        preview.insert("toolbarGradientEnd", colorValue(token(scheme, "chrome", "endColor")));
        preview.insert(
            "toolbarGradientMiddlePosition",
            std::clamp(token(scheme, "chrome", "middlePosition").toDouble(0.5), 0.0, 1.0)
        );
        preview.insert("windowRadius", metric("windowRadius", 0, 48));
        preview.insert("contentRadius", metric("contentRadius", 0, 48));
        preview.insert("cardRadius", metric("cardRadius", 0, 48));
        preview.insert("controlRadius", metric("controlRadius", 0, 48));
        return preview;
    }

    void ThemeManager::refresh() {
        QList<ThemeDefinition> themes;
        themes.append(defaultTheme());
        QDir directory(themeDirectory());
        if (!directory.exists()) {
            directory.mkpath(".");
        }
        QString firstError;
        const QFileInfoList files = directory.entryInfoList({"*.json"}, QDir::Files | QDir::Readable, QDir::Name);
        for (const QFileInfo &fileInfo : files) {
            QFile file(fileInfo.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly) || file.size() > 131072) {
                if (firstError.isEmpty()) {
                    firstError = "Could not read " + fileInfo.fileName();
                }
                continue;
            }
            QString error;
            std::optional<ThemeDefinition> theme =
                parseTheme(file.readAll(), fileInfo.absoluteFilePath(), false, error);
            if (!theme) {
                if (firstError.isEmpty()) {
                    firstError = fileInfo.fileName() + ": " + error;
                }
                continue;
            }
            if (theme->id == defaultTheme().id) {
                if (firstError.isEmpty()) {
                    firstError = fileInfo.fileName() + ": the built-in theme ID is reserved";
                }
                continue;
            }
            const auto existing =
                std::find_if(themes.cbegin(), themes.cend(), [&theme](const ThemeDefinition &candidate) {
                    return candidate.id == theme->id;
                });
            if (existing == themes.cend()) {
                themes.append(std::move(*theme));
            }
        }
        beginResetModel();
        m_themes = std::move(themes);
        m_activeIndex = 0;
        const QString configured = SettingsStore::instance()->themeId();
        for (int index = 0; index < m_themes.size(); ++index) {
            if (m_themes.at(index).id == configured) {
                m_activeIndex = index;
                break;
            }
        }
        endResetModel();
        setLastError(firstError);
        emit activeThemeChanged();
        emit themeChanged();
    }

    bool ThemeManager::installTheme(const QUrl &source) {
        if (!source.isLocalFile()) {
            setLastError("Themes must be installed from a local JSON file");
            return false;
        }
        QFile file(source.toLocalFile());
        if (!file.open(QIODevice::ReadOnly)) {
            setLastError("Could not open the selected theme");
            return false;
        }
        if (file.size() > 131072) {
            setLastError("Theme files must be 128 KiB or smaller");
            return false;
        }
        const QByteArray data = file.readAll();
        QString error;
        std::optional<ThemeDefinition> theme = parseTheme(data, file.fileName(), false, error);
        if (!theme) {
            setLastError(error);
            return false;
        }
        if (theme->id == defaultTheme().id) {
            setLastError("The built-in theme ID is reserved");
            return false;
        }
        QDir directory(themeDirectory());
        if (!directory.exists() && !directory.mkpath(".")) {
            setLastError("Could not create the theme directory");
            return false;
        }
        QSaveFile destination(directory.filePath(theme->id + ".json"));
        if (!destination.open(QIODevice::WriteOnly) || destination.write(data) != data.size() ||
            !destination.commit()) {
            setLastError("Could not save the installed theme");
            return false;
        }
        const QString installedId = theme->id;
        refresh();
        return activateTheme(installedId);
    }

    bool ThemeManager::activateTheme(const QString &id) {
        for (int index = 0; index < m_themes.size(); ++index) {
            if (m_themes.at(index).id != id) {
                continue;
            }
            if (m_activeIndex == index) {
                setLastError({});
                return true;
            }
            const int previous = m_activeIndex;
            m_activeIndex = index;
            SettingsStore::instance()->setThemeId(id);
            if (previous >= 0) {
                emit dataChanged(this->index(previous), this->index(previous), {ActiveRole});
            }
            emit dataChanged(this->index(index), this->index(index), {ActiveRole});
            setLastError({});
            emit activeThemeChanged();
            emit themeChanged();
            return true;
        }
        setLastError("The selected theme is not installed");
        return false;
    }

    bool ThemeManager::beginEditing(const QString &id) {
        const QString targetId = id.isEmpty() ? activeThemeId() : id;
        for (const ThemeDefinition &theme : m_themes) {
            if (theme.id != targetId) {
                continue;
            }
            m_editorRoot = theme.root;
            m_editorSourceId = theme.id;
            m_editorBuiltIn = theme.builtIn;
            m_editorDirty = false;
            m_editing = true;
            setLastError({});
            emit editorChanged();
            m_editorModel->refreshValues();
            emit themeChanged();
            return true;
        }
        setLastError("The selected theme is not installed");
        return false;
    }

    void ThemeManager::cancelEditing() {
        if (!m_editing) {
            return;
        }
        m_editing = false;
        m_editorBuiltIn = false;
        m_editorDirty = false;
        m_editorSourceId.clear();
        m_editorRoot = {};
        setLastError({});
        emit editorChanged();
        m_editorModel->refreshValues();
        emit themeChanged();
    }

    bool ThemeManager::revertEditing() {
        return m_editing && beginEditing(m_editorSourceId);
    }

    QVariant ThemeManager::editorValue(const QString &path) const {
        if (!m_editing) {
            return valueAtPath(defaultTheme().root, path).toVariant();
        }
        QJsonValue value = valueAtPath(m_editorRoot, path);
        if (value.isUndefined()) {
            value = valueAtPath(defaultTheme().root, path);
        }
        return value.toVariant();
    }

    bool ThemeManager::setEditorToken(const QString &path, const QVariant &value) {
        if (!m_editing) {
            return false;
        }
        const ThemeEditorModel::TokenDefinition *definition = m_editorModel->definition(path);
        if (!definition) {
            return false;
        }
        QJsonValue converted;
        if (definition->kind == "color") {
            const QString text = value.toString().trimmed();
            if (!QColor(text).isValid()) {
                return false;
            }
            converted = text;
        } else if (definition->kind == "integer" || definition->kind == "real") {
            bool valid = false;
            const qreal number = value.toString().toDouble(&valid);
            if (!valid || number < definition->minimum || number > definition->maximum) {
                return false;
            }
            if (definition->kind == "integer" && number != qRound64(number)) {
                return false;
            }
            converted = number;
        } else if (definition->kind == "choice") {
            const QString text = value.toString();
            if (!definition->options.contains(text)) {
                return false;
            }
            converted = text;
        } else {
            const QString text = value.toString().trimmed();
            if (text.isEmpty()) {
                return false;
            }
            converted = text;
        }
        const QJsonValue current = valueAtPath(m_editorRoot, path);
        if (current == converted) {
            return true;
        }
        m_editorRoot = rootWithValue(m_editorRoot, path.split('.'), converted);
        m_editorDirty = true;
        setLastError({});
        emit editorChanged();
        m_editorModel->refreshValues();
        emit themeChanged();
        return true;
    }

    bool ThemeManager::setEditorColor(const QString &path, const QColor &color) {
        if (!color.isValid()) {
            return false;
        }
        const QColor::NameFormat format = color.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb;
        return setEditorToken(path, color.name(format));
    }

    bool ThemeManager::setEditorGradientValue(const QString &path, const QString &key, const QVariant &value) {
        if (!m_editing) {
            return false;
        }
        const ThemeEditorModel::TokenDefinition *definition = m_editorModel->definition(path);
        if (!definition || definition->kind != "gradient") {
            return false;
        }
        QJsonValue converted;
        if (key == "mode") {
            const QString mode = value.toString();
            if (mode != "solid" && mode != "linear") {
                return false;
            }
            converted = mode;
        } else if (key == "angle") {
            bool valid = false;
            const qreal number = value.toDouble(&valid);
            if (!valid || number < 0 || number > 360) {
                return false;
            }
            converted = number;
        } else if (key == "middlePosition") {
            bool valid = false;
            const qreal number = value.toDouble(&valid);
            if (!valid || number < 0 || number > 1) {
                return false;
            }
            converted = number;
        } else if (key == "startColor" || key == "middleColor" || key == "endColor") {
            const QColor color = value.value<QColor>();
            if (!color.isValid()) {
                return false;
            }
            const QColor::NameFormat format = color.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb;
            converted = color.name(format);
        } else {
            return false;
        }
        const QString valuePath = path + "." + key;
        if (valueAtPath(m_editorRoot, valuePath) == converted) {
            return true;
        }
        m_editorRoot = rootWithValue(m_editorRoot, valuePath.split('.'), converted);
        m_editorDirty = true;
        setLastError({});
        emit editorChanged();
        m_editorModel->refreshValues();
        emit themeChanged();
        return true;
    }

    bool ThemeManager::saveEditing() {
        if (!m_editing) {
            return false;
        }
        if (m_editorBuiltIn) {
            if (editorId().trimmed() != m_editorSourceId) {
                return saveEditingAs();
            }
            const QString baseId = m_editorSourceId + "-edited";
            QString id = baseId;
            int suffix = 2;
            const auto idExists = [this](const QString &candidate) {
                return std::any_of(m_themes.cbegin(), m_themes.cend(), [&candidate](const ThemeDefinition &theme) {
                    return theme.id == candidate;
                });
            };
            while (idExists(id)) {
                id = baseId.left(60) + "-" + QString::number(suffix++);
            }
            m_editorRoot.insert("id", id);
            m_editorRoot.insert("name", editorName().trimmed().left(73) + " Edited");
            emit editorChanged();
            return saveEditingAs();
        }
        const auto source = std::find_if(m_themes.cbegin(), m_themes.cend(), [this](const ThemeDefinition &theme) {
            return theme.id == m_editorSourceId;
        });
        if (source == m_themes.cend() || source->path.isEmpty()) {
            setLastError("The edited theme file is unavailable");
            return false;
        }
        const QString id = editorId().trimmed();
        if (id == m_editorSourceId) {
            if (!writeEditorTheme(source->path)) {
                return false;
            }
            return finishEditorSave(m_editorSourceId);
        }
        if (id == defaultTheme().id ||
            std::any_of(m_themes.cbegin(), m_themes.cend(), [&id](const ThemeDefinition &theme) {
                return theme.id == id;
            })) {
            setLastError("That theme ID is already installed");
            return false;
        }
        QDir directory(themeDirectory());
        if (!directory.exists() && !directory.mkpath(".")) {
            setLastError("Could not create the theme directory");
            return false;
        }
        const QString previousPath = source->path;
        const QString nextPath = directory.filePath(id + ".json");
        if (!writeEditorTheme(nextPath)) {
            return false;
        }
        if (previousPath != nextPath) {
            QFile::remove(previousPath);
        }
        return finishEditorSave(id);
    }

    bool ThemeManager::saveEditingAs() {
        if (!m_editing) {
            return false;
        }
        const QString id = editorId().trimmed();
        if (id == defaultTheme().id) {
            setLastError("Choose a new theme ID before saving");
            return false;
        }
        const auto existing = std::find_if(m_themes.cbegin(), m_themes.cend(), [&id](const ThemeDefinition &theme) {
            return theme.id == id;
        });
        if (existing != m_themes.cend()) {
            setLastError("That theme ID is already installed");
            return false;
        }
        QDir directory(themeDirectory());
        if (!directory.exists() && !directory.mkpath(".")) {
            setLastError("Could not create the theme directory");
            return false;
        }
        if (!writeEditorTheme(directory.filePath(id + ".json"))) {
            return false;
        }
        return finishEditorSave(id);
    }

    bool ThemeManager::exportEditing(const QUrl &destination) {
        if (!m_editing || !destination.isLocalFile()) {
            setLastError("Choose a local JSON export file");
            return false;
        }
        if (editorId().trimmed() == defaultTheme().id) {
            setLastError("Choose a shareable theme ID before exporting");
            return false;
        }
        QString path = destination.toLocalFile();
        if (!path.endsWith(".json", Qt::CaseInsensitive)) {
            path += ".json";
        }
        return writeEditorTheme(path);
    }

    QJsonValue ThemeManager::valueAtPath(const QJsonObject &root, const QString &path) const {
        QJsonValue value(root);
        for (const QString &part : path.split('.')) {
            if (!value.isObject()) {
                return {};
            }
            value = value.toObject().value(part);
        }
        return value;
    }

    QJsonObject ThemeManager::rootWithValue(
        const QJsonObject &root,
        const QStringList &parts,
        const QJsonValue &value,
        int index
    ) const {
        if (index < 0 || index >= parts.size()) {
            return root;
        }
        QJsonObject updated = root;
        const QString &key = parts.at(index);
        if (index == parts.size() - 1) {
            updated.insert(key, value);
        } else {
            updated.insert(key, rootWithValue(root.value(key).toObject(), parts, value, index + 1));
        }
        return updated;
    }

    const QJsonObject &ThemeManager::effectiveRoot() const {
        return m_editing ? m_editorRoot : activeTheme().root;
    }

    void ThemeManager::setEditorIdentity(const QString &key, const QString &value) {
        if (!m_editing || m_editorRoot.value(key).toString() == value) {
            return;
        }
        m_editorRoot.insert(key, value);
        m_editorDirty = true;
        setLastError({});
        emit editorChanged();
    }

    bool ThemeManager::writeEditorTheme(const QString &path) {
        QJsonObject root = m_editorRoot;
        root.insert("id", editorId().trimmed());
        root.insert("name", editorName().trimmed());
        root.insert("author", editorAuthor().trimmed());
        QString error;
        const std::optional<ThemeDefinition> validated =
            parseTheme(QJsonDocument(root).toJson(QJsonDocument::Compact), path, false, error);
        if (!validated) {
            setLastError(error);
            return false;
        }
        QSaveFile file(path);
        const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            setLastError("Could not write the theme file");
            return false;
        }
        m_editorRoot = root;
        setLastError({});
        emit editorChanged();
        return true;
    }

    bool ThemeManager::finishEditorSave(const QString &id) {
        m_editing = false;
        refresh();
        if (!activateTheme(id)) {
            return false;
        }
        return beginEditing(id);
    }

    std::optional<ThemeManager::ThemeDefinition>
    ThemeManager::parseTheme(const QByteArray &data, const QString &path, bool builtIn, QString &error) const {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            error = "Invalid JSON: " + parseError.errorString();
            return std::nullopt;
        }
        QJsonObject root = migrateTheme(document.object());
        if (root.value("formatVersion").toInt() != 2) {
            error = "Unsupported or missing formatVersion";
            return std::nullopt;
        }
        const QString id = root.value("id").toString();
        static const QRegularExpression idPattern("^[a-z0-9][a-z0-9-]{1,63}$");
        if (!idPattern.match(id).hasMatch()) {
            error = "Theme IDs use 2 to 64 lowercase letters, numbers, or hyphens";
            return std::nullopt;
        }
        const QString name = root.value("name").toString().trimmed();
        if (name.isEmpty() || name.size() > 80) {
            error = "Theme names use 1 to 80 characters";
            return std::nullopt;
        }
        const QString author = root.value("author").toString().trimmed();
        if (author.size() > 80) {
            error = "Theme authors use at most 80 characters";
            return std::nullopt;
        }
        const QJsonObject base = root.value("base").toObject();
        for (const QString &scope : {QString("base"), QString("light"), QString("dark")}) {
            const QJsonObject palette = root.value(scope).toObject().value("colors").toObject();
            for (auto iterator = palette.begin(); iterator != palette.end(); ++iterator) {
                const QColor color(iterator.value().toString());
                if (!iterator.value().isString() || !color.isValid()) {
                    error = "Invalid " + scope + " color: " + iterator.key();
                    return std::nullopt;
                }
            }
        }
        const QStringList iconStyles = {"linear", "line-duotone", "bold", "bold-duotone", "broken", "outline"};
        const QJsonObject icons = base.value("icons").toObject();
        for (const QString &key : {QString("style"), QString("activeStyle")}) {
            const QJsonValue value = icons.value(key);
            if (!value.isUndefined() && (!value.isString() || !iconStyles.contains(value.toString()))) {
                error = "Unsupported Solar icon style: " + value.toString();
                return std::nullopt;
            }
        }
        struct NumericRule {
            QString group;
            QString key;
            qreal minimum;
            qreal maximum;
            bool integer;
        };
        const QList<NumericRule> numericRules = {
            {"metrics", "windowRadius", 0, 48, true},
            {"metrics", "contentRadius", 0, 48, true},
            {"metrics", "cardRadius", 0, 48, true},
            {"metrics", "controlRadius", 0, 48, true},
            {"metrics", "menuRadius", 0, 48, true},
            {"metrics", "suggestionRadius", 0, 48, true},
            {"metrics", "workspaceInset", 0, 48, true},
            {"metrics", "workspaceGap", 0, 48, true},
            {"metrics", "tabMinimumWidth", 64, 240, true},
            {"metrics", "tabMaximumWidth", 96, 400, true},
            {"metrics", "pinnedTabWidth", 36, 96, true},
            {"metrics", "windowBorderWidth", 0, 8, true},
            {"metrics", "contentBorderWidth", 0, 8, true},
            {"metrics", "paneBorderWidth", 0, 8, true},
            {"metrics", "menuBorderWidth", 0, 8, true},
            {"metrics", "focusBorderWidth", 0, 8, true},
            {"shadow", "extent", 0, 96, true},
            {"shadow", "blur", 0, 128, false},
            {"shadow", "spread", -32, 32, false},
            {"shadow", "verticalOffset", -64, 64, false},
            {"shadow", "horizontalOffset", -64, 64, false},
            {"shadow", "ambientBlur", 0, 128, false},
            {"shadow", "ambientVerticalOffset", -64, 64, false},
            {"shadow", "overlayBlur", 0, 128, false},
            {"shadow", "overlaySpread", -32, 32, false},
            {"shadow", "overlayVerticalOffset", -64, 64, false},
            {"icons", "size", 12, 32, true},
            {"icons", "disabledOpacity", 0, 1, false},
            {"motion", "shortDuration", 0, 2000, true},
            {"motion", "mediumDuration", 0, 3000, true},
            {"motion", "longDuration", 0, 5000, true},
            {"typography", "bodySize", 8, 40, true},
            {"typography", "labelSize", 8, 40, true},
            {"typography", "titleSize", 8, 64, true},
        };
        for (const NumericRule &rule : numericRules) {
            const QJsonValue value = base.value(rule.group).toObject().value(rule.key);
            if (value.isUndefined()) {
                continue;
            }
            const qreal number = value.toDouble(std::numeric_limits<qreal>::quiet_NaN());
            if (!value.isDouble() || !std::isfinite(number) || number < rule.minimum || number > rule.maximum ||
                (rule.integer && number != qRound64(number))) {
                error = "Invalid theme value: " + rule.group + "." + rule.key;
                return std::nullopt;
            }
        }
        for (const QString &scheme : {QString("light"), QString("dark")}) {
            const QJsonObject scope = root.value(scheme).toObject();
            const QJsonObject chrome = scope.value("chrome").toObject();
            const QString mode = chrome.value("mode").toString();
            if (!mode.isEmpty() && mode != "solid" && mode != "linear") {
                error = "Invalid gradient mode: " + scheme;
                return std::nullopt;
            }
            for (const QString &key : {QString("startColor"), QString("middleColor"), QString("endColor")}) {
                const QJsonValue value = chrome.value(key);
                if (!value.isUndefined() && (!value.isString() || !QColor(value.toString()).isValid())) {
                    error = "Invalid gradient color: " + scheme + "." + key;
                    return std::nullopt;
                }
            }
            const qreal angle = chrome.value("angle").toDouble(90);
            const qreal middlePosition = chrome.value("middlePosition").toDouble(0.5);
            if (angle < 0 || angle > 360 || middlePosition < 0 || middlePosition > 1) {
                error = "Invalid gradient geometry: " + scheme;
                return std::nullopt;
            }
            const QJsonObject schemeShadow = scope.value("shadow").toObject();
            for (const QString &key : {QString("color"), QString("ambientColor"), QString("overlayColor")}) {
                const QJsonValue value = schemeShadow.value(key);
                if (!value.isUndefined() && (!value.isString() || !QColor(value.toString()).isValid())) {
                    error = "Invalid shadow color: " + scheme + "." + key;
                    return std::nullopt;
                }
            }
        }
        ThemeDefinition definition;
        definition.id = id;
        definition.name = name;
        definition.author = author;
        definition.path = path;
        definition.builtIn = builtIn;
        definition.root = root;
        return definition;
    }

    QJsonObject ThemeManager::migrateTheme(QJsonObject root) const {
        if (root.value("formatVersion").toInt() != 1) {
            return root;
        }
        const QJsonObject legacyColors = root.take("colors").toObject();
        QJsonObject base;
        QJsonObject baseColors;
        const QJsonObject legacyLight = legacyColors.value("light").toObject();
        for (const QString &key :
             {QString("primary"),
              QString("onPrimary"),
              QString("primaryContainer"),
              QString("onPrimaryContainer"),
              QString("privatePrimary")}) {
            if (legacyLight.contains(key)) {
                baseColors.insert(key, legacyLight.value(key));
            }
        }
        base.insert("colors", baseColors);
        for (const QString &group : {QString("metrics"), QString("motion"), QString("icons"), QString("typography")}) {
            base.insert(group, root.take(group));
        }
        QJsonObject legacyShadow = root.take("shadow").toObject();
        QJsonObject baseShadow = legacyShadow;
        for (const QString &key :
             {QString("lightColor"), QString("darkColor"), QString("ambientLightColor"), QString("ambientDarkColor")}) {
            baseShadow.remove(key);
        }
        base.insert("shadow", baseShadow);
        root.insert("base", base);
        for (const QString &scheme : {QString("light"), QString("dark")}) {
            QJsonObject palette = legacyColors.value(scheme).toObject();
            const QString toolbarColor = palette.take("toolbarBackground").toString();
            for (const QString &key :
                 {QString("primary"),
                  QString("onPrimary"),
                  QString("primaryContainer"),
                  QString("onPrimaryContainer"),
                  QString("privatePrimary"),
                  QString("focusBorder"),
                  QString("disabledIcon")}) {
                palette.remove(key);
            }
            QJsonObject chrome;
            if (!toolbarColor.isEmpty()) {
                chrome.insert("mode", "solid");
                chrome.insert("angle", 90);
                chrome.insert("startColor", toolbarColor);
                chrome.insert("middleColor", toolbarColor);
                chrome.insert("endColor", toolbarColor);
                chrome.insert("middlePosition", 0.5);
            }
            QJsonObject schemeShadow;
            const QJsonValue shadowColor = legacyShadow.value(scheme == "light" ? "lightColor" : "darkColor");
            const QJsonValue ambientColor =
                legacyShadow.value(scheme == "light" ? "ambientLightColor" : "ambientDarkColor");
            if (!shadowColor.isUndefined()) {
                schemeShadow.insert("color", shadowColor);
            }
            if (!ambientColor.isUndefined()) {
                schemeShadow.insert("ambientColor", ambientColor);
            }
            QJsonObject scope;
            scope.insert("colors", palette);
            scope.insert("chrome", chrome);
            scope.insert("shadow", schemeShadow);
            root.insert(scheme, scope);
        }
        root.insert("formatVersion", 2);
        return root;
    }

    const ThemeManager::ThemeDefinition &ThemeManager::activeTheme() const {
        return m_themes.at(std::clamp(m_activeIndex, 0, static_cast<int>(m_themes.size()) - 1));
    }

    const ThemeManager::ThemeDefinition &ThemeManager::defaultTheme() const {
        return m_themes.first();
    }

    QJsonValue ThemeManager::themeValue(const QString &group, const QString &key) const {
        const QJsonValue activeValue = effectiveRoot().value("base").toObject().value(group).toObject().value(key);
        if (!activeValue.isUndefined()) {
            return activeValue;
        }
        return defaultTheme().root.value("base").toObject().value(group).toObject().value(key);
    }

    QJsonValue ThemeManager::schemeValue(const QString &group, const QString &key) const {
        const QString scheme = darkScheme() ? "dark" : "light";
        const QJsonValue activeValue = effectiveRoot().value(scheme).toObject().value(group).toObject().value(key);
        if (!activeValue.isUndefined()) {
            return activeValue;
        }
        return defaultTheme().root.value(scheme).toObject().value(group).toObject().value(key);
    }

    QColor ThemeManager::paletteColor(const QString &key) const {
        static const QStringList baseColors =
            {"primary", "onPrimary", "primaryContainer", "onPrimaryContainer", "privatePrimary"};
        if (key == "focusBorder") {
            return paletteColor("primary");
        }
        return colorValue(baseColors.contains(key) ? themeValue("colors", key) : schemeValue("colors", key));
    }

    QColor ThemeManager::colorValue(const QJsonValue &value) const {
        const QColor color(value.toString());
        return color.isValid() ? color : QColor(Qt::transparent);
    }

    QColor ThemeManager::gradientColor(const QString &key) const {
        return colorValue(schemeValue("chrome", key));
    }

    QColor ThemeManager::shadowColor() const {
        return colorValue(schemeValue("shadow", "color"));
    }

    int ThemeManager::integerToken(const QString &group, const QString &key, int minimum, int maximum) const {
        const int fallback = defaultTheme().root.value("base").toObject().value(group).toObject().value(key).toInt();
        return std::clamp(themeValue(group, key).toInt(fallback), minimum, maximum);
    }

    qreal ThemeManager::realToken(const QString &group, const QString &key, qreal minimum, qreal maximum) const {
        const qreal fallback =
            defaultTheme().root.value("base").toObject().value(group).toObject().value(key).toDouble();
        return std::clamp(themeValue(group, key).toDouble(fallback), minimum, maximum);
    }

    QString ThemeManager::stringToken(const QString &group, const QString &key) const {
        const QString fallback =
            defaultTheme().root.value("base").toObject().value(group).toObject().value(key).toString();
        const QString value = themeValue(group, key).toString().trimmed();
        return value.isEmpty() ? fallback : value;
    }

    bool ThemeManager::darkScheme() const {
        SettingsStore *settings = SettingsStore::instance();
        return settings->theme() == "dark" || (settings->theme() == "system" && settings->systemDark());
    }

    void ThemeManager::setLastError(const QString &error) {
        if (m_lastError == error) {
            return;
        }
        m_lastError = error;
        emit lastErrorChanged();
    }

    void ThemeManager::selectConfiguredTheme() {
        const QString configured = SettingsStore::instance()->themeId();
        for (int index = 0; index < m_themes.size(); ++index) {
            if (m_themes.at(index).id == configured) {
                if (m_activeIndex == index) {
                    return;
                }
                const int previous = m_activeIndex;
                m_activeIndex = index;
                emit dataChanged(this->index(previous), this->index(previous), {ActiveRole});
                emit dataChanged(this->index(index), this->index(index), {ActiveRole});
                emit activeThemeChanged();
                emit themeChanged();
                return;
            }
        }
    }

}
