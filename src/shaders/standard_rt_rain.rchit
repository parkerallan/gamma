#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// Rain-on-window closest-hit shader.
//
// Drop field ported from Martijn Steinrucken's "Heartfelt" Shadertoy
// (https://www.shadertoy.com/view/ltffzl). Only the rain drops are kept: the heart
// shape, the start/end time fades, the lightning/thunder flash, the blue colour
// grade, the vignette AND the foggy-glass blur are all dropped. The author's
// drop-field functions (N13, N, Saw, DropLayer2, StaticDrops, Drops) are
// reproduced verbatim — they are the look.
//
// The original samples a flat background image, offsetting the lookup by the drop
// normal (refraction). Here there is no background image: the glass is a BOX proxy
// (cube.glb, local [-1,1]^3) in a path tracer — squash it thin to make a window
// pane. The drops live on the hit (front) face and bump its normal; the surface is
// shaded as a real glass window: a Snell-refracted ray that travels through the
// box's real depth to the back face (true geometric THICKNESS — the view is
// displaced by the actual glass depth) and a Fresnel reflection ray, plus a
// path-length Beer-Lambert absorption tint. No blur/fog.
//
// Runs when a ray hits a mesh whose material shader type is Rain
// (instance shader_type == 4, SBT hit-group offset 6).

struct PrimaryPayload
{
    vec4 color;
    float hit_distance;
    uint depth;
    vec3 pos_ws_curr;
    vec3 pos_ws_prev;
    vec3 shading_normal;
    uint material_flags;
};

layout(location = 0) rayPayloadInEXT PrimaryPayload primary_payload;
hitAttributeEXT vec2 hit_attributes;

layout(set = 0, binding = 0) uniform accelerationStructureEXT top_level_as;
layout(set = 0, binding = 2, std140) uniform SceneUniforms
{
    mat4 view_inverse;
    mat4 projection_inverse;
    vec4 ambient_light;
    vec4 directional_light_color;
    vec4 directional_light_direction;
    vec4 directional_light_data;
    vec4 point_light_color;
    vec4 point_light_position;
    vec4 point_light_data;
    vec4 spot_light_color;
    vec4 spot_light_direction;
    vec4 spot_light_position;
    vec4 spot_light_data;
    vec4 grid_data;
    vec4 grid_origin_extent;
    vec4 skybox_data;
    uvec4 counts;
    uvec4 accumulation_data;
    vec4 animation_time_data;
    mat4 prev_view_projection;
    mat4 current_view_projection;
    vec4 taa_params;
    vec4 jitter_offset;
    vec4 adaptive_params;
    vec4 underwater_data;
    mat4 underwater_world_to_local;
    uvec4 cloud_count;
    vec4 cloud_params[8];
} scene_uniforms;

// ---- Tunables -------------------------------------------------------------
// Drop density, in field units per world unit (scale-adaptive — see main). Larger
// = more, smaller drops. 0.5 matches the original look at the plane's default
// scale (a 2x2 world-unit plane shows ~2 drop rows) and holds that drop size as
// the window is scaled up/down.
#define RAIN_TILING        0.5
// How much the drops/trails bump the glass surface normal (lens strength).
#define RAIN_DROP_BUMP     1.5
// ---- Glass-window material ----
// Index of refraction (window glass ~1.45-1.52). Drives both the see-through
// Snell refraction and the Fresnel reflection amount.
#define RAIN_IOR           1.45
// Transmission: weight of the see-through (refracted) view. 1 = fully clear glass.
#define RAIN_TRANSMISSION  1.0
// Reflection strength multiplier (the Fresnel sky/scene reflection that reads as
// glass). 0 = no reflection.
#define RAIN_REFLECT       1.0
// Absorption strength per world unit of glass travelled (the Beer-Lambert tint).
// Thickness itself is REAL geometry now — it comes from the box's depth/scale, not
// a constant. Larger = more tinted/darker through the same pane.
#define RAIN_ABSORB        0.5
// Glass colour the volume absorbs toward (subtle green like real glass edges).
#define RAIN_TINT          vec3(0.80, 0.94, 0.86)
// Continuation recursion guard (matches the other transparent RT materials).
#define RAIN_MAX_DEPTH     8u

