// Custom shader fragment setup for RT hit groups (closest-hit and any-hit).
// Include inside main() under #ifdef RT_CUSTOM_HIT_GROUP.
//
// Required variables before inclusion:
//   uint  rt_geometry_idx   -- geometry/material index
//   vec3  rt_hit_pos        -- world-space hit position
//   vec2  rt_uv             -- interpolated UV
//   vec3  rt_normal         -- world-space geometry normal (flipped for back-face)
//   vec3  rt_tangent        -- world-space tangent
//   vec3  rt_bitangent      -- world-space bitangent
//   bool  rt_front_face     -- true if front-face hit
//   vec4  rt_custom0..3     -- CUSTOM0-3 vertex attributes (vec4(0) if absent)
//
// Required bindings/types:
//   materials[], CustomMaterialUniforms, scene_data_block

MaterialData rt_mat = materials[rt_geometry_idx];
material = CustomMaterialUniforms(rt_mat.uniform_address);

// packed_bounces_flags is a plain field on the payload, so this is a mask rather
// than a full path_unpack.
rt_bounce_index = get_total_bounces(payload.packed_bounces_flags);
rt_primary_hit = (rt_bounce_index == 0u);

// Matrices.
mat4 rt_view_matrix = transpose(mat4(scene_data_block.data.view_matrix[0],
		scene_data_block.data.view_matrix[1],
		scene_data_block.data.view_matrix[2],
		vec4(0.0, 0.0, 0.0, 1.0)));

GeometryData rt_geom = geometries[rt_geometry_idx];
mat4 rt_aabb_xform;
mat4 rt_inv_aabb_xform;
get_aabb_compression_xforms(rt_geom, rt_aabb_xform, rt_inv_aabb_xform);

read_model_matrix = mat4(gl_ObjectToWorldEXT) * rt_inv_aabb_xform;
read_view_matrix = rt_view_matrix;
inv_view_matrix = transpose(mat4(scene_data_block.data.inv_view_matrix[0],
		scene_data_block.data.inv_view_matrix[1],
		scene_data_block.data.inv_view_matrix[2],
		vec4(0.0, 0.0, 0.0, 1.0)));
projection_matrix = scene_data_block.data.projection_matrix;
inv_projection_matrix = scene_data_block.data.inv_projection_matrix;
read_viewport_size = scene_data_block.data.viewport_size;
global_time = scene_data_block.data.time;

mat4 rt_world_to_object_decomp = rt_aabb_xform * mat4(gl_WorldToObjectEXT);

vertex = (rt_world_to_object_decomp * vec4(rt_hit_pos, 1.0)).xyz;
normal = mat3(rt_world_to_object_decomp) * rt_normal;
tangent = mat3(rt_world_to_object_decomp) * rt_tangent;
binormal = mat3(rt_world_to_object_decomp) * rt_bitangent;
uv_interp = rt_uv;
uv2_interp = rt_uv;
color_interp = rt_color;
custom0_attrib = rt_custom0;
custom1_attrib = rt_custom1;
custom2_attrib = rt_custom2;
custom3_attrib = rt_custom3;
view = -gl_WorldRayDirectionEXT;
rt_front_facing = rt_front_face;
rt_screen_uv = vec2(gl_LaunchIDEXT.xy) / vec2(gl_LaunchSizeEXT.xy);
rt_frag_coord = vec4(gl_LaunchIDEXT.xy, 0.0, 1.0);

// Per-pixel world-space footprint at the hit (ray differentials). The camera's
// right/up axes, scaled by the world size of one pixel at this distance, then
// slid along the ray onto the surface plane so grazing angles stretch the
// footprint the way real derivatives would.
//
// projection_matrix[1][1] is 1/tan(fovy/2), so 2*t/(that * viewport_height) is the
// world height of a pixel at distance t. On secondary bounces gl_HitTEXT is that
// bounce's length rather than a camera distance, which makes this an
// approximation there -- still far better than the zero derivative it replaces.
{
	float rt_t = gl_HitTEXT;
	vec3 rt_dir = gl_WorldRayDirectionEXT;
	// abs(): projection_matrix[1][1] is NEGATIVE under Vulkan's Y-flip. update_ubo
	// builds projection = correction * cam_projection, and Projection::set_depth_correction
	// sets columns[1][1] = flip_y ? -1 : 1 (projection.cpp:796), so this term is about
	// -1/tan(fovy/2). Without abs() the product goes negative, max(negative, 1e-6)
	// returns the epsilon, and the footprint inflates by ~1e6 -- every textureGrad in a
	// custom hit group then takes the coarsest mip. MEASURED on map_dc_level: median LOD
	// 6.000 (clamp ceiling) against the rasterizer's ground-truth 0.494.
	float rt_px = 2.0 * rt_t / max(abs(projection_matrix[1][1]) * read_viewport_size.y, 1e-6);
	vec3 rt_fx = inv_view_matrix[0].xyz * rt_px;
	vec3 rt_fy = inv_view_matrix[1].xyz * rt_px;
	vec3 rt_n = normalize(rt_normal);
	float rt_ndotd = dot(rt_n, rt_dir);
	if (abs(rt_ndotd) > 1e-4) {
		rt_footprint_x = rt_fx - rt_dir * (dot(rt_fx, rt_n) / rt_ndotd);
		rt_footprint_y = rt_fy - rt_dir * (dot(rt_fy, rt_n) / rt_ndotd);
	} else {
		rt_footprint_x = rt_fx;
		rt_footprint_y = rt_fy;
	}
}

// Run vertex shader (computes varyings, may modify built-ins).
/* RT_CUSTOM_VERTEX_CALL */

// Post-vertex transform: object-space -> view-space (mirrors rasterizer post-vertex).
mat4 rt_modelview = rt_view_matrix * read_model_matrix;
vertex = (rt_modelview * vec4(vertex, 1.0)).xyz;
normal = normalize(mat3(rt_modelview) * normal);
tangent = normalize(mat3(rt_modelview) * tangent);
binormal = normalize(mat3(rt_modelview) * binormal);

// Fragment outputs with sensible defaults.
vec3 albedo = vec3(1.0);
float alpha = 1.0;
float metallic = 0.0;
float roughness = 0.5;
float specular = 0.5;
vec3 emission = vec3(0.0);
vec3 normal_map = vec3(0.5, 0.5, 1.0);
float normal_map_depth = 1.0;
float ao = 1.0;
float ao_light_affect = 0.0;
vec3 backlight = vec3(0.0);
float sss_strength = 0.0;
float rim = 0.0;
float rim_tint = 0.0;
float clearcoat = 0.0;
float clearcoat_roughness = 0.0;
float anisotropy = 0.0;
vec2 anisotropy_flow = vec2(1.0, 0.0);
float alpha_scissor_threshold = 0.0;
float alpha_hash_scale = 1.0;
float alpha_antialiasing_edge = 0.0;
vec2 alpha_texture_coordinate = vec2(0.0);

{
	/* RT_CUSTOM_FRAGMENT_CODE */
}
