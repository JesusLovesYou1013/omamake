VARYING vec3 vWorld;
VARYING vec2 vLocal;

void MAIN()
{
    vWorld = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    vLocal = VERTEX.xy * planeScale;   // millimetres along the sketch's own X and Y
    POSITION = MODELVIEWPROJECTION_MATRIX * vec4(VERTEX, 1.0);
}
