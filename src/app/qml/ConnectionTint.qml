pragma ComponentBehavior: Bound
import QtQuick
import Quarry.Controls

// The color a connection was given, as a background: `fill` paints the row of the connection and
// its tabs, `ink` is the text and icon color that reads on it (WCAG AA, Theme.onColor). Without
// a color (colorIndex < 0) `active` is false and nothing should be tinted.
QtObject {
    property int colorIndex: -1

    readonly property bool active: colorIndex >= 0
    readonly property color fill: {
        const themeChanged = Theme.bgCanvas; // connectionColor() is a function: this ties the binding to the theme
        return active && themeChanged.a >= 0 ? Theme.connectionColor(colorIndex) : Qt.alpha(Theme.bgSurface, 0);
    }
    readonly property color ink: active ? Theme.onColor(fill) : Theme.fgDefault
}
