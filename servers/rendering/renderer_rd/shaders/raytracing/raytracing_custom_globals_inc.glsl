// Custom shader globals for RT hit groups (closest-hit and any-hit).
// Include under #ifdef RT_CUSTOM_HIT_GROUP, outside main().
// Requires: bindless_textures[], materials[] bindings already declared.

layout(buffer_reference, std140) readonly buffer CustomMaterialUniforms{
	/* RT_CUSTOM_UNIFORM_MEMBERS */
};

// Shared vertex/fragment built-ins at global scope so that the vertex function,
// texture #defines, and fragment_globals functions can all access them.
// Assigned to real hit values in main() before use.
CustomMaterialUniforms material = CustomMaterialUniforms(uint64_t(0));
vec3 vertex = vec3(0.0);
vec3 normal = vec3(0.0, 0.0, 1.0);
vec3 tangent = vec3(1.0, 0.0, 0.0);
vec3 binormal = vec3(0.0, 1.0, 0.0);
vec2 uv_interp = vec2(0.0);
vec2 uv2_interp = vec2(0.0);
vec4 color_interp = vec4(1.0);
// ShaderCompiler renames CUSTOM0-3 to these (see actions.renames in
// SceneShaderRaytracing), so they must exist even when the surface has no such
// array -- otherwise any shader touching CUSTOM0 fails to compile and takes its
// whole hit group down.
// Path depth of the hit being shaded. 0 is the camera-visible hit; anything
// higher is an indirect bounce contributing through the throughput, where a
// cheaper material is usually indistinguishable. Shaders can use these to skip
// detail work on secondary bounces -- in a path tracer the material is evaluated
// once per bounce per sample, so that scales with pt_bounces.
// Exposed to GDShader as BOUNCE_INDEX and PRIMARY_HIT.
uint rt_bounce_index = 0u;
bool rt_primary_hit = true;

vec4 custom0_attrib = vec4(0.0);
vec4 custom1_attrib = vec4(0.0);
vec4 custom2_attrib = vec4(0.0);
vec4 custom3_attrib = vec4(0.0);
vec3 view = vec3(0.0, 0.0, -1.0);
mat4 read_model_matrix = mat4(1.0);
mat4 read_view_matrix = mat4(1.0);
mat4 inv_view_matrix = mat4(1.0);
mat4 projection_matrix = mat4(1.0);
mat4 inv_projection_matrix = mat4(1.0);
float global_time = 0.0;
vec2 read_viewport_size = vec2(1.0);
bool rt_front_facing = true;
vec2 rt_screen_uv = vec2(0.0);
vec4 rt_frag_coord = vec4(0.0);
float alpha_antialiasing_edge = 0.0;
vec2 alpha_texture_coordinate = vec2(0.0);

// Screen-space derivatives do not exist in ray tracing: a hit shader has no 2x2
// pixel quad, so dFdx/dFdy/fwidth have no closest-hit overload and glslang
// rejects any shader calling them -- failing the whole hit group, which
// build_tlas then skips, leaving the geometry invisible but still collidable.
//
// These are macros rather than functions because redefining a built-in with an
// existing signature is not legal GLSL, and macros stay type-generic.
//
// LIMITATION: the derivative is zero, so textureGrad/textureQueryLod select
// LOD 0 and screen-footprint mip selection is lost under RT (expect aliasing on
// minified detail). Doing this properly needs ray differentials or ray cones,
// which require the hit triangle's attribute gradients and so cannot be
// expressed as a macro over an arbitrary expression.
#define dFdx(m_v) ((m_v) * 0.0)
#define dFdy(m_v) ((m_v) * 0.0)
#define dFdxCoarse(m_v) ((m_v) * 0.0)
#define dFdyCoarse(m_v) ((m_v) * 0.0)
#define dFdxFine(m_v) ((m_v) * 0.0)
#define dFdyFine(m_v) ((m_v) * 0.0)
#define fwidth(m_v) ((m_v) * 0.0)

// Screen/depth textures are unavailable in RT -- alias to bindless slot 0
// so shaders that reference them still compile (reads return dummy values).
#define depth_buffer bindless_textures[0]
#define color_buffer bindless_textures[0]
#define normal_roughness_buffer bindless_textures[0]

/* RT_CUSTOM_TEXTURE_DEFINES */
/* RT_CUSTOM_FRAGMENT_GLOBALS */

/* RT_CUSTOM_VERTEX_FUNCTION */
