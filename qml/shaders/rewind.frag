// The Timeline's rewind (owner, 2026-11-07): one step's before and after shapes drawn together, split
// where the extrude has got to: the after shape keeps what lies between `low` and `high` along its
// direction from its plane (keepInside 1), the before shape the rest (keepInside 0), so a join
// grows out of its sketch and a cut sinks in from it as the marker moves. A glowing band marks the
// split, and the shape's inside, seen through the opening, glows instead of looking hollow.
void MAIN()
{
    // Only over the areas (box, in the plane's directions) is anything split; the after shape owns the
    // plane itself (by `kOwn` mm), so the two never both leave a point of a face lying in it.
    vec3 d = VAR_WORLD_POSITION - origin;
    float h = dot(d, axis);
    float u = dot(d, xDir), v = dot(d, yDir);
    const float kOwn = 0.01;
    bool over = u >= box.x && u <= box.y && v >= box.z && v <= box.w;
    bool inside = over && h >= low - kOwn && h <= high + kOwn;
    if ((keepInside > 0.5) != inside)
        discard;
    float band = max(high > 0.0 ? 1.0 - smoothstep(0.0, glowWidth, abs(h - high)) : 0.0,
                     low < 0.0 ? 1.0 - smoothstep(0.0, glowWidth, abs(h - low)) : 0.0) * glowOn * (over ? 1.0 : 0.0);
    if (gl_FrontFacing) {
        BASE_COLOR = vec4(tint.rgb, 1.0);
        METALNESS = metal;
        ROUGHNESS = rough;
        EMISSIVE_COLOR = glow.rgb * band * 1.4;
    } else {
        BASE_COLOR = vec4(glow.rgb * 0.15, 1.0);
        METALNESS = 0.0;
        ROUGHNESS = 0.6;
        EMISSIVE_COLOR = glow.rgb * (0.55 + 0.9 * band);
    }
}