#define S(a, b, t) smoothstep(a, b, t)

// ---- Heartfelt drop field (verbatim) --------------------------------------
vec3 N13(float p)
{
    //  from DAVE HOSKINS
    vec3 p3 = fract(vec3(p) * vec3(.1031, .11369, .13787));
    p3 += dot(p3, p3.yzx + 19.19);
    return fract(vec3((p3.x + p3.y) * p3.z, (p3.x + p3.z) * p3.y, (p3.y + p3.z) * p3.x));
}

float N(float t)
{
    return fract(sin(t * 12345.564) * 7658.76);
}

float Saw(float b, float t)
{
    return S(0., b, t) * S(1., b, t);
}

vec2 DropLayer2(vec2 uv, float t)
{
    vec2 UV = uv;

    uv.y += t * 0.75;
    vec2 a = vec2(6., 1.);
    vec2 grid = a * 2.;
    vec2 id = floor(uv * grid);

    float colShift = N(id.x);
    uv.y += colShift;

    id = floor(uv * grid);
    vec3 n = N13(id.x * 35.2 + id.y * 2376.1);
    vec2 st = fract(uv * grid) - vec2(.5, 0);

    float x = n.x - .5;

    float y = UV.y * 20.;
    float wiggle = sin(y + sin(y));
    x += wiggle * (.5 - abs(x)) * (n.z - .5);
    x *= .7;
    float ti = fract(t + n.z);
    y = (Saw(.85, ti) - .5) * .9 + .5;
    vec2 p = vec2(x, y);

    float d = length((st - p) * a.yx);

    float mainDrop = S(.4, .0, d);

    float r = sqrt(S(1., y, st.y));
    float cd = abs(st.x - x);
    float trail = S(.23 * r, .15 * r * r, cd);
    float trailFront = S(-.02, .02, st.y - y);
    trail *= trailFront * r * r;

    y = UV.y;
    float trail2 = S(.2 * r, .0, cd);
    float droplets = max(0., (sin(y * (1. - y) * 120.) - st.y)) * trail2 * trailFront * n.z;
    y = fract(y * 10.) + (st.y - .5);
    float dd = length(st - vec2(x, y));
    droplets = S(.3, 0., dd);
    float m = mainDrop + droplets * r * trailFront;

    return vec2(m, trail);
}

float StaticDrops(vec2 uv, float t)
{
    uv *= 40.;

    vec2 id = floor(uv);
    uv = fract(uv) - .5;
    vec3 n = N13(id.x * 107.45 + id.y * 3543.654);
    vec2 p = (n.xy - .5) * .7;
    float d = length(uv - p);

    float fade = Saw(.025, fract(t + n.z));
    float c = S(.3, 0., d) * fract(n.z * 10.) * fade;
    return c;
}

vec2 Drops(vec2 uv, float t, float l0, float l1, float l2)
{
    float s = StaticDrops(uv, t) * l0;
    vec2 m1 = DropLayer2(uv, t) * l1;
    vec2 m2 = DropLayer2(uv * 1.85, t) * l2;

    float c = s + m1.x + m2.x;
    c = S(.3, 1., c);

    return vec2(c, max(m1.y * l0, m2.y * l1));
}

// Outward unit normal of the cube face nearest the surface point p (object space,
// cube is [-1,1]^3): the axis with the largest |component|.
vec3 cube_face_normal(vec3 p)
{
    vec3 a = abs(p);
    if (a.x >= a.y && a.x >= a.z) return vec3(sign(p.x), 0.0, 0.0);
    if (a.y >= a.z)               return vec3(0.0, sign(p.y), 0.0);
    return vec3(0.0, 0.0, sign(p.z));
}

