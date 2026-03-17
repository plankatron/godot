// Common closest_hit utilities shared by all hit groups.
//
// Required includes (before this file):
//   raytracing_inc.glsl, brdf_inc.glsl, raytracing_hit_inc.glsl, raytracing_lights_inc.glsl,
//   raytracing_material_eval_inc.glsl
//
// Required bindings (before this file):
//   tlas, payload, scene_data_block, geometries[], materials[], bindless_textures[],
//   SAMPLER_* (12 material samplers), rt_params, rt_depth_image,
//   DLSS-RR images (ifdef DLSS_RR_ENABLED)

// ============================================================================
// HIT DATA
// ============================================================================

struct HitData {
	vec3 hit_pos;
	vec3 geometry_normal; // World space, flipped for back-face hits.
	vec3 tangent; // World space.
	vec3 bitangent; // World space.
	vec2 uv; // Raw UV (no material scale/offset applied).
	bool is_front_face;
	uint geometry_idx;
};

/// Fetch vertex attributes and transform to world space.
/// Requires hitAttributeEXT attribs and GeometryBuffer/MaterialBuffer bindings.
HitData compute_hit_data() {
	HitData h;
	h.geometry_idx = gl_InstanceCustomIndexEXT;
	GeometryData geom = geometries[h.geometry_idx];

	VertexAttributes attrs = fetch_vertex_attributes(geom, attribs, true, true);
	h.uv = attrs.uv;

	mat3 model_rotation = mat3(gl_ObjectToWorldEXT);
	mat3 normal_matrix = mat3(
			normalize(model_rotation[0]),
			normalize(model_rotation[1]),
			normalize(model_rotation[2]));

	h.geometry_normal = normalize(normal_matrix * attrs.normal);
	h.tangent = normalize(normal_matrix * attrs.tangent);
	h.bitangent = cross(h.geometry_normal, h.tangent) * attrs.bitangent_sign;

	h.is_front_face = (gl_HitKindEXT == gl_HitKindFrontFacingTriangleEXT);
	if (!h.is_front_face) {
		h.geometry_normal = -h.geometry_normal;
	}

	h.hit_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;

	return h;
}

// ============================================================================
// HELPERS
// ============================================================================

vec4 sample_bindless_texture(uint tex_idx, vec2 uv) {
	return texture(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv);
}

/// Sample with point/nearest filtering (for pixel art textures).
vec4 sample_bindless_texture_point(uint tex_idx, vec2 uv) {
	return texture(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_NEAREST_REPEAT), uv);
}

/// Sample with the appropriate filter based on material flags (bit 2 = point filtering).
vec4 sample_material_texture(uint tex_idx, vec2 uv, uint mat_flags) {
	if ((mat_flags & 4u) != 0u) {
		return sample_bindless_texture_point(tex_idx, uv);
	}
	return sample_bindless_texture(tex_idx, uv);
}

/// Apply tangent-space normal map to geometry normal.
vec3 apply_normal_map(HitData h, vec3 tangent_space_normal, float normal_map_depth) {
	vec3 mapped = h.tangent * tangent_space_normal.x + h.bitangent * tangent_space_normal.y + h.geometry_normal * tangent_space_normal.z;
	return normalize(mix(h.geometry_normal, mapped, normal_map_depth));
}

// ============================================================================
// DEPTH WRITE (primary ray only)
// ============================================================================

/// Write NDC depth for primary ray hits (bounce 0, sample 0 only).
void write_primary_hit_depth(vec3 hit_pos) {
	if (get_total_bounces(payload.packed_bounces_flags) == 0u && is_sample_zero(payload.packed_bounces_flags)) {
		mat4 view_mat = transpose(mat4(scene_data_block.data.view_matrix[0],
				scene_data_block.data.view_matrix[1],
				scene_data_block.data.view_matrix[2],
				vec4(0.0, 0.0, 0.0, 1.0)));
		vec3 view_pos = (view_mat * vec4(hit_pos, 1.0)).xyz;
		vec4 clip_pos = scene_data_block.data.projection_matrix * vec4(view_pos, 1.0);
		float ndc_depth = clip_pos.z / clip_pos.w;
		imageStore(rt_depth_image, ivec2(gl_LaunchIDEXT.xy), vec4(ndc_depth));
	}
}

// ============================================================================
// VELOCITY WRITE (primary ray only)
// ============================================================================

