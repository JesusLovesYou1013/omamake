QT += core gui qml quick quick3d

CONFIG += c++20 release
TARGET = omamake
TEMPLATE = app
# Build out of tree: cd build && qmake6 .. && make -j2

# Fast rebuilds: ccache skips anything compiled before, mold links in a blink.
QMAKE_CXX = ccache g++
QMAKE_LINK = g++
QMAKE_LFLAGS += -fuse-ld=mold

INCLUDEPATH += src
LIBS += -lmanifold

# The QML files are read from the source tree at startup (no compile for UI changes).
DEFINES += OMAMAKE_SOURCE_QML_DIR=\\\"$$PWD/qml\\\"

HEADERS += \
    src/document/document.h \
    src/engine/engine.h \
    src/theme/transparency.h \
    src/view/meshgeometry.h

SOURCES += \
    src/main.cpp \
    src/document/document.cpp \
    src/engine/lite/liteengine.cpp \
    src/theme/transparency.cpp \
    src/view/meshgeometry.cpp