// Distance from a point on/inside the unit cube [-1,1]^3 to where it exits the
// cube travelling along dir (object space). Slab method, far boundary.
float cube_exit_t(vec3 p, vec3 dir)
{
    // Keep every component away from 0 so (face - p)*inv never evaluates 0*inf=NaN
    // when the entry point sits exactly on a face we travel parallel to.
    vec3 d;
    d.x = abs(dir.x) < 1e-5 ? 1e-5 : dir.x;
    d.y = abs(dir.y) < 1e-5 ? 1e-5 : dir.y;
    d.z = abs(dir.z) < 1e-5 ? 1e-5 : dir.z;
    vec3 inv = 1.0 / d;
    vec3 a = (vec3(-1.0) - p) * inv;
    vec3 b = (vec3( 1.0) - p) * inv;
    vec3 tmax = max(a, b);
    return max(min(min(tmax.x, tmax.y), tmax.z), 0.0);
}

// Transform an object-space normal to world space (inverse-transpose, so it stays
// correct under the non-uniform scale used to make a thin windowpane).
vec3 world_normal(vec3 nl)
{
    return normalize(transpose(mat3(gl_WorldToObjectEXT)) * nl);
}

// Trace one continuation primary ray and return the colour it sees. Leaves
// primary_payload populated with that ray's hit (caller restores what it needs).
vec3 trace_segment(vec3 origin, vec3 dir, uint next_depth)
{
    primary_payload.color = vec4(0.0, 0.0, 0.0, 1.0);
    primary_payload.hit_distance = 1e30;
    primary_payload.depth = next_depth;
    traceRayEXT(
        top_level_as,
        gl_RayFlagsNoneEXT,
        0xFF,
        0,
        1,
        0,
        origin,
        1.0e-3,
        dir,
        10000.0,
        0);
    return primary_payload.color.rgb;
}

