#version 460
#extension GL_EXT_scalar_block_layout : require

// Instanced rain streak billboard. One instance per particle, 4 vertices drawn
// as a triangle strip (no vertex buffer; the quad is generated from
// gl_VertexIndex). The particle's emitter-relative coords are read from the
// simulation SSBO and reconstructed into a world position; the quad is then a
// vertical, camera-facing streak hanging below the particle.

layout(set = 0, binding = 0, std430) readonly buffer Particles
{
    vec4 particles[];
};

layout(set = 0, binding = 1, std140) uniform RainView
{
    mat4 view_projection;
    mat4 plane_to_world;  // emitter plane: local [-1,1] XZ quad, y = 0
    vec4 cam_pos;         // xyz = camera world position
    vec4 color;           // rgb tint, a = opacity
    vec4 params;          // x = streak_width, y = streak_length, z = time, w = column_height
    vec4 depth_size;      // xy = scene-depth image dimensions
} u;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec3 out_world;

void main()
{
    uint pid = uint(gl_InstanceIndex);
    vec4 p = particles[pid];

    // Reconstruct the drop's world position from emitter-relative coords.
    vec3 plane_point = (u.plane_to_world * vec4(p.x, 0.0, p.y, 1.0)).xyz;
    vec3 drop_world = plane_point + vec3(0.0, -1.0, 0.0) * p.z;

    // Vertical billboard: length runs along world up, width faces the camera.
    const vec3 world_up = vec3(0.0, 1.0, 0.0);
    vec3 view_dir = normalize(u.cam_pos.xyz - drop_world);
    vec3 right = normalize(cross(world_up, view_dir));

    // Triangle-strip quad corners: cx in {-1,+1}, cy in {0,1}. cy = 0 sits at the
    // drop (bottom of the streak), cy = 1 trails upward (the motion streak).
    float cx = (gl_VertexIndex == 1 || gl_VertexIndex == 3) ? 1.0 : -1.0;
    float cy = (gl_VertexIndex >= 2) ? 1.0 : 0.0;

    float half_w = 0.5 * u.params.x;
    float len = u.params.y;
    vec3 corner = drop_world + right * (cx * half_w) + world_up * (cy * len);

    gl_Position = u.view_projection * vec4(corner, 1.0);
    out_uv = vec2(cx * 0.5 + 0.5, cy);
    out_world = corner;
}
