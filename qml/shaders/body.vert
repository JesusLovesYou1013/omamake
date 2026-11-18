// The body's own (model) coordinates, the engine's mm with Z up, handed to body.frag, so Extrude's cut
// region is tested where the engine puts it, whatever the scene's turn (GB-084 correction).
VARYING vec3 objPos;
void MAIN()
{
    objPos = VERTEX;
    POSITION = MODELVIEWPROJECTION_MATRIX * vec4(VERTEX, 1.0);
}
