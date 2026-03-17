/**************************************************************************/
/*  dlss.cpp                                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "dlss.h"

#include "../storage_rd/material_storage.h"
#include "../uniform_set_cache_rd.h"

#ifdef STREAMLINE_ENABLED
#define ENABLE_DLSS 1
#endif

#ifdef ENABLE_DLSS
#include "drivers/streamline/streamline_context.h"
#elif defined(NGX_DLSS_ENABLED)
#define ENABLE_NGX_DLSS 1
#include <dlfcn.h>
#include <string>
#include "core/io/dir_access.h"
#include "core/os/os.h"
#include "drivers/vulkan/godot_vulkan.h"
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_helpers_dlssd_vk.h"
#endif

using namespace RendererRD;

#ifdef ENABLE_DLSS

// Texture layout/state constants (avoid including Vulkan/D3D12 headers here).
static constexpr uint64_t DLSS_VK_IMAGE_LAYOUT_SHADER_READ_ONLY = 5;
static constexpr uint64_t DLSS_D3D12_RESOURCE_STATE_NON_PIXEL_SR = 0x40;
static constexpr float DLSS_OPTIMAL_MODE_MAX_DISTANCE = 1000000.0f;
namespace RendererRD {
class DLSSContextInner : public DLSSContext {
public:
	sl::ViewportHandle viewport;
	sl::Constants constants;
	sl::DLSSOptions currentDlssOptions;
	sl::DLSSOptimalSettings currentOptimalSettings;
	sl::DLSSDOptions currentDlssDOptions; // DLSS Ray Reconstruction options

	DLSSContextInner();
	virtual ~DLSSContextInner();

	sl::DLSSMode find_optimal_mode(uint32_t outputWidth, uint32_t outputHeight, uint32_t desiredWidth, uint32_t desiredHeight, sl::DLSSOptimalSettings &out_optimalSettings, bool use_dlss_rr = false) {
		// For DLSS-RR, use the DLSS-D API; for regular DLSS, use the standard DLSS API
		if (use_dlss_rr) {
			if (StreamlineContext::get().slDLSSDGetOptimalSettings == nullptr) {
				return sl::DLSSMode::eOff;
			}
		} else {
			if (StreamlineContext::get().slDLSSGetOptimalSettings == nullptr) {
				return sl::DLSSMode::eOff;
			}
		}

		sl::DLSSMode modes[] = { sl::DLSSMode::eDLAA, sl::DLSSMode::eMaxQuality, sl::DLSSMode::eBalanced, sl::DLSSMode::eMaxPerformance, sl::DLSSMode::eUltraPerformance };
		sl::DLSSOptimalSettings settings[sizeof(modes) / sizeof(modes[0])];
		bool validSettings[sizeof(modes) / sizeof(modes[0])];
		Vector2 distance[sizeof(modes) / sizeof(modes[0])];
		memset(validSettings, 0, sizeof(validSettings));

		for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
			sl::Result result;

			if (use_dlss_rr) {
				sl::DLSSDOptions dlssDOptions = {};
				dlssDOptions.outputWidth = outputWidth;
				dlssDOptions.outputHeight = outputHeight;
				dlssDOptions.mode = modes[i];
				sl::DLSSDOptimalSettings dlssDSettings;
				result = StreamlineContext::get().slDLSSDGetOptimalSettings(dlssDOptions, dlssDSettings);
				if (result == sl::Result::eOk) {
					// Copy to common settings struct
					settings[i].optimalRenderWidth = dlssDSettings.optimalRenderWidth;
					settings[i].optimalRenderHeight = dlssDSettings.optimalRenderHeight;
					settings[i].optimalSharpness = dlssDSettings.optimalSharpness;
					settings[i].renderWidthMin = dlssDSettings.renderWidthMin;
					settings[i].renderHeightMin = dlssDSettings.renderHeightMin;
					settings[i].renderWidthMax = dlssDSettings.renderWidthMax;
					settings[i].renderHeightMax = dlssDSettings.renderHeightMax;
				}
			} else {
				sl::DLSSOptions dlssOptions = {};
				dlssOptions.outputWidth = outputWidth;
				dlssOptions.outputHeight = outputHeight;
				dlssOptions.mode = modes[i];
				result = StreamlineContext::get().slDLSSGetOptimalSettings(dlssOptions, settings[i]);
			}

			if (result != sl::Result::eOk) {
				continue;
			}

			sl::DLSSOptimalSettings &optimalSettings = settings[i];
			if (desiredWidth >= optimalSettings.renderWidthMin &&
					desiredWidth <= optimalSettings.renderWidthMax &&
					desiredHeight >= optimalSettings.renderHeightMin &&
					desiredHeight <= optimalSettings.renderHeightMax) {
				validSettings[i] = true;
				distance[i] = Vector2(fabsf((float)optimalSettings.optimalRenderWidth - (float)desiredWidth), fabsf((float)optimalSettings.optimalRenderHeight - (float)desiredHeight));
			}
		}

		// now select the closest match
		Vector2 closestDistance(DLSS_OPTIMAL_MODE_MAX_DISTANCE, DLSS_OPTIMAL_MODE_MAX_DISTANCE);
		int closestDistanceMatch = -1;
		for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
			if (validSettings[i]) {
				if (distance[i].length_squared() < closestDistance.length_squared()) {
					closestDistanceMatch = i;
					closestDistance = distance[i];
				}
			}
		}

		if (closestDistanceMatch != -1) {
			out_optimalSettings = settings[closestDistanceMatch];
			return modes[closestDistanceMatch];
		}

		ERR_FAIL_V_MSG(sl::DLSSMode::eOff, "Couldn't find an appropriate DLSS mode.");
	}
}; // end class
}; // end namespace RendererRD

static Vector<unsigned int> g_dlss_freeViewportIndices;
static unsigned int g_dlss_viewportIndex = 1;

DLSSContextInner::~DLSSContextInner() {
	g_dlss_freeViewportIndices.push_back((unsigned int)viewport);
}

DLSSContextInner::DLSSContextInner() {
	if (g_dlss_freeViewportIndices.size() == 0) {
		g_dlss_freeViewportIndices.push_back(g_dlss_viewportIndex++);
	}
	viewport = g_dlss_freeViewportIndices[g_dlss_freeViewportIndices.size() - 1];
	g_dlss_freeViewportIndices.remove_at(g_dlss_freeViewportIndices.size() - 1);
}

DLSSEffect::DLSSEffect() {
	// Initialize motion vector decode shader
	Vector<String> modes;
	modes.push_back("\n");
	shaders.mvec_decode_shader.initialize(modes, "");
	shaders.mvec_decode_version = shaders.mvec_decode_shader.version_create();
	shaders.mvec_decode_pipeline = RD::get_singleton()->compute_pipeline_create(shaders.mvec_decode_shader.version_get_shader(shaders.mvec_decode_version, 0));
}

DLSSEffect::~DLSSEffect() {
	// Deinitialize motion vector decode
	shaders.mvec_decode_shader.version_free(shaders.mvec_decode_version);
}

DLSSContext *DLSSEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
	DLSSContextInner *context = memnew(RendererRD::DLSSContextInner);

	context->currentDlssOptions.mode = context->find_optimal_mode(p_target_size.width, p_target_size.height, p_internal_size.width, p_internal_size.height, context->currentOptimalSettings);
	context->currentDlssOptions.outputWidth = p_target_size.width;
	context->currentDlssOptions.outputHeight = p_target_size.height;

	context->is_d3d12 = (RD::get_singleton()->get_device_api_name().to_lower() == "d3d12");

	return context;
}

static sl::float4x4 sl_make_identity_matrix() {
	sl::float4x4 ret;
	ret.setRow(0, sl::float4(1.0f, 0.0f, 0.0f, 0.0f));
	ret.setRow(1, sl::float4(0.0f, 1.0f, 0.0f, 0.0f));
	ret.setRow(2, sl::float4(0.0f, 0.0f, 1.0f, 0.0f));
	ret.setRow(3, sl::float4(0.0f, 0.0f, 0.0f, 1.0f));
	return ret;
}

static sl::float4x4 sl_convert_matrix(const Projection &mtx) {
	sl::float4x4 ret;
	ret.setRow(0, sl::float4(mtx.columns[0].x, mtx.columns[1].x, mtx.columns[2].x, mtx.columns[3].x));
	ret.setRow(1, sl::float4(mtx.columns[0].y, mtx.columns[1].y, mtx.columns[2].y, mtx.columns[3].y));
	ret.setRow(2, sl::float4(mtx.columns[0].z, mtx.columns[1].z, mtx.columns[2].z, mtx.columns[3].z));
	ret.setRow(3, sl::float4(mtx.columns[0].w, mtx.columns[1].w, mtx.columns[2].w, mtx.columns[3].w));
	return ret;
}

static sl::float3 sl_convert_vector(const Vector3 &vec) {
	return sl::float3(vec.x, vec.y, vec.z);
}

void DLSSEffect::upscale(const DLSSContext::Parameters &p_params) {
	DLSSContextInner *context = (DLSSContextInner *)p_params.context;

	// Delay enablement
	if (context->delay > 0) {
		--context->delay;
		return;
	}

	// If DLSS is not loaded, escape early
	if (StreamlineContext::get().slDLSSSetOptions == nullptr) {
		return;
	}

	// Begin frame if needed.
	if (StreamlineContext::get().last_token == nullptr) {
		StreamlineContext::get().get_new_frame_token();
	}

	context->last_parameters = p_params;
	context->last_effect = this;

	// Decode mvecs
	{
		RD::get_singleton()->draw_command_begin_label("Decode Invalid Motion Vectors");
		UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
		ERR_FAIL_NULL(uniform_set_cache);
		MaterialStorage *material_storage = MaterialStorage::get_singleton();
		ERR_FAIL_NULL(material_storage);

		// setup our uniforms
		RD::Uniform u_velocity_image(RD::UNIFORM_TYPE_IMAGE, 0, p_params.velocity);
		RD::Uniform u_depth_texture(RD::UNIFORM_TYPE_TEXTURE, 0, p_params.depth);

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

		RID shader = shaders.mvec_decode_shader.version_get_shader(shaders.mvec_decode_version, 0);
		ERR_FAIL_COND(shader.is_null());

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, shaders.mvec_decode_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_velocity_image), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_depth_texture), 1);

		auto texture_format = RD::get_singleton()->texture_get_format(p_params.velocity);

		float push_constants[20];
		push_constants[0] = texture_format.width;
		push_constants[1] = texture_format.height;
		push_constants[2] = 0.0f;
		push_constants[3] = 0.0f;
		memcpy(push_constants + 4, &p_params.reprojection.columns[0].x, sizeof(float) * 16);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, push_constants, sizeof(push_constants));

		RD::get_singleton()->compute_list_dispatch_threads(compute_list, texture_format.width, texture_format.height, 1);
		RD::get_singleton()->compute_list_add_barrier(compute_list);

		RD::get_singleton()->compute_list_end();
		RD::get_singleton()->draw_command_end_label();
	}

	// Inject DLSS into the render graph
	RD::CallbackResource res[8]; // Increased for DLSS-RR buffers
	int num_resources = 0;
	res[num_resources++].rid = p_params.color;
	res[num_resources++].rid = p_params.output;
	res[num_resources++].rid = p_params.depth;
	res[num_resources++].rid = p_params.velocity;

	// Add DLSS-RR buffers if provided
	if (p_params.dlss_rr) {
		if (p_params.dlss_rr_diffuse_albedo.is_valid()) {
			res[num_resources++].rid = p_params.dlss_rr_diffuse_albedo;
		}
		if (p_params.dlss_rr_specular_albedo.is_valid()) {
			res[num_resources++].rid = p_params.dlss_rr_specular_albedo;
		}
		if (p_params.dlss_rr_normal_roughness.is_valid()) {
			res[num_resources++].rid = p_params.dlss_rr_normal_roughness;
		}
		if (p_params.dlss_rr_specular_hit_dist.is_valid()) {
			res[num_resources++].rid = p_params.dlss_rr_specular_hit_dist;
		}
	}

	for (int i = 0; i < num_resources; i++) {
		res[i].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	}
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)DLSSEffect::_upscale_internal_graph_callback, p_params.context, VectorView<RD::CallbackResource>(res, num_resources));
}

void DLSSEffect::_upscale_internal(RDD::CommandBufferID cmdid, const DLSSContext::Parameters &p_params) {
	DLSSContextInner *context = (DLSSContextInner *)p_params.context;

	void *nativeCmdlist = RD::get_singleton()->get_device_driver()->command_buffer_get_native_handle(cmdid);

	// Helper function for tagging resources.
	auto assignResource = [context](sl::Resource *resources, sl::ResourceTag *resourceTags, int &numResources, RID textureRID, sl::BufferType bufferType, sl::ResourceLifecycle lifecycle) {
		if (!textureRID.is_valid() || textureRID.is_null()) {
			return;
		}

		RD::TextureFormat texture_format = RD::get_singleton()->texture_get_format(textureRID);
		uint64_t texture_image = RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE, textureRID);
		uint64_t texture_view = RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_VIEW, textureRID);
		uint64_t texture_device_memory = RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_DEVICE_MEMORY, textureRID);
		uint64_t texture_state = DLSS_VK_IMAGE_LAYOUT_SHADER_READ_ONLY;
		if (context->is_d3d12) {
			texture_state = DLSS_D3D12_RESOURCE_STATE_NON_PIXEL_SR;
		}
		uint64_t texture_vkformat = RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, textureRID);
		uint64_t texture_usage_flags = RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_USAGE_FLAGS, textureRID);
		auto &destinationResource = resources[numResources];
		if (context->is_d3d12) {
			destinationResource = sl::Resource(sl::ResourceType::eTex2d,
					(void *)texture_view, texture_state);
		} else {
			destinationResource = sl::Resource(sl::ResourceType::eTex2d,
					(void *)texture_image, (void *)texture_device_memory, (void *)texture_view, texture_state);
		}
		destinationResource.width = texture_format.width;
		destinationResource.height = texture_format.height;
		destinationResource.nativeFormat = texture_vkformat;
		destinationResource.arrayLayers = texture_format.array_layers;
		destinationResource.flags = 0;
		destinationResource.mipLevels = texture_format.mipmaps;
		destinationResource.usage = texture_usage_flags;

		resourceTags[numResources] = sl::ResourceTag(resources + numResources, bufferType, lifecycle, nullptr);
		++numResources;
	};

	// Set DLSS or DLSS-RR options depending on mode
	bool use_dlss_rr = p_params.dlss_rr && StreamlineContext::get().slDLSSDSetOptions != nullptr && StreamlineContext::get().streamline_capabilities.dlss_rr_available;

	if (use_dlss_rr) {
		// Set DLSS-RR (Ray Reconstruction) options
		context->currentDlssDOptions.mode = context->currentDlssOptions.mode;
		context->currentDlssDOptions.outputWidth = context->currentDlssOptions.outputWidth;
		context->currentDlssDOptions.outputHeight = context->currentDlssOptions.outputHeight;
		context->currentDlssDOptions.colorBuffersHDR = sl::Boolean::eTrue;
		context->currentDlssDOptions.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked; // Normal XYZ + Roughness W

		// Set world-to-camera and camera-to-world matrices for DLSS-RR
		// worldToCameraView = view matrix (world-to-camera transformation)
		// cameraViewToWorld = inverse view matrix (camera-to-world transformation)
		Transform3D view_matrix = p_params.cam_transform.affine_inverse();
		context->currentDlssDOptions.worldToCameraView = sl_convert_matrix(Projection(view_matrix)); //
		context->currentDlssDOptions.cameraViewToWorld = sl_convert_matrix(Projection(view_matrix).inverse());
		char dlssPreset = p_params.preset;
		if (dlssPreset == '?') {
			dlssPreset = StreamlineContext::get().dlss_rr_default_preset;
		}

		if (dlssPreset == '?') {
			context->currentDlssDOptions.dlaaPreset = sl::DLSSDPreset::eDefault;
			context->currentDlssDOptions.qualityPreset = sl::DLSSDPreset::eDefault;
			context->currentDlssDOptions.balancedPreset = sl::DLSSDPreset::eDefault;
			context->currentDlssDOptions.performancePreset = sl::DLSSDPreset::eDefault;
			context->currentDlssDOptions.ultraPerformancePreset = sl::DLSSDPreset::eDefault;
		} else {
			int presetNo = ((int)dlssPreset - (int)'D');
			sl::DLSSDPreset preset = (sl::DLSSDPreset)((int)sl::DLSSDPreset::ePresetD + presetNo);
			context->currentDlssDOptions.dlaaPreset = preset;
			context->currentDlssDOptions.qualityPreset = preset;
			context->currentDlssDOptions.balancedPreset = preset;
			context->currentDlssDOptions.performancePreset = preset;
			context->currentDlssDOptions.ultraPerformancePreset = preset;
		}

		sl::Result result = StreamlineContext::get().slDLSSDSetOptions(context->viewport, context->currentDlssDOptions);
		if (result != sl::Result::eOk) {
			ERR_FAIL_MSG("Failed to call streamline slDLSSDSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));
		}
	} else if (StreamlineContext::get().slDLSSSetOptions != nullptr && StreamlineContext::get().streamline_capabilities.dlss_available) {
		// Set regular DLSS options
		if (p_params.exposure.is_null() || !p_params.exposure.is_valid()) {
			context->currentDlssOptions.useAutoExposure = sl::Boolean::eTrue;
		} else {
			context->currentDlssOptions.useAutoExposure = sl::Boolean::eFalse;
		}

		context->currentDlssOptions.colorBuffersHDR = sl::Boolean::eTrue;
		char dlssPreset = p_params.preset;
		if (dlssPreset == '?') {
			dlssPreset = StreamlineContext::get().dlss_default_preset;
		}

		if (dlssPreset == '?') {
			context->currentDlssOptions.dlaaPreset = sl::DLSSPreset::eDefault;
			context->currentDlssOptions.qualityPreset = sl::DLSSPreset::eDefault;
			context->currentDlssOptions.balancedPreset = sl::DLSSPreset::eDefault;
			context->currentDlssOptions.performancePreset = sl::DLSSPreset::eDefault;
			context->currentDlssOptions.ultraPerformancePreset = sl::DLSSPreset::eDefault;
		} else {
			int presetNo = ((int)dlssPreset - (int)'F');
			sl::DLSSPreset preset = (sl::DLSSPreset)((int)sl::DLSSPreset::ePresetF + presetNo);
			context->currentDlssOptions.dlaaPreset = preset;
			context->currentDlssOptions.qualityPreset = preset;
			context->currentDlssOptions.balancedPreset = preset;
			context->currentDlssOptions.performancePreset = preset;
			context->currentDlssOptions.ultraPerformancePreset = preset;
		}

		sl::Result result = StreamlineContext::get().slDLSSSetOptions(context->viewport, context->currentDlssOptions);
		if (result != sl::Result::eOk) {
			ERR_FAIL_MSG("Failed to call streamline slDLSSSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));
		}
	}

	// Set SL Options
	if (StreamlineContext::get().slSetConstants != nullptr) {
		sl::float4x4 mtxIdentity = sl_make_identity_matrix();
		context->constants.cameraViewToClip = sl_convert_matrix(p_params.cam_projection); // projection mtx (unjittered)
		context->constants.clipToCameraView = sl_convert_matrix(p_params.cam_projection.inverse()); // projection mtx (unjittered, inverted)
		context->constants.clipToLensClip = mtxIdentity; // keep identity unless some lens distortion is applied
		context->constants.clipToPrevClip = sl_convert_matrix(p_params.reprojection); // reprojection matrix
		context->constants.prevClipToClip = sl_convert_matrix(p_params.reprojection.inverse()); // inverted reprojection matrix

		context->constants.cameraPos = sl_convert_vector(p_params.cam_transform.get_origin());
		context->constants.cameraFwd = sl_convert_vector(-p_params.cam_transform.get_basis().rows[2]);
		context->constants.cameraUp = sl_convert_vector(p_params.cam_transform.get_basis().rows[1]);
		context->constants.cameraRight = sl_convert_vector(p_params.cam_transform.get_basis().rows[0]);

		context->constants.cameraNear = p_params.z_near;
		context->constants.cameraFar = p_params.z_far;
		context->constants.cameraFOV = Math::deg_to_rad(p_params.fovy);
		context->constants.cameraMotionIncluded = sl::Boolean::eTrue;
		context->constants.cameraAspectRatio = static_cast<float>(context->currentDlssOptions.outputWidth) / static_cast<float>(context->currentDlssOptions.outputHeight);
		context->constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
		context->constants.depthInverted = p_params.reverse_depth ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		context->constants.motionVectors3D = sl::Boolean::eFalse;
		context->constants.motionVectorsDilated = sl::Boolean::eFalse;
		context->constants.motionVectorsJittered = sl::Boolean::eFalse;
		context->constants.jitterOffset = sl::float2(p_params.jitter.x, p_params.jitter.y);
		context->constants.mvecScale = sl::float2(1.0f, 1.0f);
		context->constants.orthographicProjection = sl::Boolean::eFalse;
		context->constants.reset = p_params.reset_accumulation ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		sl::Result result = StreamlineContext::get().slSetConstants(context->constants, *StreamlineContext::get().last_token, context->viewport);
		if (result != sl::Result::eOk) {
			ERR_FAIL_MSG("Failed to call streamline slSetConstants. Result: " + String(StreamlineContext::result_to_string(result)));
		}
	}

	// Tag resources
	if (StreamlineContext::get().slSetTag != nullptr) {
		sl::Resource resources[10];
		sl::ResourceTag resourceTags[10];
		int numResources = 0;

		assignResource(resources, resourceTags, numResources, p_params.color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent);
		assignResource(resources, resourceTags, numResources, p_params.output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent);
		assignResource(resources, resourceTags, numResources, p_params.depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent);
		assignResource(resources, resourceTags, numResources, p_params.velocity, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent);

		// Tag DLSS-RR specific buffers if enabled
		if (use_dlss_rr) {
			// kBufferTypeAlbedo is used for diffuse albedo
			assignResource(resources, resourceTags, numResources, p_params.dlss_rr_diffuse_albedo, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilPresent);
			assignResource(resources, resourceTags, numResources, p_params.dlss_rr_specular_albedo, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilPresent);
			// kBufferTypeNormalRoughness for packed normal+roughness (XYZ=normal, W=roughness)
			assignResource(resources, resourceTags, numResources, p_params.dlss_rr_normal_roughness, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eValidUntilPresent);
			assignResource(resources, resourceTags, numResources, p_params.dlss_rr_specular_hit_dist, sl::kBufferTypeSpecularHitDistance, sl::ResourceLifecycle::eValidUntilPresent);
		}

		sl::Result result = StreamlineContext::get().slSetTag(context->viewport, resourceTags, numResources, nativeCmdlist);
		if (result != sl::Result::eOk) {
			ERR_FAIL_MSG("Failed to call streamline slSetTag. Result: " + String(StreamlineContext::result_to_string(result)));
		}
	}

	// Toggle DLSS Frame Generation (only enabled in game mode)
	if (StreamlineContext::get().slDLSSGSetOptions != nullptr && StreamlineContext::get().is_game && StreamlineContext::get().streamline_capabilities.dlss_g_available) {
		sl::DLSSGOptions dlssGOptions{};
		bool wantActivateDLSSG = p_params.dlss_g;
		bool canActivateDLSSG = StreamlineContext::get().dlssg_delay == 0;

		// Disable previous DLSS-G context if needed
		if (StreamlineContext::get().dlssg_viewport != sl::ViewportHandle(-1) && ((!wantActivateDLSSG && StreamlineContext::get().dlssg_viewport == context->viewport) || (wantActivateDLSSG && StreamlineContext::get().dlssg_viewport != context->viewport))) {
			WARN_PRINT("Disabling DLSS-G on viewport: " + itos((unsigned int)StreamlineContext::get().dlssg_viewport));
			dlssGOptions.mode = sl::DLSSGMode::eOff;
			sl::Result result = StreamlineContext::get().slDLSSGSetOptions(StreamlineContext::get().dlssg_viewport, dlssGOptions);
			if (result != sl::Result::eOk) {
				ERR_FAIL_MSG("Failed to call streamline slDLSSGSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));
			}

			StreamlineContext::get().dlssg_viewport = sl::ViewportHandle(-1);
		}

		// Enable new DLSS-G context if needed
		if (canActivateDLSSG && wantActivateDLSSG && StreamlineContext::get().dlssg_viewport != context->viewport) {
			WARN_PRINT("Enabling DLSS-G on viewport: " + itos((unsigned int)context->viewport));

			dlssGOptions.mode = sl::DLSSGMode::eOn;
			sl::Result result = StreamlineContext::get().slDLSSGSetOptions(context->viewport, dlssGOptions);
			if (result != sl::Result::eOk) {
				ERR_FAIL_MSG("Failed to call streamline slDLSSGSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));
			}

			StreamlineContext::get().dlssg_viewport = context->viewport;
		}
	}

	// Evaluate DLSS Super Resolution or DLSS Ray Reconstruction
	if (context->currentDlssOptions.mode != sl::DLSSMode::eOff) {
		const sl::BaseStructure *inputs[] = { &context->viewport };
		sl::Result result;

		if (use_dlss_rr && StreamlineContext::get().streamline_capabilities.dlss_rr_available) {
			// Use DLSS Ray Reconstruction
			result = StreamlineContext::get().slEvaluateFeature(sl::kFeatureDLSS_RR, *StreamlineContext::get().last_token, inputs, 1, nativeCmdlist);
			if (result != sl::Result::eOk) {
				ERR_FAIL_MSG("Failed to call streamline slEvaluateFeature for DLSS Ray Reconstruction. Result: " + String(StreamlineContext::result_to_string(result)));
			}
		} else if (StreamlineContext::get().streamline_capabilities.dlss_available) {
			// Use regular DLSS
			result = StreamlineContext::get().slEvaluateFeature(sl::kFeatureDLSS, *StreamlineContext::get().last_token, inputs, 1, nativeCmdlist);
			if (result != sl::Result::eOk) {
				ERR_FAIL_MSG("Failed to call streamline slEvaluateFeature for DLSS Super Resolution. Result: " + String(StreamlineContext::result_to_string(result)));
			}
		}
	}

	// NIS support
	// **********
	if (p_params.sharpness > 0.0f && StreamlineContext::get().slNISSetOptions != nullptr && StreamlineContext::get().streamline_capabilities.nis_available) {
		{ // Set NIS settings
			sl::NISOptions options;
			options.hdrMode = sl::NISHDR::eNone;
			options.mode = sl::NISMode::eSharpen;
			options.sharpness = p_params.sharpness;
			StreamlineContext::get().slNISSetOptions(context->viewport, options);
		}

		{ // Tag NIS buffers
			sl::Resource resources[3];
			sl::ResourceTag resourceTags[3];
			int numResources = 0;

			assignResource(resources, resourceTags, numResources, p_params.output, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow);
			assignResource(resources, resourceTags, numResources, p_params.output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent);

			sl::Result result = StreamlineContext::get().slSetTag(context->viewport, resourceTags, numResources, nativeCmdlist);
			if (result != sl::Result::eOk) {
				ERR_FAIL_MSG("Failed to call streamline slSetTag for NIS. Result: " + String(StreamlineContext::result_to_string(result)));
			}
		}

		{ // Evaluate NIS
			const sl::BaseStructure *inputs[] = { &context->viewport };
			sl::Result result = StreamlineContext::get().slEvaluateFeature(sl::kFeatureNIS, *StreamlineContext::get().last_token, inputs, 1, nativeCmdlist);
			if (result != sl::Result::eOk) {
				ERR_FAIL_MSG("Failed to call streamline slEvaluateFeature for NIS. Result: " + String(StreamlineContext::result_to_string(result)));
			}
		}
	}
}

void RendererRD::DLSSEffect::_upscale_internal_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {
	DLSSContextInner *self = (DLSSContextInner *)p_userdata;
	self->last_effect->_upscale_internal(p_command_buffer, self->last_parameters);
}

bool DLSSEffect::is_ready(DLSSContext *p_context) {
	DLSSContextInner *context = (DLSSContextInner *)p_context;
	if (context->currentDlssOptions.mode == sl::DLSSMode::eOff) {
		return false; // unsupported mode.
	}
	if (context->delay > 0) {
		return false; // still in delay mode
	}
	return true;
}
#elif defined(ENABLE_NGX_DLSS)

// ============================================================================
// NGX DLSS backend for Linux — bypasses Streamline, talks directly to NGX SDK
// ============================================================================

static bool g_ngx_initialized = false;
static NVSDK_NGX_Parameter *g_ngx_params = nullptr;
static bool g_ngx_dlss_rr_available = false;
static bool g_ngx_dlss_sr_available = false;

namespace RendererRD {
class NGXDLSSContext : public DLSSContext {
public:
	NVSDK_NGX_Handle *dlss_rr_handle = nullptr;
	NVSDK_NGX_Handle *dlss_sr_handle = nullptr;
	bool feature_created = false;
	bool feature_is_rr = false; // tracks which mode was created
	NVSDK_NGX_PerfQuality_Value perf_quality = NVSDK_NGX_PerfQuality_Value_Balanced;
	uint32_t render_width = 0;
	uint32_t render_height = 0;
	uint32_t output_width = 0;
	uint32_t output_height = 0;

	void release_features() {
		if (dlss_rr_handle) {
			NVSDK_NGX_VULKAN_ReleaseFeature(dlss_rr_handle);
			dlss_rr_handle = nullptr;
		}
		if (dlss_sr_handle) {
			NVSDK_NGX_VULKAN_ReleaseFeature(dlss_sr_handle);
			dlss_sr_handle = nullptr;
		}
		feature_created = false;
	}

	virtual ~NGXDLSSContext() {
		release_features();
	}
};
} // namespace RendererRD

static void _ngx_log_callback(const char *message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature source) {
	print_line(vformat("[NGX LOG] [%d] %s", (int)level, String(message)));
}

static bool _ngx_ensure_init() {
	if (g_ngx_initialized) {
		return true;
	}

	RenderingDevice *rd = RD::get_singleton();
	if (!rd) {
		return false;
	}

	VkInstance vk_instance = (VkInstance)rd->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TOPMOST_OBJECT, RID(), 0);
	VkPhysicalDevice vk_phys_device = (VkPhysicalDevice)rd->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_PHYSICAL_DEVICE, RID(), 0);
	VkDevice vk_device = (VkDevice)rd->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0);

	if (!vk_instance || !vk_phys_device || !vk_device) {
		ERR_PRINT("[NGX DLSS] Failed to get Vulkan handles from Godot RenderingDevice");
		return false;
	}

	print_line("[NGX DLSS] Vulkan handles acquired, initializing NGX...");

	// Create the log/data directory
	DirAccess::make_dir_absolute("/tmp/ngx_dlss_logs");

	// Tell NGX where to find DLSS feature .so files (libnvidia-ngx-dlss*.so)
	// Search order:
	//   1) next to executable (shipped game: game.x86_64 + .so in same dir)
	//   2) ../thirdparty/ngx/lib/linux_x86_64 (dev build: binary in bin/, .so at repo root)
	String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
	String ngx_shipped = exe_dir;
	String ngx_dev = exe_dir.path_join("../thirdparty/ngx/lib/linux_x86_64").simplify_path();

	// Convert to wchar_t for NGX API
	// On Linux wchar_t is 4 bytes (UTF-32), so widen char-by-char from UTF-8
	auto to_wstring = [](const String &s) -> std::wstring {
		CharString utf8 = s.utf8();
		std::wstring result;
		result.reserve(utf8.length());
		for (int i = 0; i < utf8.length(); i++) {
			result.push_back((wchar_t)(unsigned char)utf8[i]);
		}
		return result;
	};
	static thread_local std::wstring shipped_ws, dev_ws;
	shipped_ws = to_wstring(ngx_shipped);
	dev_ws = to_wstring(ngx_dev);
	const wchar_t *ngx_search_paths[] = {
		shipped_ws.c_str(),
		dev_ws.c_str(),
	};
	print_line(vformat("[NGX DLSS] Searching for DLSS .so in: %s, %s", ngx_shipped, ngx_dev));

	NVSDK_NGX_FeatureCommonInfo feature_info = {};
	memset(&feature_info, 0, sizeof(feature_info));
	feature_info.PathListInfo.Path = ngx_search_paths;
	feature_info.PathListInfo.Length = 2;
	feature_info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
	feature_info.LoggingInfo.LoggingCallback = _ngx_log_callback;
	feature_info.LoggingInfo.DisableOtherLoggingSinks = false;

	// Get the REAL Vulkan function pointers from the driver, not volk globals.
	// volk defines vkGetInstanceProcAddr as a global variable (function pointer),
	// but libnvsdk_ngx.a (statically linked) resolves the symbol to the variable
	// address and tries to call it as code — crashing on the pointer bytes.
	// dlsym gives us the actual driver function address.
	void *vulkan_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
	if (!vulkan_lib) {
		vulkan_lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_NOLOAD);
	}
	ERR_FAIL_COND_V_MSG(!vulkan_lib, false, "[NGX DLSS] Cannot find loaded libvulkan.so for proc addr");

	PFN_vkGetInstanceProcAddr real_vkGetInstanceProcAddr =
			(PFN_vkGetInstanceProcAddr)dlsym(vulkan_lib, "vkGetInstanceProcAddr");
	PFN_vkGetDeviceProcAddr real_vkGetDeviceProcAddr =
			(PFN_vkGetDeviceProcAddr)dlsym(vulkan_lib, "vkGetDeviceProcAddr");
	dlclose(vulkan_lib);

	ERR_FAIL_COND_V_MSG(!real_vkGetInstanceProcAddr || !real_vkGetDeviceProcAddr, false,
			"[NGX DLSS] dlsym failed for Vulkan proc addrs");

	print_verbose(vformat("[NGX DLSS] Real vkGetInstanceProcAddr: %d, volk global: %d",
			(uint64_t)real_vkGetInstanceProcAddr, (uint64_t)vkGetInstanceProcAddr));

	// Use Init_with_ProjectID as the nvpro-samples reference does
	NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
			"a0f57b54-1daf-4934-90ae-c4035c19df04", // Project ID (GUID format)
			NVSDK_NGX_ENGINE_TYPE_CUSTOM,
			"4.7.0",  // Engine version
			L"/tmp/ngx_dlss_logs",
			vk_instance, vk_phys_device, vk_device,
			real_vkGetInstanceProcAddr, real_vkGetDeviceProcAddr,
			&feature_info,
			NVSDK_NGX_Version_API);

	if (NVSDK_NGX_FAILED(result)) {
		ERR_PRINT(vformat("[NGX DLSS] NVSDK_NGX_VULKAN_Init_Ext2 failed: 0x%x", (uint32_t)result));
		return false;
	}

	// Query required extensions (for diagnostics)
	{
		unsigned int inst_count = 0, dev_count = 0;
		const char **inst_exts = nullptr, **dev_exts = nullptr;
		NVSDK_NGX_Result ext_result = NVSDK_NGX_VULKAN_RequiredExtensions(&inst_count, &inst_exts, &dev_count, &dev_exts);
		if (NVSDK_NGX_SUCCEED(ext_result)) {
			print_line(vformat("[NGX DLSS] Required instance extensions: %d", inst_count));
			for (unsigned int i = 0; i < inst_count; i++) {
				print_line(vformat("[NGX DLSS]   Instance: %s", String(inst_exts[i])));
			}
			print_line(vformat("[NGX DLSS] Required device extensions: %d", dev_count));
			for (unsigned int i = 0; i < dev_count; i++) {
				print_line(vformat("[NGX DLSS]   Device: %s", String(dev_exts[i])));
			}
		} else {
			print_line(vformat("[NGX DLSS] RequiredExtensions query failed: 0x%x", (uint32_t)ext_result));
		}
	}

	// Allocate parameters (needed for feature creation/evaluation)
	result = NVSDK_NGX_VULKAN_AllocateParameters(&g_ngx_params);
	if (NVSDK_NGX_FAILED(result)) {
		ERR_PRINT(vformat("[NGX DLSS] AllocateParameters failed: 0x%x", (uint32_t)result));
		NVSDK_NGX_VULKAN_Shutdown1(nullptr);
		return false;
	}

	// Skip capability parameter checks — they're unreliable on Linux.
	// NGX loaded both dlss and dlssd .so files (confirmed via nvngx.log).
	// We'll try creating features at render time and handle errors there.
	g_ngx_dlss_sr_available = true;
	g_ngx_dlss_rr_available = true;

	g_ngx_initialized = true;
	print_line("[NGX DLSS] Initialized successfully. Will attempt DLSS-RR then fall back to SR.");

	return true;
}

static NVSDK_NGX_Resource_VK _ngx_make_resource(RID p_texture_rid, bool p_read_write) {
	VkImageView vk_view = (VkImageView)RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_VIEW, p_texture_rid);
	VkImage vk_image = (VkImage)RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE, p_texture_rid);
	VkFormat vk_format = (VkFormat)RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_texture_rid);
	RD::TextureFormat tex_fmt = RD::get_singleton()->texture_get_format(p_texture_rid);

	VkImageSubresourceRange subresource = {};
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	subresource.baseMipLevel = 0;
	subresource.levelCount = tex_fmt.mipmaps;
	subresource.baseArrayLayer = 0;
	subresource.layerCount = tex_fmt.array_layers;

	return NVSDK_NGX_Create_ImageView_Resource_VK(
			vk_view, vk_image, subresource, vk_format,
			tex_fmt.width, tex_fmt.height, p_read_write);
}

DLSSEffect::DLSSEffect() {
	Vector<String> modes;
	modes.push_back("\n");
	shaders.mvec_decode_shader.initialize(modes, "");
	shaders.mvec_decode_version = shaders.mvec_decode_shader.version_create();
	shaders.mvec_decode_pipeline = RD::get_singleton()->compute_pipeline_create(shaders.mvec_decode_shader.version_get_shader(shaders.mvec_decode_version, 0));
}

DLSSEffect::~DLSSEffect() {
	shaders.mvec_decode_shader.version_free(shaders.mvec_decode_version);
	if (g_ngx_initialized) {
		if (g_ngx_params) {
			NVSDK_NGX_VULKAN_DestroyParameters(g_ngx_params);
			g_ngx_params = nullptr;
		}
		NVSDK_NGX_VULKAN_Shutdown1(nullptr);
		g_ngx_initialized = false;
	}
}

DLSSContext *DLSSEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
	if (!_ngx_ensure_init()) {
		ERR_PRINT("[NGX DLSS] Cannot create context — NGX init failed");
		return nullptr;
	}

	RendererRD::NGXDLSSContext *ctx = memnew(RendererRD::NGXDLSSContext);
	ctx->render_width = p_internal_size.width;
	ctx->render_height = p_internal_size.height;
	ctx->output_width = p_target_size.width;
	ctx->output_height = p_target_size.height;

	// Determine quality mode based on render/output ratio
	float ratio = (float)p_internal_size.width / (float)p_target_size.width;
	if (ratio >= 0.95f) {
		ctx->perf_quality = NVSDK_NGX_PerfQuality_Value_DLAA;
	} else if (ratio >= 0.66f) {
		ctx->perf_quality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
	} else if (ratio >= 0.5f) {
		ctx->perf_quality = NVSDK_NGX_PerfQuality_Value_Balanced;
	} else if (ratio >= 0.33f) {
		ctx->perf_quality = NVSDK_NGX_PerfQuality_Value_MaxPerf;
	} else {
		ctx->perf_quality = NVSDK_NGX_PerfQuality_Value_UltraPerformance;
	}

	print_line(vformat("[NGX DLSS] Context created: %dx%d -> %dx%d (quality: %d)",
			p_internal_size.width, p_internal_size.height,
			p_target_size.width, p_target_size.height, (int)ctx->perf_quality));

	return ctx;
}

void DLSSEffect::upscale(const DLSSContext::Parameters &p_params) {
	RendererRD::NGXDLSSContext *ctx = (RendererRD::NGXDLSSContext *)p_params.context;
	if (!ctx) {
		return;
	}

	// Delay warmup frames
	if (ctx->delay > 0) {
		--ctx->delay;
		return;
	}

	ctx->last_parameters = p_params;
	ctx->last_effect = this;

	// Decode motion vectors (same as Streamline path)
	{
		RD::get_singleton()->draw_command_begin_label("Decode Invalid Motion Vectors");
		UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
		ERR_FAIL_NULL(uniform_set_cache);
		MaterialStorage *material_storage = MaterialStorage::get_singleton();
		ERR_FAIL_NULL(material_storage);

		RD::Uniform u_velocity_image(RD::UNIFORM_TYPE_IMAGE, 0, p_params.velocity);
		RD::Uniform u_depth_texture(RD::UNIFORM_TYPE_TEXTURE, 0, p_params.depth);

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RID shader = shaders.mvec_decode_shader.version_get_shader(shaders.mvec_decode_version, 0);
		ERR_FAIL_COND(shader.is_null());

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, shaders.mvec_decode_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_velocity_image), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_depth_texture), 1);

		auto texture_format = RD::get_singleton()->texture_get_format(p_params.velocity);

		float push_constants[20];
		push_constants[0] = texture_format.width;
		push_constants[1] = texture_format.height;
		push_constants[2] = 0.0f;
		push_constants[3] = 0.0f;
		memcpy(push_constants + 4, &p_params.reprojection.columns[0].x, sizeof(float) * 16);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, push_constants, sizeof(push_constants));

		RD::get_singleton()->compute_list_dispatch_threads(compute_list, texture_format.width, texture_format.height, 1);
		RD::get_singleton()->compute_list_add_barrier(compute_list);

		RD::get_singleton()->compute_list_end();
		RD::get_singleton()->draw_command_end_label();
	}

	// Inject NGX DLSS into the render graph via callback
	RD::CallbackResource res[8];
	int num_resources = 0;
	res[num_resources++].rid = p_params.color;
	res[num_resources++].rid = p_params.output;
	res[num_resources++].rid = p_params.depth;
	res[num_resources++].rid = p_params.velocity;

	if (p_params.dlss_rr) {
		if (p_params.dlss_rr_diffuse_albedo.is_valid()) {
			res[num_resources++].rid = p_params.dlss_rr_diffuse_albedo;
		}
		if (p_params.dlss_rr_specular_albedo.is_valid()) {
			res[num_resources++].rid = p_params.dlss_rr_specular_albedo;
		}
		if (p_params.dlss_rr_normal_roughness.is_valid()) {
			res[num_resources++].rid = p_params.dlss_rr_normal_roughness;
		}
		if (p_params.dlss_rr_specular_hit_dist.is_valid()) {
			res[num_resources++].rid = p_params.dlss_rr_specular_hit_dist;
		}
	}

	for (int i = 0; i < num_resources; i++) {
		res[i].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	}
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)DLSSEffect::_upscale_internal_graph_callback, p_params.context, VectorView<RD::CallbackResource>(res, num_resources));
}

void DLSSEffect::_upscale_internal(RDD::CommandBufferID cmdid, const DLSSContext::Parameters &p_params) {
	RendererRD::NGXDLSSContext *ctx = (RendererRD::NGXDLSSContext *)p_params.context;

	VkCommandBuffer vk_cmd = (VkCommandBuffer)RD::get_singleton()->get_device_driver()->command_buffer_get_native_handle(cmdid);
	VkDevice vk_device = (VkDevice)RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0);

	bool use_dlss_rr = p_params.dlss_rr && g_ngx_dlss_rr_available;

	// Recreate feature if mode changed (RR <-> SR)
	if (ctx->feature_created && ctx->feature_is_rr != use_dlss_rr) {
		print_line(vformat("[NGX DLSS] Mode changed (%s -> %s), recreating feature",
				ctx->feature_is_rr ? "RR" : "SR", use_dlss_rr ? "RR" : "SR"));
		ctx->release_features();
	}

	// Create NGX feature on first use or after mode change
	if (!ctx->feature_created) {
		NVSDK_NGX_Result result;

		// Try DLSS-RR (Ray Reconstruction / denoiser) first if requested
		if (use_dlss_rr && g_ngx_dlss_rr_available) {
			NVSDK_NGX_DLSSD_Create_Params dlssd_create = {};
			dlssd_create.InWidth = ctx->render_width;
			dlssd_create.InHeight = ctx->render_height;
			dlssd_create.InTargetWidth = ctx->output_width;
			dlssd_create.InTargetHeight = ctx->output_height;
			dlssd_create.InPerfQualityValue = ctx->perf_quality;
			// AutoExposure ON: Streamline reference uses AutoExposure when no
			// exposure texture is provided. The pathtracer has no exposure texture.
			dlssd_create.InFeatureCreateFlags =
					NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
					NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
					NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
					NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

			result = NGX_VULKAN_CREATE_DLSSD_EXT1(
					VK_NULL_HANDLE, vk_cmd, 1, 1,
					&ctx->dlss_rr_handle, g_ngx_params, &dlssd_create);

			if (NVSDK_NGX_SUCCEED(result)) {
				ctx->feature_created = true;
				ctx->feature_is_rr = true;
				print_line("[NGX DLSS] DLSS-RR feature created successfully");
			} else {
				WARN_PRINT(vformat("[NGX DLSS] DLSS-RR creation failed (0x%x), falling back to DLSS-SR", (uint32_t)result));
				ctx->dlss_rr_handle = nullptr;
				use_dlss_rr = false;
			}
		}

		// Fall back to DLSS-SR (Super Resolution / upscaler only)
		if (!ctx->feature_created && g_ngx_dlss_sr_available) {
			NVSDK_NGX_DLSS_Create_Params dlss_create = {};
			dlss_create.Feature.InWidth = ctx->render_width;
			dlss_create.Feature.InHeight = ctx->render_height;
			dlss_create.Feature.InTargetWidth = ctx->output_width;
			dlss_create.Feature.InTargetHeight = ctx->output_height;
			dlss_create.Feature.InPerfQualityValue = ctx->perf_quality;
			dlss_create.InFeatureCreateFlags =
					NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
					NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
					NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
					NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

			result = NGX_VULKAN_CREATE_DLSS_EXT(
					vk_cmd, 1, 1,
					&ctx->dlss_sr_handle, g_ngx_params, &dlss_create);

			if (NVSDK_NGX_FAILED(result)) {
				ERR_PRINT(vformat("[NGX DLSS] DLSS-SR feature creation failed: 0x%x", (uint32_t)result));
				return;
			}
			ctx->feature_created = true;
			ctx->feature_is_rr = false;
			use_dlss_rr = false;
			print_line("[NGX DLSS] DLSS-SR feature created successfully");
		}

		if (!ctx->feature_created) {
			return;
		}
	}

	// Evaluate DLSS
	if (use_dlss_rr && ctx->dlss_rr_handle) {
		// DLSS Ray Reconstruction evaluation
		NVSDK_NGX_Resource_VK color_res = _ngx_make_resource(p_params.color, false);
		NVSDK_NGX_Resource_VK output_res = _ngx_make_resource(p_params.output, true);
		NVSDK_NGX_Resource_VK depth_res = _ngx_make_resource(p_params.depth, false);
		NVSDK_NGX_Resource_VK mvec_res = _ngx_make_resource(p_params.velocity, false);

		NVSDK_NGX_VK_DLSSD_Eval_Params eval = {};
		memset(&eval, 0, sizeof(eval));
		eval.pInColor = &color_res;
		eval.pInOutput = &output_res;
		eval.pInDepth = &depth_res;
		eval.pInMotionVectors = &mvec_res;
		eval.InJitterOffsetX = p_params.jitter.x;
		eval.InJitterOffsetY = p_params.jitter.y;
		eval.InRenderSubrectDimensions.Width = ctx->render_width;
		eval.InRenderSubrectDimensions.Height = ctx->render_height;
		eval.InReset = p_params.reset_accumulation ? 1 : 0;
		// Motion vectors are in UV space (0-1), scale to pixel space for DLSS
		eval.InMVScaleX = (float)ctx->render_width;
		eval.InMVScaleY = (float)ctx->render_height;
		eval.InFrameTimeDeltaInMsec = p_params.delta_time * 1000.0f;

		// G-buffer inputs for DLSS-RR (stack-allocated, valid for duration of evaluate call)
		NVSDK_NGX_Resource_VK diffuse_res, specular_res, normal_res, hit_dist_res;
		if (p_params.dlss_rr_diffuse_albedo.is_valid()) {
			diffuse_res = _ngx_make_resource(p_params.dlss_rr_diffuse_albedo, false);
			eval.pInDiffuseAlbedo = &diffuse_res;
		}
		if (p_params.dlss_rr_specular_albedo.is_valid()) {
			specular_res = _ngx_make_resource(p_params.dlss_rr_specular_albedo, false);
			eval.pInSpecularAlbedo = &specular_res;
		}
		if (p_params.dlss_rr_normal_roughness.is_valid()) {
			normal_res = _ngx_make_resource(p_params.dlss_rr_normal_roughness, false);
			eval.pInNormals = &normal_res;
			eval.pInRoughness = &normal_res; // Packed mode: roughness in W channel of normals
		}
		if (p_params.dlss_rr_specular_hit_dist.is_valid()) {
			hit_dist_res = _ngx_make_resource(p_params.dlss_rr_specular_hit_dist, false);
			eval.pInSpecularHitDistance = &hit_dist_res;
		}

		// View/projection matrices for DLSS-RR
		Transform3D view_matrix = p_params.cam_transform.affine_inverse();
		Projection view_proj(view_matrix);
		float world_to_view[16];
		float view_to_clip[16];
		for (int i = 0; i < 4; i++) {
			world_to_view[i * 4 + 0] = view_proj.columns[0][i];
			world_to_view[i * 4 + 1] = view_proj.columns[1][i];
			world_to_view[i * 4 + 2] = view_proj.columns[2][i];
			world_to_view[i * 4 + 3] = view_proj.columns[3][i];
		}
		for (int i = 0; i < 4; i++) {
			view_to_clip[i * 4 + 0] = p_params.cam_projection.columns[0][i];
			view_to_clip[i * 4 + 1] = p_params.cam_projection.columns[1][i];
			view_to_clip[i * 4 + 2] = p_params.cam_projection.columns[2][i];
			view_to_clip[i * 4 + 3] = p_params.cam_projection.columns[3][i];
		}
		eval.pInWorldToViewMatrix = world_to_view;
		eval.pInViewToClipMatrix = view_to_clip;

		NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSSD_EXT(vk_cmd, ctx->dlss_rr_handle, g_ngx_params, &eval);

		if (NVSDK_NGX_FAILED(result)) {
			ERR_PRINT(vformat("[NGX DLSS] DLSS-RR evaluate failed: 0x%x", (uint32_t)result));
		}
	} else if (ctx->dlss_sr_handle) {
		// DLSS Super Resolution evaluation
		NVSDK_NGX_Resource_VK color_res = _ngx_make_resource(p_params.color, false);
		NVSDK_NGX_Resource_VK output_res = _ngx_make_resource(p_params.output, true);
		NVSDK_NGX_Resource_VK depth_res = _ngx_make_resource(p_params.depth, false);
		NVSDK_NGX_Resource_VK mvec_res = _ngx_make_resource(p_params.velocity, false);

		NVSDK_NGX_VK_DLSS_Eval_Params eval = {};
		memset(&eval, 0, sizeof(eval));
		eval.Feature.pInColor = &color_res;
		eval.Feature.pInOutput = &output_res;
		eval.Feature.InSharpness = p_params.sharpness;
		eval.pInDepth = &depth_res;
		eval.pInMotionVectors = &mvec_res;
		eval.InJitterOffsetX = p_params.jitter.x;
		eval.InJitterOffsetY = p_params.jitter.y;
		eval.InRenderSubrectDimensions.Width = ctx->render_width;
		eval.InRenderSubrectDimensions.Height = ctx->render_height;
		eval.InReset = p_params.reset_accumulation ? 1 : 0;
		// Motion vectors are in UV space (0-1), scale to pixel space for DLSS
		eval.InMVScaleX = (float)ctx->render_width;
		eval.InMVScaleY = (float)ctx->render_height;
		eval.InFrameTimeDeltaInMsec = p_params.delta_time * 1000.0f;
		eval.InPreExposure = 1.0f;
		eval.InExposureScale = 1.0f;

		NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT(vk_cmd, ctx->dlss_sr_handle, g_ngx_params, &eval);
		if (NVSDK_NGX_FAILED(result)) {
			ERR_PRINT(vformat("[NGX DLSS] DLSS-SR evaluate failed: 0x%x", (uint32_t)result));
		}
	}
}

void RendererRD::DLSSEffect::_upscale_internal_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {
	RendererRD::NGXDLSSContext *self = (RendererRD::NGXDLSSContext *)p_userdata;
	self->last_effect->_upscale_internal(p_command_buffer, self->last_parameters);
}

bool DLSSEffect::is_ready(DLSSContext *p_context) {
	RendererRD::NGXDLSSContext *ctx = (RendererRD::NGXDLSSContext *)p_context;
	if (!ctx || !g_ngx_initialized) {
		return false;
	}
	if (ctx->delay > 0) {
		return false;
	}
	return true;
}

#else
// ============================================================================
// No DLSS backend available — stub implementation
// ============================================================================
DLSSEffect::DLSSEffect() {}
DLSSEffect::~DLSSEffect() {}
DLSSContext *DLSSEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
	return nullptr;
}
void DLSSEffect::upscale(const DLSSContext::Parameters &p_params) {}
bool DLSSEffect::is_ready(DLSSContext *p_context) {
	return false;
}
void DLSSEffect::_upscale_internal(RDD::CommandBufferID cmdid, const DLSSContext::Parameters &p_params) {}
void DLSSEffect::_upscale_internal_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {}
#endif
