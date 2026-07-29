/**************************************************************************/
/*  bindless_block.h                                                      */
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

#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/rendering_device.h"

class BindlessBlock {
public:
	constexpr static uint32_t MAX_BINDLESS_TEXTURES = 128000;

	// GLSL cannot construct a sampler of one dimensionality from a texture of
	// another, so each texture type needs its OWN bindless table -- a single
	// texture2D[] table makes every sampler2DArray/sampler3D/samplerCube uniform
	// generate sampler2DArray(texture2D, ...), which glslang rejects, failing the
	// whole hit group and rendering nothing.
	//
	// Vulkan permits only one variable-count (unbounded) binding per descriptor
	// set, and it must be the binding with the LARGEST binding number
	// (VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-pBindingFlags-03004; also
	// enforced in rendering_device_driver_vulkan.cpp). So the rarely-used typed
	// tables are fixed-capacity at the low bindings and the 2D table -- the one
	// that actually needs to scale -- stays unbounded at the highest binding.
	enum TextureKind {
		TEXTURE_2D_ARRAY,
		TEXTURE_3D,
		TEXTURE_CUBE,
		TEXTURE_CUBE_ARRAY,
		TEXTURE_2D, // Unbounded; MUST remain the highest binding.
		TEXTURE_KIND_MAX
	};

	// Fixed capacity of the typed (bounded) tables. Bounded bindings do not get
	// PARTIALLY_BOUND, so these are always written in full, padded with the
	// kind's default texture.
	constexpr static uint32_t TYPED_TABLE_CAPACITY = 256;

	static bool is_unbounded(TextureKind p_kind) { return p_kind == TEXTURE_2D; }
	static uint32_t get_binding(TextureKind p_kind) { return (uint32_t)p_kind; }

	uint32_t add_texture(RID p_texture);

	void initialize(RenderingDevice *p_rd, TextureKind p_kind = TEXTURE_2D);

	// p_set_valid: whether the shared uniform set this table belongs to is still
	// live. When it is not, dead texture RIDs are scrubbed and a rebuild is
	// requested. The set itself is owned by the caller, since all kinds share one.
	void begin_frame(bool p_set_valid);

	// Appends this table's binding to p_uniforms. Every table in the set must be
	// collected before the set is created, so creation is the caller's job.
	void collect_uniform(Vector<RD::Uniform> &r_uniforms) const;

	bool needs_rebuild() const { return needs_refinalize; }
	void mark_rebuilt() { needs_refinalize = false; }

	uint32_t get_texture_count() const { return textures.size(); }
	TextureKind get_kind() const { return kind; }
	bool is_initialized() const { return rd != nullptr; }

	void clear();
	~BindlessBlock();

private:
	RenderingDevice *rd = nullptr;
	TextureKind kind = TEXTURE_2D;
	LocalVector<RID> textures;
	HashMap<RID, uint32_t> texture_to_index;
	LocalVector<uint32_t> free_indices;
	uint32_t free_indices_count = 0; // logical top to avoid resize of free_indices array
	RID default_texture;
	bool needs_refinalize = true;
};
