// Drawn in front of everything: the preview of what an operation adds (blue) or removes (red) stays
// visible through the bodies it works on (owner, t116u).
void MAIN()
{
    vec4 p = MODELVIEWPROJECTION_MATRIX * vec4(VERTEX, 1.0);
    p.z = p.w * 0.002;
    POSITION = p;
}
