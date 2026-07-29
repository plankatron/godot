/**************************************************************************/
/*  bindless_block.cpp                                                    */
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

#include "bindless_block.h"

#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"

void BindlessBlock::initialize(RenderingDevice *p_rd, TextureKind p_kind) {
	ERR_FAIL_NULL(p_rd);

	rd = p_rd;
	kind = p_kind;

	RendererRD::TextureStorage::DefaultRDTexture default_id = RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE;
	switch (kind) {
		case TEXTURE_2D_ARRAY:
			default_id = RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE;
			break;
		case TEXTURE_3D:
			default_id = RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE;
			break;
		case TEXTURE_CUBE:
			default_id = RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_CUBEMAP_WHITE;
			break;
		case TEXTURE_CUBE_ARRAY:
			default_id = RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_CUBEMAP_ARRAY_WHITE;
			break;
		default:
			break;
	}

	default_texture = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(default_id);
	textures.push_back(default_texture);
	texture_to_index[default_texture] = 0;
	needs_refinalize = true;
}

void BindlessBlock::begin_frame(bool p_set_valid) {
	if (!is_initialized()) {
		return;
	}

	if (p_set_valid) {
		return;
	}

	// Uniform set was externally invalidated (a texture was destroyed/recreated).
	// Scrub dead RIDs: replace with default and add slots to the freelist.
	texture_to_index.clear();
	texture_to_index[default_texture] = 0;

	for (uint32_t i = 1; i < textures.size(); i++) {
		if (!rd->texture_is_valid(textures[i])) {
			textures[i] = default_texture;
			if (free_indices_count < free_indices.size()) {
				free_indices[free_indices_count] = i;
			} else {
				free_indices.push_back(i);
			}
			++free_indices_count;
		} else {
			texture_to_index[textures[i]] = i;
		}
	}

	needs_refinalize = true;
}

uint32_t BindlessBlock::add_texture(RID p_texture) {
	ERR_FAIL_COND_V_MSG(!is_initialized(), 0, "BindlessBlock not initialized. Call initialize() first.");

	// Substitute null/stale RIDs with the default white texture.
	if (!p_texture.is_valid() || !rd->texture_is_valid(p_texture)) {
		p_texture = default_texture;
	}

	HashMap<RID, uint32_t>::Iterator it = texture_to_index.find(p_texture);
	if (it != texture_to_index.end()) {
		return it->value;
	}

	const uint32_t capacity = is_unbounded(kind) ? MAX_BINDLESS_TEXTURES : TYPED_TABLE_CAPACITY;
	if (textures.size() >= capacity && free_indices_count == 0) {
		ERR_PRINT_ONCE("BindlessBlock: Maximum texture count exceeded. Using default texture.");
		return 0;
	}

	needs_refinalize = true;

	uint32_t index;
	if (free_indices_count > 0) {
		--free_indices_count;
		index = free_indices[free_indices_count];
		textures[index] = p_texture;
	} else {
		index = textures.size();
		textures.push_back(p_texture);
	}

	texture_to_index[p_texture] = index;
	return index;
}

void BindlessBlock::collect_uniform(Vector<RD::Uniform> &r_uniforms) const {
	ERR_FAIL_COND_MSG(!is_initialized(), "BindlessBlock not initialized.");
	ERR_FAIL_COND_MSG(textures.is_empty(), "BindlessBlock has no textures.");

	RD::Uniform u;
	u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
	u.binding = get_binding(kind);
	u.variable_count = is_unbounded(kind);

	for (uint32_t i = 0; i < textures.size(); i++) {
		u.append_id(textures[i]);
	}

	// A bounded binding gets no PARTIALLY_BOUND flag, so every descriptor in the
	// declared array must be written; pad the tail with this kind's default.
	if (!is_unbounded(kind)) {
		for (uint32_t i = textures.size(); i < TYPED_TABLE_CAPACITY; i++) {
			u.append_id(default_texture);
		}
	}

	r_uniforms.push_back(u);
}

void BindlessBlock::clear() {
	// The uniform set is shared across kinds and owned by the caller.
	textures.clear();
	texture_to_index.clear();
	free_indices.clear();
	free_indices_count = 0;
	needs_refinalize = true;
	rd = nullptr;
}

BindlessBlock::~BindlessBlock() {
	clear();
}
