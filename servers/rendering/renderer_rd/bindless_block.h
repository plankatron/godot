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

	// One table per texture type: GLSL cannot build a sampler2DArray/sampler3D/samplerCube from a texture2D,
	// and RenderingDevice rejects the whole set if one texture's type differs from its binding's.
	// Only the highest binding of a set may be variable-count, so the typed tables have a fixed capacity at
	// bindings 0-3 and the 2D table, the only one that needs to scale, is unbounded at binding 4.
	// Keep in sync with the bindless_textures* declarations in the raytracing and fog shaders.
	enum TextureKind {
		TEXTURE_KIND_2D_ARRAY,
		TEXTURE_KIND_3D,
		TEXTURE_KIND_CUBE,
		TEXTURE_KIND_CUBE_ARRAY,
		TEXTURE_KIND_2D, // Unbounded, so it must stay the highest binding.
		TEXTURE_KIND_MAX
	};
	constexpr static uint32_t TYPED_TABLE_CAPACITY = 256;

	// Index into p_kind's table. A texture of another type gets that table's default (index 0).
	uint32_t add_texture(RID p_texture, TextureKind p_kind = TEXTURE_KIND_2D);
	void replace_texture(RID p_old, RID p_new);

	void initialize(RenderingDevice *p_rd);

	void begin_frame();

	void finalize(RID p_shader, uint32_t p_set_index);

	// For consumers whose set layout differs from the finalized one (e.g. other shader stages).
	// Recreate when get_version() changes.
	RID create_uniform_set(RID p_shader, uint32_t p_set_index) const;
	uint64_t get_version() const { return version; }

	RID get_uniform_set() const { return uniform_set; }
	uint32_t get_texture_count(TextureKind p_kind = TEXTURE_KIND_2D) const { return tables[p_kind].textures.size(); }
	bool is_initialized() const { return rd != nullptr; }
	bool is_finalized() const { return uniform_set.is_valid(); }

	void clear();
	~BindlessBlock();

private:
	struct Table {
		LocalVector<RID> textures;
		HashMap<RID, uint32_t> texture_to_index;
		LocalVector<uint32_t> free_indices;
		uint32_t free_indices_count = 0; // logical top to avoid resize of free_indices array
		RID default_texture;
	};

	RenderingDevice *rd = nullptr;
	Table tables[TEXTURE_KIND_MAX];
	RID uniform_set;
	bool needs_refinalize = false;
	uint64_t version = 0;
};
