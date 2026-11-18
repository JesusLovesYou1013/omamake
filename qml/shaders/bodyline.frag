// Edge lines for the body's look with Extrude's cut region (GB-084 second correction): role 0, the bodies'
// edges as they are, none inside the region; role 1, the edges of the bodies as the cut leaves them, only
// inside it. The region test is body.frag's, copied (custom shaders can't share a file): keep the two in step.
VARYING vec3 objPos;

// A face lying in the start or end plane counts as inside, and so does one on the profile's own outline (a wall
// the cut takes away), within `edge`: three quarters of a mask texel, the field's own accuracy.
// (body.frag gives a lifted picked part more room off the start plane; lines here are never lifted.)
bool inSide(vec4 s, float w, float sd, float from, float to, float edge)
{
    if (s.x < 0.5)
        return false;
    const float kOwn = 0.02;
    float lo = min(from, to) - kOwn, hi = max(from, to) + kOwn;
    return w >= lo && w <= hi && sd + s.w * abs(w - from) >= -edge;
}
// The signed distance between the mask's texels: each texel decoded (16 bits in R and G, see RegionMask::fill)
// and blended here, so nothing is asked of the GPU's filtering or mip levels.
float sdAt(ivec2 at, ivec2 hi)
{
    vec4 t = texelFetch(regionMask, clamp(at, ivec2(0), hi), 0);
    float unit = (floor(t.r * 255.0 + 0.5) * 256.0 + floor(t.g * 255.0 + 0.5)) / 65535.0;
    return (unit - 0.5) * 2.0 * regionRange;
}
float maskAt(vec2 uv)
{
    ivec2 n = textureSize(regionMask, 0);
    vec2 t = uv * vec2(n) - 0.5;
    ivec2 a = ivec2(floor(t));
    vec2 f = t - vec2(a);
    ivec2 hi = n - ivec2(1);
    return mix(mix(sdAt(a, hi), sdAt(a + ivec2(1, 0), hi), f.x), mix(sdAt(a + ivec2(0, 1), hi), sdAt(a + ivec2(1, 1), hi), f.x), f.y);
}
bool insideCut(vec3 p)
{
    vec3 d = p - rO;
    float u = dot(d, rX), v = dot(d, rY), w = dot(d, rN);
    vec2 uv = vec2((u - regionGrid.x) * regionGrid.z, (v - regionGrid.y) * regionGrid.w);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return false;
    float sd = maskAt(uv);
    float edge = 0.75 / (regionGrid.z * float(textureSize(regionMask, 0).x));
    return inSide(s0, w, sd, s0.y, s0.z, edge) || inSide(s1, w, sd, s1.y, s1.z, edge);
}
void MAIN()
{
    bool inside = regionOn > 0.5 && insideCut(objPos);
    if (inside != (role > 0.5))
        discard;
    FRAGCOLOR = vec4(tint.rgb, 1.0);
}
