#include "regionmask.h"

#include <QSize>
#include <algorithm>
#include <cmath>

// Plain 8-bit RGBA, the one texture format every desktop's GPU takes as given (float textures arrived empty
// on this one): R and G the signed distance as 16 bits, sd = (value / 65535 - 0.5) * 2 * range mm; body.frag
// decodes each texel and blends them itself (maskAt). B and A unused.
void RegionMask::fill(int width, int height, const std::vector<float> &data)
{
    float most = 1;
    for (std::size_t i = 0; i < data.size(); i += 4)
        most = std::max(most, std::abs(data[i]));
    range = most * 1.01f;
    QByteArray bytes(qsizetype(std::size_t(width) * height * 4), Qt::Uninitialized);
    auto *out = reinterpret_cast<unsigned char *>(bytes.data());
    for (std::size_t k = 0; k < std::size_t(width) * height; ++k) {
        const double unit = std::clamp(data[k * 4] / range * 0.5 + 0.5, 0.0, 1.0);
        const unsigned v = unsigned(std::lround(unit * 65535));
        out[k * 4] = (unsigned char)(v >> 8);
        out[k * 4 + 1] = (unsigned char)(v & 0xff);
        out[k * 4 + 2] = 0;
        out[k * 4 + 3] = 255;
    }
    setSize(QSize(width, height));
    setFormat(QQuick3DTextureData::RGBA8);
    setHasTransparency(false);
    setTextureData(bytes);
}
