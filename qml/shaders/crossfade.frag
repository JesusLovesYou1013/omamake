#version 440
// The view going from before to after, pixel by pixel (Main.qml's crossfade). A straight
// mix of the two pictures, so see-through parts of the window stay exactly as see-through.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float progress;
};
layout(binding = 1) uniform sampler2D before;
layout(binding = 2) uniform sampler2D after;

void main()
{
    fragColor = mix(texture(before, qt_TexCoord0), texture(after, qt_TexCoord0), progress) * qt_Opacity;
}
