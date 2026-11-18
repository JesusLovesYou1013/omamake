// The body's look (as its PrincipledMaterial was: tint, metalness, roughness), with Extrude's cut region
// (owner, GB-084 corrections): while a cut shows, what lies inside the cut's volume is drawn as the cut will
// leave it; the rest of the body exactly as always. The volume is the profile pushed along its plane's normal: `regionMask`
// holds the profile's signed distance (mm, inside positive) over the plane (u, v along rX, rY from rO), and
// each side's start and end along rN (with distance per area and the lengths differing: all the areas'
// together, Document::extrudeRegion);
// a taper widens it by tan(angle) per mm from the start (Fusion's way round). Positions are the model's own
// (engine mm, Z up: body.vert's objPos).
// role 0: the bodies as they are, with a hole where the cut's region is; 1: the bodies as the cut will leave
// them (pocket walls and floor, solid), only inside the region; 2: a lit or picked part, none inside the region.
// Lines: bodyline.frag, the same test (keep the two in step).
VARYING vec3 objPos;

// A face lying in the start or end plane counts as inside, and so does one on the profile's own outline (a wall
// the cut takes away), within `edge`: three quarters of a mask texel, the field's own accuracy.
// `own`: how far off the start or end plane still counts as in it (mm): 0.02 for the bodies; a lit or picked
// part is lifted off its face (Document::fillParts: 0.3 of its tube's radius, under a pixel), so for it a pixel
// and a half's worth more, else the picked start face stayed over the cut at full strength (owner, t389u).
bool inSide(vec4 s, float w, float sd, float from, float to, float edge, float own)
{
    if (s.x < 0.5)
        return false;
    float lo = min(from, to) - own, hi = max(from, to) + own;
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
    float own = role > 1.5 ? 0.02 + 1.5 * length(fwidth(p)) : 0.02;
    return inSide(s0, w, sd, s0.y, s0.z, edge, own) || inSide(s1, w, sd, s1.y, s1.z, edge, own);
}
void MAIN()
{
    bool inside = regionOn > 0.5 && insideCut(objPos);
    // role 0 (the bodies as they are) and 2 (lit / picked parts on them): nothing inside the cut's region;
    // role 1 (the bodies as the cut leaves them, Document::fillExtrudeCutResult): nothing outside it.
    if (inside != (role > 0.5 && role < 1.5))
        discard;
    BASE_COLOR = vec4(tint.rgb, 1.0);
    METALNESS = metal;
    ROUGHNESS = rough;
}
