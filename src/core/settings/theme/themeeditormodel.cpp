#include "core/settings/theme/themeeditormodel.h"
#include "core/settings/theme/thememanager.h"

namespace eden::core {

    ThemeEditorModel::ThemeEditorModel(ThemeManager *manager)
        : QAbstractListModel(manager),
          m_manager(manager) {
        addSection("Overview", "Name the theme, preview its personality, and manage saving or sharing.");
        addSection("Core palette", "Shared brand colors that stay consistent in light and dark mode.");
        addSection("Surfaces", "Light and dark surfaces are paired on each row so their relationship is easy to tune.");
        addSection("Text and icons", "Readable foregrounds and icon colors for both appearance modes.");
        addSection("Borders", "Scheme-aware outlines for windows, panes, content, and menus.");
        addSection("Status and privacy", "Status colors and the distinct visual identity used by private browsing.");
        addSection("Shape and spacing", "Shared corner radii, gutters, pane gaps, and tab sizing.");
        addSection("Border widths", "Shared stroke widths for each surface class and focus treatment.");
        addSection("Window shadow", "Paired shadow colors with shared geometry and softness.");
        addSection("Icons", "Shared Solar families, active-state emphasis, size, and disabled treatment.");
        addSection("Typography", "The shared type family and size scale used throughout Eden.");
        addSection("Motion", "Shared animation timing for feedback and layout transitions.");

        addBaseColors();
        addSchemeColors();

        addToken(
            "base.metrics.windowRadius",
            "Window radius",
            "Rounds the four outside corners of the browser window.",
            "Shape and spacing",
            "integer",
            0,
            48,
            "px"
        );
        addToken(
            "base.metrics.contentRadius",
            "Content radius",
            "Rounds webpage viewports and internal-page surfaces.",
            "Shape and spacing",
            "integer",
            0,
            48,
            "px"
        );
        addToken(
            "base.metrics.cardRadius",
            "Surface radius",
            "Rounds cards, sidebars, panes, and grouped surfaces.",
            "Shape and spacing",
            "integer",
            0,
            48,
            "px"
        );
        addToken(
            "base.metrics.controlRadius",
            "Control radius",
            "Rounds buttons, fields, toggles, and pill controls.",
            "Shape and spacing",
            "integer",
            0,
            48,
            "px"
        );
        addToken(
            "base.metrics.menuRadius",
            "Overlay radius",
            "Rounds menus, popovers, and suggestion containers.",
            "Shape and spacing",
            "integer",
            0,
            48,
            "px"
        );
        addToken(
            "base.metrics.workspaceInset",
            "Workspace inset",
            "Sets the gutter between browser chrome and content surfaces.",
            "Shape and spacing",
            "integer",
            0,
            48,
            "px"
        );
        addToken(
            "base.metrics.workspaceGap",
            "Pane gap",
            "Sets the space between the webpage and an open sidebar or tool pane.",
            "Shape and spacing",
            "integer",
            0,
            48,
            "px"
        );
        addToken(
            "base.metrics.tabMinimumWidth",
            "Tab width range",
            "Minimum width before horizontal overflow navigation appears.",
            "Shape and spacing",
            "integer",
            64,
            240,
            "px"
        );
        addToken(
            "base.metrics.tabMaximumWidth",
            "Maximum tab width",
            "Maximum width available to a horizontal tab.",
            "Shape and spacing",
            "integer",
            96,
            400,
            "px"
        );
        addToken(
            "base.metrics.pinnedTabWidth",
            "Pinned tab width",
            "Favicon-only width used by pinned tabs.",
            "Shape and spacing",
            "integer",
            36,
            96,
            "px"
        );

        addToken(
            "base.metrics.windowBorderWidth",
            "Window border",
            "Stroke width around the complete browser window.",
            "Border widths",
            "integer",
            0,
            8,
            "px"
        );
        addToken(
            "base.metrics.contentBorderWidth",
            "Content border",
            "Stroke width around webpage and internal-page surfaces.",
            "Border widths",
            "integer",
            0,
            8,
            "px"
        );
        addToken(
            "base.metrics.paneBorderWidth",
            "Pane border",
            "Stroke width around sidebars and tool panes.",
            "Border widths",
            "integer",
            0,
            8,
            "px"
        );
        addToken(
            "base.metrics.menuBorderWidth",
            "Overlay border",
            "Stroke width around menus, suggestions, and popovers.",
            "Border widths",
            "integer",
            0,
            8,
            "px"
        );
        addToken(
            "base.metrics.focusBorderWidth",
            "Focus ring",
            "Stroke width used for the active keyboard focus ring.",
            "Border widths",
            "integer",
            0,
            8,
            "px"
        );

        addSchemeToken(
            "shadow.color",
            "Shadow color",
            "Main outer shadow color. Alpha controls its strength.",
            "Window shadow",
            "color"
        );
        addSchemeToken(
            "shadow.ambientColor",
            "Ambient shadow",
            "Soft shadow close to the window edge. Alpha controls its strength.",
            "Window shadow",
            "color"
        );
        addSchemeToken(
            "shadow.overlayColor",
            "Overlay shadow",
            "Shadow beneath menus, popovers, and suggestion panels.",
            "Window shadow",
            "color"
        );
        addToken(
            "base.shadow.extent",
            "Shadow extent",
            "Transparent room reserved around the window for the shadow.",
            "Window shadow",
            "integer",
            0,
            96,
            "px"
        );
        addToken(
            "base.shadow.blur",
            "Shadow softness",
            "Controls how softly the main shadow fades.",
            "Window shadow",
            "real",
            0,
            128,
            "px"
        );
        addToken(
            "base.shadow.spread",
            "Shadow spread",
            "Expands or contracts the main shadow before blur.",
            "Window shadow",
            "real",
            -32,
            32,
            "px"
        );
        addToken(
            "base.shadow.horizontalOffset",
            "Horizontal offset",
            "Moves the main shadow left or right.",
            "Window shadow",
            "real",
            -64,
            64,
            "px"
        );
        addToken(
            "base.shadow.verticalOffset",
            "Vertical offset",
            "Moves the main shadow down or up.",
            "Window shadow",
            "real",
            -64,
            64,
            "px"
        );
        addToken(
            "base.shadow.ambientBlur",
            "Ambient softness",
            "Controls the softness of the close ambient shadow.",
            "Window shadow",
            "real",
            0,
            128,
            "px"
        );
        addToken(
            "base.shadow.ambientVerticalOffset",
            "Ambient offset",
            "Moves the ambient shadow down or up.",
            "Window shadow",
            "real",
            -64,
            64,
            "px"
        );
        addToken(
            "base.shadow.overlayBlur",
            "Overlay softness",
            "Controls how softly menu and popover shadows fade.",
            "Window shadow",
            "real",
            0,
            128,
            "px"
        );
        addToken(
            "base.shadow.overlaySpread",
            "Overlay spread",
            "Expands or contracts menu and popover shadows before blur.",
            "Window shadow",
            "real",
            -32,
            32,
            "px"
        );
        addToken(
            "base.shadow.overlayVerticalOffset",
            "Overlay offset",
            "Moves menu and popover shadows down or up.",
            "Window shadow",
            "real",
            -64,
            64,
            "px"
        );

        const QStringList iconStyles = {"linear", "line-duotone", "bold", "bold-duotone", "broken", "outline"};
        addToken(
            "base.icons.style",
            "Default Solar family",
            "Icon family used for normal controls and inactive states.",
            "Icons",
            "choice",
            0,
            0,
            {},
            iconStyles
        );
        addToken(
            "base.icons.activeStyle",
            "Active Solar family",
            "Icon family used for selected and active states.",
            "Icons",
            "choice",
            0,
            0,
            {},
            iconStyles
        );
        addToken(
            "base.icons.size",
            "Icon size",
            "Shared visual size for browser chrome icons.",
            "Icons",
            "integer",
            12,
            32,
            "px"
        );
        addToken(
            "base.icons.disabledOpacity",
            "Disabled opacity",
            "Opacity applied to icons on unavailable controls.",
            "Icons",
            "real",
            0,
            1
        );

        addToken(
            "base.typography.fontFamily",
            "Font family",
            "Typeface used by browser chrome and internal pages.",
            "Typography",
            "text"
        );
        addToken(
            "base.typography.bodySize",
            "Body size",
            "Size for fields, URLs, paragraphs, and supporting text.",
            "Typography",
            "integer",
            8,
            40,
            "px"
        );
        addToken(
            "base.typography.labelSize",
            "Label size",
            "Size for buttons, compact navigation, and field labels.",
            "Typography",
            "integer",
            8,
            40,
            "px"
        );
        addToken(
            "base.typography.titleSize",
            "Title size",
            "Size for card headings, pane titles, and prominent labels.",
            "Typography",
            "integer",
            8,
            64,
            "px"
        );

        addToken(
            "base.motion.shortDuration",
            "Quick feedback",
            "Duration for hover, press, icon, and color feedback.",
            "Motion",
            "integer",
            0,
            2000,
            "ms"
        );
        addToken(
            "base.motion.mediumDuration",
            "Layout transition",
            "Duration for panes and medium layout changes.",
            "Motion",
            "integer",
            0,
            3000,
            "ms"
        );
        addToken(
            "base.motion.longDuration",
            "Large transition",
            "Duration for the most prominent transitions.",
            "Motion",
            "integer",
            0,
            5000,
            "ms"
        );

        rebuildVisibleRows();
    }

