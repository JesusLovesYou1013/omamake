// A sketch's shaded profile lies on the face it was drawn on, and the face's own triangles (a
// converted body's sit up to 0.06 mm off its plane) fought it, flickering as the view turned
// (owner, 2026-11-03). So it is pulled a hair toward the eye: `pull` mm plus `share` of the way,
// enough to stay over its own face, little enough that anything really in front still hides it.
// Selection.qml picks a profile over a face by the same rule.
void MAIN()
{
    vec3 world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    vec3 toEye = CAMERA_POSITION - world;
    float d = length(toEye);
    world += toEye / max(d, 1e-6) * min(d * 0.5, pull + d * share);
    POSITION = VIEWPROJECTION_MATRIX * vec4(world, 1.0);
}
