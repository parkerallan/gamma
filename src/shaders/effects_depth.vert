#version 450
// Full-screen triangle; no vertex buffer needed.
void main()
{
    // Generates the vertices of a triangle that covers the entire screen.
    // Vertex 0: (-1,-1)  Vertex 1: (3,-1)  Vertex 2: (-1, 3)
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
