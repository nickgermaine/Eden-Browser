#include "engine/cef/devtoolstheme.h"

#include "core/settings/theme/thememanager.h"

#include <QColor>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace eden::engine::cef {

    static QString css(const QColor &color) {
        return QStringLiteral("rgba(%1,%2,%3,%4)")
            .arg(color.red())
            .arg(color.green())
            .arg(color.blue())
            .arg(QString::number(color.alphaF(), 'f', 3));
    }

    static QString mix(const QColor &color, int percent, const QString &base = QStringLiteral("transparent")) {
        return QStringLiteral("color-mix(in srgb, %1 %2%, %3)").arg(css(color)).arg(percent).arg(base);
    }

    QString devToolsThemeScript() {
        const core::ThemeManager *theme = core::ThemeManager::instance();
        const QString surface = css(theme->surface());
        const QString container = css(theme->surfaceContainer());
        const QString containerLow = css(theme->surfaceContainerLow());
        const QString containerHigh = css(theme->surfaceContainerHigh());
        const QString containerHighest = css(theme->surfaceContainerHighest());
        const QString text = css(theme->surfaceText());
        const QString mutedText = css(theme->surfaceVariantText());
        const QString disabledText = css(theme->disabledText());
        const QString primary = css(theme->primary());
        const QString primaryText = css(theme->primaryText());
        const QString hairline = mix(theme->outline(), 22);
        const QString hairlineStrong = mix(theme->outline(), 38);
        const QJsonObject tokens{
            {"--sys-color-cdt-base-container", surface},
            {"--sys-color-cdt-base", surface},
            {"--sys-color-base", surface},
            {"--sys-color-base-container", container},
            {"--sys-color-base-container-elevated", containerHigh},
            {"--sys-color-surface", surface},
            {"--sys-color-surface1", containerLow},
            {"--sys-color-surface2", container},
            {"--sys-color-surface3", containerHigh},
            {"--sys-color-surface4", containerHighest},
            {"--sys-color-surface5", containerHighest},
            {"--sys-color-surface-variant", containerHigh},
            {"--sys-color-on-surface", text},
            {"--sys-color-on-base", text},
            {"--sys-color-on-surface-subtle", mutedText},
            {"--sys-color-on-surface-variant", mutedText},
            {"--sys-color-token-subtle", disabledText},
            {"--sys-color-state-disabled", disabledText},
            {"--sys-color-primary", primary},
            {"--sys-color-primary-bright", primary},
            {"--sys-color-on-primary", primaryText},
            {"--sys-color-tonal-container", mix(theme->primary(), 24)},
            {"--sys-color-on-tonal-container", text},
            {"--sys-color-tonal-outline", mix(theme->primary(), 45)},
            {"--sys-color-neutral-container", containerHigh},
            {"--sys-color-neutral-bright", containerHighest},
            {"--sys-color-neutral-outline", hairlineStrong},
            {"--sys-color-outline", hairlineStrong},
            {"--sys-color-divider", hairline},
            {"--sys-color-divider-prominent", hairlineStrong},
            {"--sys-color-divider-on-tonal-container", mix(theme->primary(), 35)},
            {"--sys-color-state-hover-on-subtle", mix(theme->surfaceText(), 7)},
            {"--sys-color-state-hover-on-prominent", mix(theme->surfaceText(), 12)},
            {"--sys-color-state-ripple-neutral-on-subtle", mix(theme->surfaceText(), 12)},
            {"--sys-color-state-focus-highlight", mix(theme->surfaceText(), 8)},
            {"--sys-color-state-focus-select", mix(theme->primary(), 30)},
            {"--sys-color-state-focus-ring", css(theme->focusBorder())},
            {"--sys-color-state-header-hover", containerHigh},
            {"--sys-color-error", css(theme->error())},
            {"--app-color-toolbar-background", container},
            {"--app-color-selected", mix(theme->primary(), 24)},
            {"--color-background", surface},
            {"--color-background-elevation-1", container},
            {"--color-background-elevation-2", containerHigh},
            {"--color-text-primary", text},
            {"--color-text-secondary", mutedText},
            {"--color-details-hairline", hairline},
            {"--color-details-hairline-light", hairline},
            {"--color-primary-old", primary},
            {"--eden-font", QLatin1Char('"') + theme->fontFamily().remove(QLatin1Char('"')) + QLatin1Char('"')},
            {"--eden-radius-small", QStringLiteral("%1px").arg(std::min(theme->controlRadius(), 7))},
            {"--eden-radius-medium", QStringLiteral("%1px").arg(std::min(theme->cardRadius(), 10))},
            {"--eden-duration", QStringLiteral("%1ms").arg(theme->shortDuration())},
            {"--eden-container-high", containerHigh},
            {"--eden-container-highest", containerHighest},
            {"--eden-text", text},
            {"--eden-muted-text", mutedText},
            {"--eden-scrollbar", mix(theme->surfaceText(), 22)},
            {"--eden-scrollbar-hover", mix(theme->surfaceText(), 38)}
        };
        const QString sheet = QStringLiteral(
            ":host,body{font-family:var(--eden-font),system-ui,sans-serif}"
            "::-webkit-scrollbar{width:10px;height:10px;background:transparent}"
            "::-webkit-scrollbar-thumb{border:3px solid transparent;border-radius:999px;"
            "background-color:var(--eden-scrollbar);background-clip:padding-box}"
            "::-webkit-scrollbar-thumb:hover{background-color:var(--eden-scrollbar-hover)}"
            "::-webkit-scrollbar-corner{background:transparent}"
            ".tabbed-pane-header{border-bottom-color:var(--sys-color-divider)!important;"
            "background-color:var(--app-color-toolbar-background)!important;min-height:34px;align-items:center}"
            ".tabbed-pane-header-tabs{align-items:center;gap:2px}"
            ".tabbed-pane-header-tab{height:26px!important;margin:0 1px!important;padding:0 10px!important;"
            "border:none!important;border-radius:var(--eden-radius-medium)!important;color:var(--eden-muted-text)!important;"
            "background:transparent!important;transition:background-color var(--eden-duration) ease-out}"
            ".tabbed-pane-header-tab:hover{background:var(--eden-container-high)!important;color:var(--eden-text)!important}"
            ".tabbed-pane-header-tab.selected{background:var(--eden-container-highest)!important;"
            "color:var(--eden-text)!important}"
            ".tabbed-pane-tab-slider{display:none!important}"
            ".toolbar-button,devtools-button{border-radius:var(--eden-radius-small)}"
            "input,select,textarea,.toolbar-input{border-radius:var(--eden-radius-small)!important}"
        );
        const QString tokenJson = QString::fromUtf8(QJsonDocument(tokens).toJson(QJsonDocument::Compact));
        const QString sheetJson =
            QString::fromUtf8(QJsonDocument(QJsonObject{{"css", sheet}}).toJson(QJsonDocument::Compact));
        return QStringLiteral(
                   "(function(){try{var tokens=%1;var root=document.documentElement;"
                   "for(var name in tokens){root.style.setProperty(name,tokens[name],'important');}"
                   "var sheet=window.__edenSheet;if(!sheet){sheet=new CSSStyleSheet();window.__edenSheet=sheet;}"
                   "sheet.replaceSync(%2.css);"
                   "var adopt=function(target){if(target&&target.adoptedStyleSheets&&"
                   "target.adoptedStyleSheets.indexOf(sheet)<0){"
                   "target.adoptedStyleSheets=target.adoptedStyleSheets.concat([sheet]);}};"
                   "var walk=function(node){adopt(node);var all=node.querySelectorAll('*');"
                   "for(var index=0;index<all.length;index++){if(all[index].shadowRoot){walk(all[index].shadowRoot);}}};"
                   "walk(document);"
                   "if(!window.__edenAttach){window.__edenAttach=Element.prototype.attachShadow;"
                   "Element.prototype.attachShadow=function(options){var shadow=window.__edenAttach.call(this,options);"
                   "var host=shadow;queueMicrotask(function(){adopt(host);});return shadow;};}"
                   "}catch(error){}})()"
        )
            .arg(tokenJson, sheetJson);
    }

}
