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
    src/theme/omarchytheme.h \
    src/theme/transparency.h \
    src/view/meshgeometry.h

SOURCES += \
    src/main.cpp \
    src/document/document.cpp \
    src/engine/lite/liteengine.cpp \
    src/theme/omarchytheme.cpp \
    src/theme/transparency.cpp \
    src/view/meshgeometry.cpp

# Qt Quick shaders (2D effects) are compiled to .qsb beside their source, where the QML reads them.
QSB_SOURCES = $$PWD/qml/shaders/crossfade.frag
qsb.input = QSB_SOURCES
qsb.output = $$PWD/qml/shaders/${QMAKE_FILE_BASE}.frag.qsb
qsb.commands = $$[QT_HOST_BINS]/qsb --glsl \"100 es,120,150\" --hlsl 50 --msl 12 -o ${QMAKE_FILE_OUT} ${QMAKE_FILE_IN}
qsb.CONFIG += no_link target_predeps
QMAKE_EXTRA_COMPILERS += qsb
