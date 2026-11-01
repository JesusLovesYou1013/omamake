#pragma once

// The system file dialog (Save As) in the app's own look, dark or light (owner, 2026-10-29: a
// bright dialog over the dark look is not OK). On Omarchy that dialog is GTK's, drawn inside
// this process by Qt's gtk3 platform theme, and left alone it comes up in GTK's light default
// whenever GTK can't see the desktop's own setting. setDark tells GTK which one to use, just
// before the dialog opens. While it's open the look is locked (Theme.locked), so the two can't
// drift apart. GTK is found at run time (it's only there when Qt loaded it), so it isn't needed
// to build or run; without it this does nothing.

#include <QObject>

class DialogLook : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    Q_INVOKABLE void setDark(bool dark);
};
