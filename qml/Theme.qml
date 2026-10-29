import QtQuick

// Every colour in OmaMake comes from here. See HANDOFF.md "The look: rules for new features".
//
// Three sources, picked by two switches (both remembered in omamake.conf, [look]):
//   light          false = dark look, true = light look (moon/sun button, top right)
//   followSystem   true = colours come from the current Omarchy theme (right-click that button)
// Built-in dark is OmaShow's dark blue palette exactly (omashow src/qml/Theme.qml);
// built-in light is its soft counterpart (no pure white anywhere, so nothing glares).
// Following Omarchy: the theme's background hue makes the space and panels (a little
// darker than the theme in the dark look, a soft light tint of it in the light look),
// its foreground makes the text, and its accent makes the model, toned so it stands
// off the background without glowing.
QtObject {
    id: theme

    property bool light: false
    property bool followSystem: false
    // Only on Omarchy; elsewhere the option isn't offered and the saved switch is ignored.
    readonly property bool following: followSystem && omarchyTheme.onOmarchy && omarchyTheme.available

    readonly property var builtInDark: ({
        windowBg: "#0B0E13", panelBg: "#111722", panelRaised: "#171E2A", controlBg: "#1B2431",
        controlHover: "#222D3D", border: "#293241", borderStrong: "#3A4658",
        textPrimary: "#E7EDF7", textSecondary: "#A7B1C1", textMuted: "#727E90",
        accent: "#27C2FF", warning: "#FBBF24", danger: "#EF4444", success: "#34D399",
        model: "#7F95C2", modelEdge: "#0B0E13", hover: "#A8E4FF", selection: "#1FA8F0",
        gridMinor: Qt.rgba(0.72, 0.78, 0.88, 0.16), gridMajor: Qt.rgba(0.72, 0.78, 0.88, 0.34),
        axisX: "#EF4444", axisY: "#34D399"
    })
    readonly property var builtInLight: ({
        windowBg: "#D5DCE6", panelBg: "#E6EBF1", panelRaised: "#EDF1F6", controlBg: "#DDE3EB",
        controlHover: "#D0D8E3", border: "#BCC6D3", borderStrong: "#9DA9B9",
        textPrimary: "#151C27", textSecondary: "#3B4759", textMuted: "#677386",
        accent: "#0A86C4", warning: "#B7791F", danger: "#C53030", success: "#1F8A5B",
        model: "#6681B3", modelEdge: "#1A2333", hover: "#86D3F7", selection: "#0A72C4",
        gridMinor: Qt.rgba(0.20, 0.26, 0.36, 0.13), gridMajor: Qt.rgba(0.20, 0.26, 0.36, 0.38),
        axisX: "#D13B3B", axisY: "#1F9A62"
    })

    // An Omarchy colour by name, with a fallback when a theme leaves one out.
    function sys(name, fallback) {
        const v = omarchyTheme.colors[name]
        return v ? Qt.color(v) : Qt.color(fallback)
    }
    function hue(c) { return c.hslHue < 0 ? 0 : c.hslHue }
    function hsl(h, s, l, a) {
        return Qt.hsla(h, Math.max(0, Math.min(1, s)), Math.max(0, Math.min(1, l)), a === undefined ? 1 : a)
    }
    // The same colour moved to a lightness, keeping its hue.
    function atLightness(c, l, satScale) {
        return hsl(hue(c), c.hslSaturation * (satScale === undefined ? 1 : satScale), l)
    }

    // Selection must never look like the model (which is made from the accent): use the
    // theme's blue when its hue is clearly different, else the accent's opposite hue.
    function pickColour(acc, lightness) {
        const blue = sys("blue", "#4080e0")
        const gap = Math.abs(hue(blue) - hue(acc))
        const h = Math.min(gap, 1 - gap) > 0.12 ? hue(blue) : (hue(acc) + 0.5) % 1
        return hsl(h, 0.78, lightness)
    }

    function fromOmarchy(isLight) {
        const bg = sys("background", "#101010")
        const fg = sys("foreground", "#d0d0d0")
        const acc = sys("accent", "#4090e0")
        const h = hue(bg), s = Math.min(bg.hslSaturation, isLight ? 0.3 : 0.6)
        const red = sys("red", "#d05050"), green = sys("green", "#40a060")

        if (!isLight) {
            // A little darker than the theme's own background.
            const L = Math.max(0.025, bg.hslLightness * 0.7)
            const up = d => hsl(h, s, L + d)
            const accent = acc.hslLightness < 0.5 ? atLightness(acc, 0.55) : acc
            return {
                windowBg: up(0), panelBg: up(0.03), panelRaised: up(0.055), controlBg: up(0.075),
                controlHover: up(0.11), border: up(0.15), borderStrong: up(0.23),
                textPrimary: sys("bright_foreground", fg.toString()), textSecondary: fg,
                textMuted: sys("muted", Qt.darker(fg, 1.5).toString()),
                accent: accent, warning: sys("yellow", "#d0b040"), danger: red, success: green,
                model: atLightness(acc, 0.6, 0.55), modelEdge: hsl(h, s, L * 0.6),
                hover: hsl(hue(acc), 0.85, 0.78), selection: pickColour(acc, 0.56),
                gridMinor: hsl(hue(fg), fg.hslSaturation * 0.6, 0.78, 0.15),
                gridMajor: hsl(hue(fg), fg.hslSaturation * 0.6, 0.78, 0.32),
                axisX: red, axisY: green
            }
        }
        // A soft light tint of the theme: a little lighter than its own background
        // when the theme is already light, never brighter than about 93%.
        const L = bg.hslLightness > 0.6 ? Math.min(0.93, bg.hslLightness + (1 - bg.hslLightness) * 0.3) : 0.86
        const at = d => hsl(h, s, L + d)
        const ink = (l, sat) => hsl(h, Math.min(bg.hslSaturation, sat), l)
        return {
            windowBg: at(0), panelBg: at(0.045), panelRaised: at(0.065), controlBg: at(0.025),
            controlHover: at(-0.03), border: at(-0.11), borderStrong: at(-0.2),
            textPrimary: ink(0.12, 0.35), textSecondary: ink(0.28, 0.3), textMuted: ink(0.45, 0.2),
            accent: atLightness(acc, Math.min(acc.hslLightness, 0.4)),
            warning: atLightness(sys("yellow", "#b08020"), 0.38), danger: atLightness(red, 0.42),
            success: atLightness(green, 0.33),
            model: atLightness(acc, 0.47, 0.6), modelEdge: ink(0.14, 0.4),
            hover: hsl(hue(acc), 0.75, 0.62), selection: pickColour(acc, 0.4),
            gridMinor: ink(0.25, 0.3), gridMajor: ink(0.25, 0.3),
            axisX: atLightness(red, 0.45), axisY: atLightness(green, 0.36)
        }
    }

    readonly property var p: following ? fromOmarchy(light) : (light ? builtInLight : builtInDark)
    // Light grid lines get their see-through amount here (the colour above is the ink).
    readonly property real lightGridMinorAlpha: 0.13
    readonly property real lightGridMajorAlpha: 0.38

    readonly property color windowBg: p.windowBg
    readonly property color panelBg: p.panelBg
    readonly property color panelRaised: p.panelRaised
    readonly property color controlBg: p.controlBg
    readonly property color controlHover: p.controlHover
    readonly property color border: p.border
    readonly property color borderStrong: p.borderStrong
    readonly property color textPrimary: p.textPrimary
    readonly property color textSecondary: p.textSecondary
    readonly property color textMuted: p.textMuted
    readonly property color accent: p.accent
    readonly property color warning: p.warning
    readonly property color danger: p.danger
    readonly property color success: p.success

    // The 3D space.
    readonly property color model: p.model
    readonly property color modelEdge: p.modelEdge
    readonly property color hover: p.hover            // part under the mouse
    readonly property color selection: p.selection    // selected parts
    readonly property color gridMinor: following && light ? Qt.rgba(p.gridMinor.r, p.gridMinor.g, p.gridMinor.b, lightGridMinorAlpha) : p.gridMinor
    readonly property color gridMajor: following && light ? Qt.rgba(p.gridMajor.r, p.gridMajor.g, p.gridMajor.b, lightGridMajorAlpha) : p.gridMajor
    readonly property color axisX: p.axisX
    readonly property color axisY: p.axisY
}
