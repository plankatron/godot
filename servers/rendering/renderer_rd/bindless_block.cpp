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

static RD::TextureType _kind_texture_type(BindlessBlock::TextureKind p_kind) {
	switch (p_kind) {
		case BindlessBlock::TEXTURE_KIND_2D_ARRAY:
			return RD::TEXTURE_TYPE_2D_ARRAY;
		case BindlessBlock::TEXTURE_KIND_3D:
			return RD::TEXTURE_TYPE_3D;
		case BindlessBlock::TEXTURE_KIND_CUBE:
			return RD::TEXTURE_TYPE_CUBE;
		case BindlessBlock::TEXTURE_KIND_CUBE_ARRAY:
			return RD::TEXTURE_TYPE_CUBE_ARRAY;
		default:
			return RD::TEXTURE_TYPE_2D;
	}
}

void BindlessBlock::initialize(RenderingDevice *p_rd) {
	ERR_FAIL_NULL(p_rd);

	rd = p_rd;

	static const RendererRD::TextureStorage::DefaultRDTexture defaults[TEXTURE_KIND_MAX] = {
		RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE,
		RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE,
		RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_CUBEMAP_WHITE,
		RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_CUBEMAP_ARRAY_WHITE,
		RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE,
	};
	for (uint32_t k = 0; k < TEXTURE_KIND_MAX; k++) {
		Table &table = tables[k];
		table.default_texture = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(defaults[k]);
		table.textures.push_back(table.default_texture);
		table.texture_to_index[table.default_texture] = 0;
	}
}

void BindlessBlock::begin_frame() {
	if (!is_initialized()) {
		return;
	}

	bool set_valid = uniform_set.is_valid() && rd->uniform_set_is_valid(uniform_set);
	if (set_valid) {
		return;
	}

	// Uniform set was externally invalidated (a texture was destroyed/recreated).
	// Scrub dead RIDs: replace with default and add slots to the freelist.
	uniform_set = RID();

	for (Table &table : tables) {
		table.texture_to_index.clear();
		table.texture_to_index[table.default_texture] = 0;

		for (uint32_t i = 1; i < table.textures.size(); i++) {
			if (!rd->texture_is_valid(table.textures[i])) {
				table.textures[i] = table.default_texture;
				if (table.free_indices_count < table.free_indices.size()) {
					table.free_indices[table.free_indices_count] = i;
				} else {
					table.free_indices.push_back(i);
				}
				++table.free_indices_count;
			} else {
				table.texture_to_index[table.textures[i]] = i;
			}
		}
	}

	needs_refinalize = true;
	version++;
}

uint32_t BindlessBlock::add_texture(RID p_texture, TextureKind p_kind) {
	ERR_FAIL_COND_V_MSG(!is_initialized(), 0, "BindlessBlock not initialized. Call initialize() first.");
	ERR_FAIL_INDEX_V(p_kind, TEXTURE_KIND_MAX, 0);
	Table &table = tables[p_kind];

	// Substitute null/stale RIDs with the default white texture.
	if (!p_texture.is_valid() || !rd->texture_is_valid(p_texture)) {
		p_texture = table.default_texture;
	}

	HashMap<RID, uint32_t>::Iterator it = table.texture_to_index.find(p_texture);
	if (it != table.texture_to_index.end()) {
		return it->value;
	}

	// RenderingDevice fails the whole set when one texture's type differs from its binding's, which would
	// take every material's textures down with it. Such a texture (e.g. a 2D array bound to a sampler2D
	// uniform) samples this table's default instead.
	if (rd->texture_get_format(p_texture).texture_type != _kind_texture_type(p_kind)) {
		return 0;
	}

	const uint32_t capacity = p_kind == TEXTURE_KIND_2D ? MAX_BINDLESS_TEXTURES : TYPED_TABLE_CAPACITY;
	if (table.textures.size() >= capacity && table.free_indices_count == 0) {
		ERR_PRINT_ONCE("BindlessBlock: Maximum texture count exceeded. Using default texture.");
		return 0;
	}

	if (is_finalized()) {
		needs_refinalize = true;
	}

	uint32_t index;
	if (table.free_indices_count > 0) {
		--table.free_indices_count;
		index = table.free_indices[table.free_indices_count];
		table.textures[index] = p_texture;
	} else {
		index = table.textures.size();
		table.textures.push_back(p_texture);
	}

	table.texture_to_index[p_texture] = index;
	version++;
	return index;
}

void BindlessBlock::replace_texture(RID p_old, RID p_new) {
	for (Table &table : tables) {
		HashMap<RID, uint32_t>::Iterator it = table.texture_to_index.find(p_old);
		if (it == table.texture_to_index.end()) {
			continue;
		}
		const uint32_t index = it->value;
		table.texture_to_index.remove(it);
		table.textures[index] = p_new;
		if (!table.texture_to_index.has(p_new)) {
			table.texture_to_index[p_new] = index;
		}
	}
}

RID BindlessBlock::create_uniform_set(RID p_shader, uint32_t p_set_index) const {
	ERR_FAIL_COND_V_MSG(!is_initialized(), RID(), "BindlessBlock not initialized.");

	// Every table is supplied; uniform_set_create() only binds the ones p_shader declares.
	Vector<RD::Uniform> uniforms;
	for (uint32_t k = 0; k < TEXTURE_KIND_MAX; k++) {
		const Table &table = tables[k];
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.binding = k;
		u.variable_count = k == TEXTURE_KIND_2D;
		for (uint32_t i = 0; i < table.textures.size(); i++) {
			u.append_id(table.textures[i]);
		}
		// Fixed-size bindings are not partially bound, so every element must be written.
		if (k != TEXTURE_KIND_2D) {
			for (uint32_t i = table.textures.size(); i < TYPED_TABLE_CAPACITY; i++) {
				u.append_id(table.default_texture);
			}
		}
		uniforms.push_back(u);
	}

	return rd->uniform_set_create(uniforms, p_shader, p_set_index);
}

void BindlessBlock::finalize(RID p_shader, uint32_t p_set_index) {
	ERR_FAIL_COND_MSG(!is_initialized(), "BindlessBlock not initialized.");

	if (is_finalized() && !needs_refinalize && rd->uniform_set_is_valid(uniform_set)) {
		return;
	}

	if (uniform_set.is_valid() && rd->uniform_set_is_valid(uniform_set)) {
		rd->free_rid(uniform_set);
	}
	uniform_set = create_uniform_set(p_shader, p_set_index);
	ERR_FAIL_COND_MSG(!uniform_set.is_valid(), "Failed to create bindless uniform set.");
	rd->set_resource_name(uniform_set, "Bindless Texture Set");

	needs_refinalize = false;
}

void BindlessBlock::clear() {
	if (uniform_set.is_valid() && rd && rd->uniform_set_is_valid(uniform_set)) {
		rd->free_rid(uniform_set);
	}
	uniform_set = RID();
	for (Table &table : tables) {
		table.textures.clear();
		table.texture_to_index.clear();
		table.free_indices.clear();
		table.free_indices_count = 0;
		table.default_texture = RID();
	}
	rd = nullptr;
}

BindlessBlock::~BindlessBlock() {
	clear();
}