    int ThemeEditorModel::rowCount(const QModelIndex &parent) const {
        return parent.isValid() ? 0 : m_visibleRows.size();
    }

    QVariant ThemeEditorModel::data(const QModelIndex &index, int role) const {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_visibleRows.size()) {
            return {};
        }
        const TokenDefinition &token = m_tokens.at(m_visibleRows.at(index.row()));
        if (role == PathRole) {
            return token.path;
        }
        if (role == LabelRole) {
            return token.label;
        }
        if (role == DescriptionRole) {
            return token.description;
        }
        if (role == SectionRole) {
            return token.section;
        }
        if (role == KindRole) {
            return token.kind;
        }
        if (role == ValueRole) {
            return token.path.isEmpty() ? QVariant() : m_manager->editorValue(token.path);
        }
        if (role == MinimumRole) {
            return token.minimum;
        }
        if (role == MaximumRole) {
            return token.maximum;
        }
        if (role == UnitRole) {
            return token.unit;
        }
        if (role == OptionsRole) {
            return token.options;
        }
        if (role == SectionStartRole) {
            return index.row() == 0 || m_tokens.at(m_visibleRows.at(index.row() - 1)).section != token.section;
        }
        if (role == PairedRole) {
            return !token.lightPath.isEmpty();
        }
        if (role == LightPathRole) {
            return token.lightPath;
        }
        if (role == DarkPathRole) {
            return token.darkPath;
        }
        if (role == LightValueRole) {
            return token.lightPath.isEmpty() ? QVariant() : m_manager->editorValue(token.lightPath);
        }
        if (role == DarkValueRole) {
            return token.darkPath.isEmpty() ? QVariant() : m_manager->editorValue(token.darkPath);
        }
        return {};
    }

    QHash<int, QByteArray> ThemeEditorModel::roleNames() const {
        return {
            {PathRole, "tokenPath"},
            {LabelRole, "tokenLabel"},
            {DescriptionRole, "tokenDescription"},
            {SectionRole, "tokenSection"},
            {KindRole, "tokenKind"},
            {ValueRole, "tokenValue"},
            {MinimumRole, "tokenMinimum"},
            {MaximumRole, "tokenMaximum"},
            {UnitRole, "tokenUnit"},
            {OptionsRole, "tokenOptions"},
            {SectionStartRole, "sectionStart"},
            {PairedRole, "tokenPaired"},
            {LightPathRole, "tokenLightPath"},
            {DarkPathRole, "tokenDarkPath"},
            {LightValueRole, "tokenLightValue"},
            {DarkValueRole, "tokenDarkValue"}
        };
    }

    const ThemeEditorModel::TokenDefinition *ThemeEditorModel::definition(const QString &path) const {
        for (const TokenDefinition &token : m_tokens) {
            if (token.path == path || token.lightPath == path || token.darkPath == path) {
                return &token;
            }
        }
        return nullptr;
    }

    void ThemeEditorModel::refreshValues() {
        if (!m_visibleRows.isEmpty()) {
            emit dataChanged(index(0), index(m_visibleRows.size() - 1), {ValueRole, LightValueRole, DarkValueRole});
        }
    }

    QString ThemeEditorModel::selectedSection() const {
        return m_selectedSection;
    }

    void ThemeEditorModel::setSelectedSection(const QString &section) {
        if (m_selectedSection == section) {
            return;
        }
        m_selectedSection = section;
        emit selectedSectionChanged();
    }

    QVariantList ThemeEditorModel::navigationSections() const {
        QVariantList result;
        result.reserve(m_sections.size());
        for (const SectionDefinition &section : m_sections) {
            QVariantMap entry;
            entry.insert("title", section.title);
            entry.insert("description", section.description);
            result.append(entry);
        }
        return result;
    }

    int ThemeEditorModel::firstIndexForSection(const QString &section) const {
        for (int row = 0; row < m_visibleRows.size(); ++row) {
            if (m_tokens.at(m_visibleRows.at(row)).section == section) {
                return row;
            }
        }
        return -1;
    }

    QString ThemeEditorModel::sectionDescription(const QString &section) const {
        for (const SectionDefinition &definition : m_sections) {
            if (definition.title == section) {
                return definition.description;
            }
        }
        return {};
    }

    void ThemeEditorModel::addSection(const QString &title, const QString &description) {
        m_sections.append({title, description});
    }

    void ThemeEditorModel::addBaseColors() {
        addToken(
            "base.colors.primary",
            "Primary",
            "Shared accent for important actions, selected controls, and focus rings.",
            "Core palette",
            "color"
        );
        addToken(
            "base.colors.onPrimary",
            "On primary",
            "Text and icons placed directly on the primary accent.",
            "Core palette",
            "color"
        );
        addToken(
            "base.colors.primaryContainer",
            "Primary container",
            "Shared softer accent surface for selected groups.",
            "Core palette",
            "color"
        );
        addToken(
            "base.colors.onPrimaryContainer",
            "On primary container",
            "Text and icons placed on primary containers.",
            "Core palette",
            "color"
        );
        addToken(
            "base.colors.privatePrimary",
            "Private accent",
            "Shared accent that distinguishes private browsing.",
            "Status and privacy",
            "color"
        );
    }

    void ThemeEditorModel::addSchemeColors() {
        addSchemeToken(
            "chrome",
            "Toolbar and gutters",
            "Solid or three-stop linear gradient shared by the toolbar and workspace gutter.",
            "Surfaces",
            "gradient"
        );
        addSchemeToken(
            "colors.background",
            "Content background",
            "Base color behind webpage surfaces and internal pages.",
            "Surfaces",
            "color"
        );
        addSchemeToken(
            "colors.tabBarBackground",
            "Tab bar overlay",
            "Color layered over the toolbar gradient behind tabs. Alpha controls how strongly it tints the gradient.",
            "Surfaces",
            "color"
        );
        addSchemeToken(
            "colors.panelBackground",
            "Panels",
            "Cards, menus, sidebars, and tool panes.",
            "Surfaces",
            "color"
        );
        addSchemeToken(
            "colors.hoverBackground",
            "Hover surface",
            "Temporary surface shown under a hovered interactive item.",
            "Surfaces",
            "color"
        );
        addSchemeToken(
            "colors.activeBackground",
            "Active surface",
            "Active tabs, selected rows, and pressed controls.",
            "Surfaces",
            "color"
        );

        addSchemeToken(
            "colors.foreground",
            "Primary text",
            "Highest-emphasis text used for titles, labels, and content.",
            "Text and icons",
            "color"
        );
        addSchemeToken(
            "colors.mutedForeground",
            "Secondary text",
            "Descriptions, URLs, and supporting information.",
            "Text and icons",
            "color"
        );
        addSchemeToken(
            "colors.disabledForeground",
            "Disabled text",
            "Text color for controls that are unavailable.",
            "Text and icons",
            "color"
        );
        addSchemeToken(
            "colors.icon",
            "Icons",
            "Default tint applied to Solar icons in browser chrome.",
            "Text and icons",
            "color"
        );
        addSchemeToken(
            "colors.completionHint",
            "Address completion",
            "Hinted URL suffix shown after typed omnibox text.",
            "Text and icons",
            "color"
        );

        addSchemeToken(
            "colors.outline",
            "General outline",
            "Low-emphasis separators and general outlines.",
            "Borders",
            "color"
        );
        addSchemeToken(
            "colors.windowBorder",
            "Window border",
            "Outer stroke around the complete browser window.",
            "Borders",
            "color"
        );
        addSchemeToken(
            "colors.contentBorder",
            "Content border",
            "Stroke around webpage and internal-page surfaces.",
            "Borders",
            "color"
        );
        addSchemeToken(
            "colors.paneBorder",
            "Pane border",
            "Stroke around sidebars and right-side tool panes.",
            "Borders",
            "color"
        );
        addSchemeToken(
            "colors.menuBorder",
            "Overlay border",
            "Stroke around menus, suggestions, and popovers.",
            "Borders",
            "color"
        );

        addSchemeToken(
            "colors.error",
            "Error",
            "Warnings, invalid values, failed actions, and destructive emphasis.",
            "Status and privacy",
            "color"
        );
        addSchemeToken(
            "colors.privateBackground",
            "Private background",
            "Window background used by private browsing sessions.",
            "Status and privacy",
            "color"
        );
    }

    void ThemeEditorModel::addToken(
        const QString &path,
        const QString &label,
        const QString &description,
        const QString &section,
        const QString &kind,
        qreal minimum,
        qreal maximum,
        const QString &unit,
        const QStringList &options
    ) {
        m_tokens.append({path, label, description, section, kind, unit, {}, {}, minimum, maximum, options});
    }

    void ThemeEditorModel::addSchemeToken(
        const QString &path,
        const QString &label,
        const QString &description,
        const QString &section,
        const QString &kind,
        qreal minimum,
        qreal maximum,
        const QString &unit,
        const QStringList &options
    ) {
        m_tokens.append(
            {{}, label, description, section, kind, unit, "light." + path, "dark." + path, minimum, maximum, options}
        );
    }

    void ThemeEditorModel::rebuildVisibleRows() {
        m_visibleRows.clear();
        for (int row = 0; row < m_tokens.size(); ++row) {
            m_visibleRows.append(row);
        }
    }

}
