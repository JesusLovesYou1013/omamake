VARYING vec3 vWorld;

void MAIN()
{
    vWorld = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    POSITION = MODELVIEWPROJECTION_MATRIX * vec4(VERTEX, 1.0);
}
