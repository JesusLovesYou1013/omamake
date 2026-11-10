QT += core core-private gui gui-private qml quick quick3d

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
# Heavy engine: OpenCascade (exact shapes, STEP).
INCLUDEPATH += /usr/include/opencascade
LIBS += -lTKernel -lTKMath -lTKG2d -lTKG3d -lTKGeomBase -lTKBRep -lTKGeomAlgo -lTKTopAlgo -lTKPrim -lTKFeat -lTKFillet -lTKOffset -lTKBO -lTKBool -lTKHLR -lTKMesh -lTKShHealing -lTKDESTEP -lTKDESTL -lTKXSBase

# The app's icon (owner, GB-064): the window icon, built in; resources/icons/hicolor/ holds the sizes the
# desktop entry's Icon=omamake finds once installed (resources/icons/omamake.png is the full-size source).
RESOURCES += resources/omamake.qrc

# The QML files are read from the source tree at startup (no compile for UI changes).
DEFINES += OMAMAKE_SOURCE_QML_DIR=\\\"$$PWD/qml\\\"
# Failed operations' logs go in this folder's logs/ (dev builds; gitignored).
DEFINES += OMAMAKE_SOURCE_DIR=\\\"$$PWD\\\"

HEADERS += \
    src/document/document.h \
    src/document/expression.h \
    src/document/parameterfile.h \
    src/document/sketchgeometry.h \
    src/document/profiles.h \
    src/document/projection.h \
    src/engine/engine.h \
    src/engine/heavy/meshsolid.h \
    src/engine/heavy/refine.h \
    src/engine/heavy/pushface.h \
    src/engine/heavy/chamfer.h \
    src/engine/heavy/projectexact.h \
    src/engine/heavy/taper.h \
    src/theme/dialoglook.h \
    src/theme/omarchytheme.h \
    src/theme/transparency.h \
    src/view/meshgeometry.h \
    src/view/modifierkeys.h

SOURCES += \
    src/main.cpp \
    src/document/document.cpp \
    src/document/expression.cpp \
    src/document/parameterfile.cpp \
    src/document/sketchgeometry.cpp \
    src/document/profiles.cpp \
    src/document/projection.cpp \
    src/engine/lite/liteengine.cpp \
    src/engine/heavy/heavyengine.cpp \
    src/engine/heavy/meshsolid.cpp \
    src/engine/heavy/refine.cpp \
    src/engine/heavy/pushface.cpp \
    src/engine/heavy/chamfer.cpp \
    src/engine/heavy/projectexact.cpp \
    src/engine/heavy/taper.cpp \
    src/theme/dialoglook.cpp \
    src/theme/omarchytheme.cpp \
    src/theme/transparency.cpp \
    src/view/meshgeometry.cpp \
    src/view/modifierkeys.cpp

# Qt Quick shaders (2D effects) are compiled to .qsb beside their source, where the QML reads them.
QSB_SOURCES = $$PWD/qml/shaders/crossfade.frag
qsb.input = QSB_SOURCES
qsb.output = $$PWD/qml/shaders/${QMAKE_FILE_BASE}.frag.qsb
qsb.commands = $$[QT_HOST_BINS]/qsb --glsl \"100 es,120,150\" --hlsl 50 --msl 12 -o ${QMAKE_FILE_OUT} ${QMAKE_FILE_IN}
qsb.CONFIG += no_link target_predeps
QMAKE_EXTRA_COMPILERS += qsb
