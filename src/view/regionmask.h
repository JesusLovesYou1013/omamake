#pragma once

// Extrude's cut region over the profile's plane, as a texture for the body's shader (GB-084 correction,
// qml/shaders/body.frag): per texel the profile's signed distance in mm (inside positive), as 16 bits in R and
// G of plain RGBA8 (see fill; `range` goes to the shader). Filled by Document::extrudeRegion; kept while the
// profile stays the same, so a drag only changes the shader's numbers.

#include <QQuick3DTextureData>
#include <QtQml/qqmlregistration.h>
#include <vector>

class RegionMask : public QQuick3DTextureData {
    Q_OBJECT
public:
    explicit RegionMask(QQuick3DObject *parent = nullptr) : QQuick3DTextureData(parent) {}
    // w x h texels, 4 floats each, row by row from v's low end.
    void fill(int w, int h, const std::vector<float> &rgba);

    // Kept by Document::extrudeRegion between calls.
    std::size_t key = 0;
    int w = 0, h = 0;
    double u0 = 0, v0 = 0, cell = 1;
    std::vector<float> sd;                 // the signed distance per texel
    std::vector<int> area;                 // which area a texel is in (-1: none; the nearest one inside)
    std::vector<float> rgba;
    float range = 1;                       // the distance at full scale (mm)
};
