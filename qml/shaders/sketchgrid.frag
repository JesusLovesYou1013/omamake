// Sketch grid: the floor grid's look, laid on the sketch plane. Lines every inch,
// stronger every foot, the sketch's X axis red and Y axis green, fading with distance.
VARYING vec3 vWorld;
VARYING vec2 vLocal;

float lines(vec2 p)
{
    vec2 w = fwidth(p);
    vec2 g = abs(fract(p - 0.5) - 0.5) / max(w, vec2(1e-5));
    float crowd = 1.0 - smoothstep(0.25, 0.5, max(w.x, w.y));
    return (1.0 - min(min(g.x, g.y), 1.0)) * crowd;
}

void MAIN()
{
    vec2 p = vLocal / interval;
    vec4 color = gridMinor * lines(p);
    color = mix(color, gridMajor, lines(p / 12.0));

    vec2 w = fwidth(vLocal);
    float onX = 1.0 - min(abs(vLocal.y) / max(w.y, 1e-5) / 1.2, 1.0);
    float onY = 1.0 - min(abs(vLocal.x) / max(w.x, 1e-5) / 1.2, 1.0);
    color = mix(color, axisX, onX);
    color = mix(color, axisY, onY);

    float d = length(vWorld - CAMERA_POSITION);
    color.a *= 1.0 - smoothstep(fadeDistance * 0.4, fadeDistance, d);
    FRAGCOLOR = vec4(color.rgb * color.a, color.a);
}
