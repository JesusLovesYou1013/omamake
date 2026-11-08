// The model's outline during the Timeline's rewind: kept on the same side of the split as its shape
// (shaders/rewind.frag), brightening into the glow at the split.
VARYING vec3 vWorld;
void MAIN()
{
    // Only over the areas (box, in the plane's directions) is anything split; the after shape owns the
    // plane itself (by `kOwn` mm), so the two never both leave a point of a face lying in it.
    vec3 d = vWorld - origin;
    float h = dot(d, axis);
    float u = dot(d, xDir), v = dot(d, yDir);
    const float kOwn = 0.01;
    bool over = u >= box.x && u <= box.y && v >= box.z && v <= box.w;
    bool inside = over && h >= low - kOwn && h <= high + kOwn;
    if ((keepInside > 0.5) != inside)
        discard;
    float band = max(high > 0.0 ? 1.0 - smoothstep(0.0, glowWidth, abs(h - high)) : 0.0,
                     low < 0.0 ? 1.0 - smoothstep(0.0, glowWidth, abs(h - low)) : 0.0) * glowOn * (over ? 1.0 : 0.0);
    FRAGCOLOR = vec4(mix(ink.rgb, glow.rgb, band), 1.0);
}
