#version 450
// Converts the ray-tracer's R32_SFLOAT linear hit-distance image into
// hardware NDC depth [0,1] so that Effekseer effects are properly occluded
// by scene geometry.  The image is bound as a storage image (VK_IMAGE_USAGE_STORAGE_BIT)
// because the ray-tracer does not create it with SAMPLED usage.
layout(set = 0, binding = 0, r32f) readonly uniform image2D scene_linear_depth;

layout(push_constant) uniform PushConstants
{
    float z_near;
    float z_far;
} pc;

void main()
{
    ivec2 coord = ivec2(gl_FragCoord.xy);
    float d = imageLoad(scene_linear_depth, coord).r;

    // Sky / miss rays write 1e30; treat as far plane so they never occlude.
    if (d >= 1e29)
    {
        gl_FragDepth = 1.0;
        return;
    }

    d = max(d, pc.z_near);

    // NDC depth from Effekseer's PerspectiveFovRH (Vulkan [0,1] range):
    //   P[2][2] = zf / (zn - zf)
    //   P[3][2] = zn * zf / (zn - zf)
    //   w_clip  = -z_view = d
    //   z_clip  = P[2][2]*(-d) + P[3][2] = zf*(zn-d)/(zn-zf)
    //   ndc_z   = z_clip / w_clip = zf*(zn-d) / ((zn-zf)*d)
    float ndc_z = pc.z_far * (pc.z_near - d) / ((pc.z_near - pc.z_far) * d);
    gl_FragDepth = clamp(ndc_z, 0.0, 1.0);
}
