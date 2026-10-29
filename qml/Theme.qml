import QtQuick

// Two looks: dark is OmaShow's dark blue palette exactly (omashow src/qml/Theme.qml);
// light is its soft counterpart (no pure white anywhere, so nothing glares).
// "Follow the Omarchy theme" comes later.
QtObject {
    property bool light: false

    readonly property color windowBg: light ? "#D5DCE6" : "#0B0E13"
    readonly property color panelBg: light ? "#E6EBF1" : "#111722"
    readonly property color panelRaised: light ? "#EDF1F6" : "#171E2A"
    readonly property color controlBg: light ? "#DDE3EB" : "#1B2431"
    readonly property color controlHover: light ? "#D0D8E3" : "#222D3D"
    readonly property color border: light ? "#BCC6D3" : "#293241"
    readonly property color borderStrong: light ? "#9DA9B9" : "#3A4658"
    readonly property color textPrimary: light ? "#151C27" : "#E7EDF7"
    readonly property color textSecondary: light ? "#3B4759" : "#A7B1C1"
    readonly property color textMuted: light ? "#677386" : "#727E90"
    readonly property color accent: light ? "#0A86C4" : "#27C2FF"
    readonly property color warning: light ? "#B7791F" : "#FBBF24"
    readonly property color danger: light ? "#C53030" : "#EF4444"
    readonly property color success: light ? "#1F8A5B" : "#34D399"

    // The 3D space. The model is a deeper blue in the light look so it stands off the background.
    readonly property color model: light ? "#6681B3" : "#7F95C2"
    readonly property color modelEdge: light ? "#1A2333" : "#0B0E13"
    readonly property color gridMinor: light ? Qt.rgba(0.20, 0.26, 0.36, 0.13) : Qt.rgba(0.72, 0.78, 0.88, 0.16)
    readonly property color gridMajor: light ? Qt.rgba(0.20, 0.26, 0.36, 0.38) : Qt.rgba(0.72, 0.78, 0.88, 0.34)
    readonly property color axisX: light ? "#D13B3B" : "#EF4444"
    readonly property color axisY: light ? "#1F9A62" : "#34D399"
}
