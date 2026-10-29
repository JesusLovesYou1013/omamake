// Floor grid: thin lines every inch, stronger every foot, red/green axes,
// fading out with distance. Drawn per pixel, so it stays sharp at any zoom.
VARYING vec3 vWorld;

float lines(vec2 p)
{
    vec2 w = fwidth(p);
    vec2 g = abs(fract(p - 0.5) - 0.5) / max(w, vec2(1e-5));
    // Lines thinner than a pixel apart would turn into a grey wash: fade them out.
    float crowd = 1.0 - smoothstep(0.25, 0.5, max(w.x, w.y));
    return (1.0 - min(min(g.x, g.y), 1.0)) * crowd;
}

void MAIN()
{
    vec2 p = vec2(vWorld.x, vWorld.z) / interval;
    float minor = lines(p);
    float major = lines(p / 12.0);

    vec4 color = gridMinor * minor;
    color = mix(color, gridMajor, major);

    // Axes: engine X runs along scene x (where z = 0), engine Y along scene -z (where x = 0).
    vec2 w = fwidth(vWorld.xz);
    float onX = 1.0 - min(abs(vWorld.z) / max(w.y, 1e-5) / 1.2, 1.0);
    float onY = 1.0 - min(abs(vWorld.x) / max(w.x, 1e-5) / 1.2, 1.0);
    color = mix(color, axisX, onX);
    color = mix(color, axisY, onY);

    float d = length(vWorld - CAMERA_POSITION);
    color.a *= 1.0 - smoothstep(fadeDistance * 0.4, fadeDistance, d);
    FRAGCOLOR = vec4(color.rgb * color.a, color.a);
}