/// Write UV-space motion vector for primary ray hits (bounce 0, sample 0 only).
/// Reprojects the hit point through the previous frame's object + camera transforms.
void write_primary_hit_velocity(vec3 world_hit_pos) {
	if (get_total_bounces(payload.packed_bounces_flags) != 0u || !is_sample_zero(payload.packed_bounces_flags)) {
		return;
	}

	ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
	vec2 uv = (vec2(pixel) + 0.5) / vec2(gl_LaunchSizeEXT.xy);

	// Transform world hit position to object space via gl_WorldToObjectEXT.
	vec3 obj_pos = (gl_WorldToObjectEXT * vec4(world_hit_pos, 1.0)).xyz;

	// Reconstruct previous world position from prev_transforms SSBO.
	uint idx = gl_InstanceCustomIndexEXT;
	vec4 r0 = prev_transforms[idx * 3u + 0u];
	vec4 r1 = prev_transforms[idx * 3u + 1u];
	vec4 r2 = prev_transforms[idx * 3u + 2u];
	vec3 prev_world = vec3(
			dot(r0.xyz, obj_pos) + r0.w,
			dot(r1.xyz, obj_pos) + r1.w,
			dot(r2.xyz, obj_pos) + r2.w);

	// Project through previous camera (view + unjittered projection).
	mat4 prev_view_mat = transpose(mat4(
			prev_frame.prev_view_matrix[0],
			prev_frame.prev_view_matrix[1],
			prev_frame.prev_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	vec3 prev_view_pos = (prev_view_mat * vec4(prev_world, 1.0)).xyz;
	vec4 prev_clip = prev_frame.prev_projection_matrix * vec4(prev_view_pos, 1.0);
	vec2 prev_uv = 0.5 + (prev_clip.xy / prev_clip.w) * 0.5;

	imageStore(rt_velocity_image, pixel, vec4(prev_uv - uv, 0.0, 0.0));
}

// ============================================================================
// SHADE AND BOUNCE
// ============================================================================

/// Production shading: emissive + NEE direct lighting + BRDF importance sampling + next bounce.
/// Also handles DLSS-RR G-buffer output on primary ray.
void shade_and_bounce(HitData h, MaterialResult m) {
	vec3 V = -gl_WorldRayDirectionEXT;
	float NdotV = max(dot(m.normal, V), 0.0001);

	uint total_bounces = get_total_bounces(payload.packed_bounces_flags);
	uint diffuse_bounces = get_diffuse_bounces(payload.packed_bounces_flags);

	// Emissive contribution.
	payload.radiance += payload.throughput * m.emissive;

	// Bounce limit check.
	if (total_bounces >= RT_GET_MAX_BOUNCES() || diffuse_bounces >= MAX_DIFFUSE_BOUNCES) {
		return;
	}

	// BRDF material setup.
	MaterialProperties brdf_mat;
	brdf_mat.baseColor = m.albedo;
	brdf_mat.metalness = m.metalness;
	brdf_mat.roughness = m.roughness;
	brdf_mat.emissive = m.emissive;
	brdf_mat.transmissivness = 0.0;
	brdf_mat.opacity = 1.0;

	vec3 specularF0 = baseColorToSpecularF0(brdf_mat.baseColor, brdf_mat.metalness);
	vec3 diffuseReflectance = baseColorToDiffuseReflectance(brdf_mat.baseColor, brdf_mat.metalness);

	// =================================================================
	// DLSS Ray Reconstruction output (primary ray, sample 0 only)
	// =================================================================
#ifdef DLSS_RR_ENABLED
	if (total_bounces == 0u && is_sample_zero(payload.packed_bounces_flags)) {
		ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);

		vec3 diffuse_albedo = DLSSRR_computeDiffuseAlbedo(m.albedo, m.metalness);
		imageStore(dlss_rr_diffuse_albedo, pixel, vec4(diffuse_albedo, 1.0));

		vec3 specular_albedo = DLSSRR_computeSpecularAlbedo(m.albedo, m.metalness, m.roughness, NdotV);
		imageStore(dlss_rr_specular_albedo, pixel, vec4(specular_albedo, 1.0));

		imageStore(dlss_rr_normal_roughness, pixel, vec4(m.normal, m.roughness));

		// Specular hit distance via inline ray query.
		vec3 spec_dir = reflect(-V, m.normal);
		vec3 spec_origin = offset_ray_origin(h.hit_pos, spec_dir);

		rayQueryEXT spec_rq;
		rayQueryInitializeEXT(spec_rq, tlas, RT_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT,
				0xFF, spec_origin, 0.001, spec_dir, 10000.0);
		while (rayQueryProceedEXT(spec_rq)) {
			if (rayQueryGetIntersectionTypeEXT(spec_rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
				if (ray_query_alpha_test(
							rayQueryGetIntersectionInstanceCustomIndexEXT(spec_rq, false),
							rayQueryGetIntersectionPrimitiveIndexEXT(spec_rq, false),
							rayQueryGetIntersectionBarycentricsEXT(spec_rq, false))) {
					rayQueryConfirmIntersectionEXT(spec_rq);
				}
			}
		}
		float spec_hit_dist = -1.0;
		vec2 spec_mv = vec2(0.0);
		if (rayQueryGetIntersectionTypeEXT(spec_rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
			spec_hit_dist = rayQueryGetIntersectionTEXT(spec_rq, true);

			vec3 spec_world_hit = spec_origin + spec_dir * spec_hit_dist;
			uint spec_idx = rayQueryGetIntersectionInstanceCustomIndexEXT(spec_rq, true);

			// Current UV of reflected point.
			mat4 cur_view_mat = transpose(mat4(
					scene_data_block.data.view_matrix[0],
					scene_data_block.data.view_matrix[1],
					scene_data_block.data.view_matrix[2],
					vec4(0.0, 0.0, 0.0, 1.0)));
			vec3 spec_view_pos = (cur_view_mat * vec4(spec_world_hit, 1.0)).xyz;
			vec4 spec_clip = scene_data_block.data.projection_matrix * vec4(spec_view_pos, 1.0);
			vec2 spec_cur_uv = 0.5 + (spec_clip.xy / spec_clip.w) * 0.5;

			// Previous world position via object space + prev transform.
			mat4x3 spec_world_to_obj = rayQueryGetIntersectionWorldToObjectEXT(spec_rq, true);
			vec3 spec_obj_pos = spec_world_to_obj * vec4(spec_world_hit, 1.0);

			vec4 sr0 = prev_transforms[spec_idx * 3u + 0u];
			vec4 sr1 = prev_transforms[spec_idx * 3u + 1u];
			vec4 sr2 = prev_transforms[spec_idx * 3u + 2u];
			vec3 spec_prev_world = vec3(
					dot(sr0.xyz, spec_obj_pos) + sr0.w,
					dot(sr1.xyz, spec_obj_pos) + sr1.w,
					dot(sr2.xyz, spec_obj_pos) + sr2.w);

			// Previous UV via prev camera.
			mat4 prev_view_mat = transpose(mat4(
					prev_frame.prev_view_matrix[0],
					prev_frame.prev_view_matrix[1],
					prev_frame.prev_view_matrix[2],
					vec4(0.0, 0.0, 0.0, 1.0)));
			vec3 spec_prev_view = (prev_view_mat * vec4(spec_prev_world, 1.0)).xyz;
			vec4 spec_prev_clip = prev_frame.prev_projection_matrix * vec4(spec_prev_view, 1.0);
			vec2 spec_prev_uv = 0.5 + (spec_prev_clip.xy / spec_prev_clip.w) * 0.5;

			spec_mv = spec_prev_uv - spec_cur_uv;
		}
		imageStore(dlss_rr_specular_hit_dist, pixel, vec4(spec_hit_dist));
		imageStore(dlss_rr_specular_mvec, pixel, vec4(spec_mv, 0.0, 0.0));
	}
#endif

	// =================================================================
	// NEE: Next Event Estimation (direct light sampling)
	// =================================================================
	uint rt_light_count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));
	if (rt_light_count > 0u) {
		vec3 hit_pos_offset = offset_ray_origin(h.hit_pos, h.geometry_normal);
		bool is_indirect = (diffuse_bounces > 0u);
		vec3 direct_light = lights_evaluate_direct_lighting(
				hit_pos_offset, m.normal, V, brdf_mat, payload.rng_state, is_indirect, rt_light_count);
		payload.radiance += payload.throughput * direct_light;
	}

	// =================================================================
	// BRDF importance sampling for next bounce
	// =================================================================
	float specularLum = luminance(specularF0);
	float diffuseLum = luminance(diffuseReflectance);

	int brdfType;
	if (diffuseLum < 0.0001) {
		brdfType = SPECULAR_TYPE;
	} else if (specularLum < 0.0001) {
		brdfType = DIFFUSE_TYPE;
	} else {
		float brdfProbability = clamp(specularLum / (specularLum + diffuseLum), 0.01, 0.99);
		if (rand(payload.rng_state) < brdfProbability) {
			brdfType = SPECULAR_TYPE;
			payload.throughput /= brdfProbability;
		} else {
			brdfType = DIFFUSE_TYPE;
			payload.throughput /= (1.0 - brdfProbability);
		}
	}

	vec2 u = rand2(payload.rng_state);
	vec3 next_dir;
	vec3 brdf_weight;
	if (!evalIndirectCombinedBRDF(u, m.normal, h.geometry_normal, V, brdf_mat, brdfType, next_dir, brdf_weight, vec4(0.0))) {
		return;
	}

	payload.throughput *= brdf_weight;

	if (brdfType == DIFFUSE_TYPE) {
		payload.packed_bounces_flags = inc_diffuse_bounce(payload.packed_bounces_flags);
	} else {
		payload.packed_bounces_flags = inc_total_bounce(payload.packed_bounces_flags);
	}

	vec3 ray_origin = offset_ray_origin(h.hit_pos, h.geometry_normal);
	traceRayEXT(tlas, RT_RAY_FLAGS, 0xFF, 0, 0, 0, ray_origin, 0.001, next_dir, 10000.0, 0);
}