void main()
{
    float T = scene_uniforms.animation_time_data.x;

    // ---- Find the hit face of the glass box ----------------------------------
    // cube.glb local bounds are [-1,1]^3. The closest-hit fires on whichever face
    // the viewer sees (the FRONT face); the drops live on that face and light
    // refracts through the box depth to the BACK face — real geometric thickness
    // driven by the box's scale (squash a cube thin to make a windowpane).
    vec3 hit_ws = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    vec3 lp = (gl_WorldToObjectEXT * vec4(hit_ws, 1.0)).xyz;

    vec3 faceN_l = cube_face_normal(lp);            // object-space front-face normal
    // The two in-plane (tangent) axes are the other two cube axes.
    vec3 tanU_l, tanV_l;
    if (faceN_l.x != 0.0)      { tanU_l = vec3(0.0, 1.0, 0.0); tanV_l = vec3(0.0, 0.0, 1.0); }
    else if (faceN_l.y != 0.0) { tanU_l = vec3(1.0, 0.0, 0.0); tanV_l = vec3(0.0, 0.0, 1.0); }
    else                       { tanU_l = vec3(1.0, 0.0, 0.0); tanV_l = vec3(0.0, 1.0, 0.0); }

    // Scale-adaptive parameterisation: lay the drop field out in WORLD units across
    // the face (axis length = world units per local unit), so drops keep a constant
    // real-world size and a bigger window just gets MORE drops instead of stretching.
    float world_u = length(mat3(gl_ObjectToWorldEXT) * tanU_l);
    float world_v = length(mat3(gl_ObjectToWorldEXT) * tanV_l);
    vec2 uv = vec2(dot(lp, tanU_l) * world_u, dot(lp, tanV_l) * world_v) * RAIN_TILING;

    // ---- Heartfelt drop field (heart/zoom/fade removed) ----------------------
    float t = T * 0.2;
    float rainAmount = sin(T * 0.05) * 0.3 + 0.7;

    float staticDrops = S(-.5, 1., rainAmount) * 2.;
    float layer1 = S(.25, .75, rainAmount);
    float layer2 = S(.0, .5, rainAmount);

    vec2 c = Drops(uv, t, staticDrops, layer1, layer2);

    // Expensive normals (gradient of the drop coverage) — the refraction normal.
    vec2 e = vec2(.001, 0.);
    float cx = Drops(uv + e, t, staticDrops, layer1, layer2).x;
    float cy = Drops(uv + e.yx, t, staticDrops, layer1, layer2).x;
    vec2 n = vec2(cx - c.x, cy - c.x);

    // ---- Glass-window shading --------------------------------------------------
    vec3 tan_u = normalize(mat3(gl_ObjectToWorldEXT) * tanU_l);
    vec3 tan_v = normalize(mat3(gl_ObjectToWorldEXT) * tanV_l);

    vec3 rd = normalize(gl_WorldRayDirectionEXT);

    // Front-face normal in world space, faced toward the viewer, then bumped by the
    // drops so they distort both the refraction and the reflection.
    vec3 Ng = world_normal(faceN_l);
    if (dot(Ng, rd) > 0.0) Ng = -Ng;
    vec3 N = normalize(Ng - (n.x * tan_u + n.y * tan_v) * RAIN_DROP_BUMP);

    // Schlick Fresnel from the IOR: how much the glass reflects vs transmits.
    float F0 = (1.0 - RAIN_IOR) / (1.0 + RAIN_IOR);
    F0 *= F0;
    float cos_i = clamp(dot(N, -rd), 0.0, 1.0);
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - cos_i, 5.0);

    // Reflection off the bumped front normal.
    vec3 rd_refl = reflect(rd, N);

    // ---- Refract through the glass volume (real thickness) -------------------
    // Enter the front face (drop-bumped normal), march through the box to the back
    // face, then refract out. The back-face exit point is OFFSET from the entry by
    // the true geometric depth, so the view through the pane is displaced exactly as
    // through real thick glass — visible from any angle, with real edges.
    bool front_tir = false;
    vec3 rd_in = refract(rd, N, 1.0 / RAIN_IOR);
    if (dot(rd_in, rd_in) < 1e-6) { rd_in = reflect(rd, N); front_tir = true; }

    vec3 rd_in_obj = normalize(mat3(gl_WorldToObjectEXT) * rd_in);
    float exit_t = cube_exit_t(lp, rd_in_obj);
    vec3 exit_lp = lp + rd_in_obj * exit_t;
    vec3 exit_ws = (gl_ObjectToWorldEXT * vec4(exit_lp, 1.0)).xyz;
    float path = distance(hit_ws, exit_ws);          // real distance through the glass

    // Refract out at the back face (flat — no drops on the inside).
    vec3 backN = world_normal(cube_face_normal(exit_lp));
    vec3 nb = (dot(rd_in, backN) > 0.0) ? -backN : backN;   // face against the inside ray
    vec3 rd_out = refract(rd_in, nb, RAIN_IOR);
    if (dot(rd_out, rd_out) < 1e-6) rd_out = rd_in;  // internal reflection at back: approx

    // ---- Trace transmitted + reflected rays ----------------------------------
    vec3 transmitted = vec3(0.0);
    vec3 reflected = vec3(0.0);
    const uint current_depth = primary_payload.depth;
    if (current_depth < RAIN_MAX_DEPTH)
    {
        if (!front_tir) transmitted = trace_segment(exit_ws, rd_out, current_depth + 1u);
        reflected = trace_segment(hit_ws, rd_refl, current_depth + 1u);
    }

    // Beer-Lambert absorption over the real path length through the glass, so a
    // thicker pane (or grazing view) tints more. RAIN_ABSORB scales the strength.
    transmitted *= pow(RAIN_TINT, vec3(path * RAIN_ABSORB));
    transmitted *= RAIN_TRANSMISSION;

    // Composite: transmit through the glass, blend in the Fresnel reflection.
    // (front_tir -> fresnel is ~1 at the grazing angle that caused it, so the
    //  reflection dominates and the zero transmission is not seen.)
    vec3 col = mix(transmitted, reflected * RAIN_REFLECT, fresnel);

    primary_payload.color = vec4(col, 1.0);

    // TAA tracking: track the GLASS FRONT FACE itself (a stable world point that
    // moves only with the camera/window), NOT the refracted hit behind it.
    // Recording the animated see-through hit is what made the view ghost. The scene
    // behind reprojects with the face — minor parallax softening when the camera
    // moves, but rock stable and no per-drop smearing.
    primary_payload.hit_distance   = gl_HitTEXT;
    primary_payload.pos_ws_curr    = hit_ws;
    primary_payload.pos_ws_prev    = hit_ws;
    primary_payload.shading_normal = Ng;
    primary_payload.material_flags = 0u;
    primary_payload.depth = current_depth;
}
