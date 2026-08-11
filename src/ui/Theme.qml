import Eden.Ui
import QtQuick
pragma Singleton

QtObject {
    readonly property bool dark: Settings.theme === "dark" || (Settings.theme === "system" && Settings.systemDark)
    readonly property color primary: Themes.primary
    readonly property color primaryText: Themes.primaryText
    readonly property color primaryContainer: Themes.primaryContainer
    readonly property color primaryContainerText: Themes.primaryContainerText
    readonly property color surface: Themes.surface
    readonly property color surfaceContainerLow: Themes.surfaceContainerLow
    readonly property bool toolbarGradientEnabled: Themes.toolbarGradientEnabled
    readonly property real toolbarGradientAngle: Themes.toolbarGradientAngle
    readonly property color toolbarGradientStart: Themes.toolbarGradientStart
    readonly property color toolbarGradientMiddle: Themes.toolbarGradientMiddle
    readonly property color toolbarGradientEnd: Themes.toolbarGradientEnd
    readonly property real toolbarGradientMiddlePosition: Themes.toolbarGradientMiddlePosition
    readonly property color tabBarBackground: Themes.tabBarBackground
    readonly property color surfaceContainer: Themes.surfaceContainer
    readonly property color surfaceContainerHigh: Themes.surfaceContainerHigh
    readonly property color surfaceContainerHighest: Themes.surfaceContainerHighest
    readonly property color surfaceText: Themes.surfaceText
    readonly property color surfaceVariantText: Themes.surfaceVariantText
    readonly property color disabledText: Themes.disabledText
    readonly property color outline: Themes.outline
    readonly property color windowBorder: Themes.windowBorder
    readonly property color contentBorder: Themes.contentBorder
    readonly property color paneBorder: Themes.paneBorder
    readonly property color menuBorder: Themes.menuBorder
    readonly property color focusBorder: Themes.focusBorder
    readonly property color error: Themes.error
    readonly property color privatePrimary: Themes.privatePrimary
    readonly property color privateBackground: Themes.privateBackground
    readonly property color completionHint: Themes.completionHint
    readonly property color iconColor: Themes.iconColor
    readonly property color disabledIconColor: Themes.disabledIconColor
    readonly property color windowShadowColor: Themes.windowShadowColor
    readonly property color overlayShadowColor: Themes.overlayShadowColor
    readonly property int windowRadius: Themes.windowRadius
    readonly property int contentRadius: Themes.contentRadius
    readonly property int cardRadius: Themes.cardRadius
    readonly property int controlRadius: Themes.controlRadius
    readonly property int menuRadius: Themes.menuRadius
    readonly property int suggestionRadius: Themes.suggestionRadius
    readonly property int workspaceInset: Themes.workspaceInset
    readonly property int workspaceGap: Themes.workspaceGap
    readonly property int tabMinimumWidth: Themes.tabMinimumWidth
    readonly property int tabMaximumWidth: Themes.tabMaximumWidth
    readonly property int pinnedTabWidth: Themes.pinnedTabWidth
    readonly property int windowBorderWidth: Themes.windowBorderWidth
    readonly property int contentBorderWidth: Themes.contentBorderWidth
    readonly property int paneBorderWidth: Themes.paneBorderWidth
    readonly property int menuBorderWidth: Themes.menuBorderWidth
    readonly property int focusBorderWidth: Themes.focusBorderWidth
    readonly property int windowShadowExtent: Themes.windowShadowExtent
    readonly property real windowShadowBlur: Themes.windowShadowBlur
    readonly property real windowShadowSpread: Themes.windowShadowSpread
    readonly property real windowShadowVerticalOffset: Themes.windowShadowVerticalOffset
    readonly property real windowShadowHorizontalOffset: Themes.windowShadowHorizontalOffset
    readonly property color windowShadowAmbientColor: Themes.windowShadowAmbientColor
    readonly property real windowShadowAmbientBlur: Themes.windowShadowAmbientBlur
    readonly property real windowShadowAmbientVerticalOffset: Themes.windowShadowAmbientVerticalOffset
    readonly property real overlayShadowBlur: Themes.overlayShadowBlur
    readonly property real overlayShadowSpread: Themes.overlayShadowSpread
    readonly property real overlayShadowVerticalOffset: Themes.overlayShadowVerticalOffset
    readonly property string iconStyle: Themes.iconStyle
    readonly property string activeIconStyle: Themes.activeIconStyle
    readonly property int iconSize: Themes.iconSize
    readonly property real disabledIconOpacity: Themes.disabledIconOpacity
    readonly property int shortDuration: Themes.shortDuration
    readonly property int mediumDuration: Themes.mediumDuration
    readonly property int longDuration: Themes.longDuration
    readonly property font bodyFont: Qt.font({
        "family": Themes.fontFamily,
        "pixelSize": Themes.bodyFontSize
    })
    readonly property font labelFont: Qt.font({
        "family": Themes.fontFamily,
        "pixelSize": Themes.labelFontSize,
        "weight": Font.DemiBold
    })
    readonly property font titleFont: Qt.font({
        "family": Themes.fontFamily,
        "pixelSize": Themes.titleFontSize,
        "weight": Font.DemiBold
    })
}
