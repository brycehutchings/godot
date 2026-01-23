/**************************************************************************/
/*  resolve.cpp                                                           */
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

#include "resolve.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

Resolve::Resolve(bool p_supports_storage) {
	supports_storage = p_supports_storage;

	{
		Vector<String> resolve_raster_modes;
		resolve_raster_modes.push_back("\n#define MODE_OUTPUT_COLOR_BUFFER\n");
		resolve_raster_modes.push_back("\n#define MODE_OUTPUT_DEPTH_BUFFER\n");

		resolve_raster.shader.initialize(resolve_raster_modes);
		resolve_raster.shader_version = resolve_raster.shader.version_create();

		// Color buffer output mode
		resolve_raster.pipelines[RESOLVE_RASTER_MODE_COLOR_BUFFER].setup(resolve_raster.shader.version_get_shader(resolve_raster.shader_version, RESOLVE_RASTER_MODE_COLOR_BUFFER), RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_disabled(), 0);

		// Depth buffer output mode
		RD::PipelineDepthStencilState depth_stencil_state;
		depth_stencil_state.enable_depth_test = true;
		depth_stencil_state.depth_compare_operator = RD::COMPARE_OP_ALWAYS;
		depth_stencil_state.enable_depth_write = true;
		resolve_raster.pipelines[RESOLVE_RASTER_MODE_DEPTH_BUFFER].setup(resolve_raster.shader.version_get_shader(resolve_raster.shader_version, RESOLVE_RASTER_MODE_DEPTH_BUFFER), RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), depth_stencil_state, RD::PipelineColorBlendState::create_disabled(), 0);
	}

	// Initialize compute shaders for GI resolve and depth resolve when textures can have storage bit.
	if (supports_storage) {
		Vector<String> resolve_modes;
		resolve_modes.push_back("\n#define MODE_RESOLVE_GI\n");
		resolve_modes.push_back("\n#define MODE_RESOLVE_GI\n#define VOXEL_GI_RESOLVE\n");
		resolve_modes.push_back("\n#define MODE_RESOLVE_DEPTH\n");

		resolve.shader.initialize(resolve_modes);

		resolve.shader_version = resolve.shader.version_create();

		for (int i = 0; i < RESOLVE_MODE_MAX; i++) {
			resolve.pipelines[i] = RD::get_singleton()->compute_pipeline_create(resolve.shader.version_get_shader(resolve.shader_version, i));
		}
	}
}

Resolve::~Resolve() {
	resolve_raster.shader.version_free(resolve_raster.shader_version);

	if (supports_storage) {
		resolve.shader.version_free(resolve.shader_version);
	}
}

void Resolve::resolve_gi(RID p_source_depth, RID p_source_normal_roughness, RID p_source_voxel_gi, RID p_dest_depth, RID p_dest_normal_roughness, RID p_dest_voxel_gi, Vector2i p_screen_size, int p_samples) {
	ERR_FAIL_COND_MSG(!supports_storage, "Can't use the compute shader resolve with the mobile renderer.");

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	ResolvePushConstant push_constant;
	push_constant.screen_size[0] = p_screen_size.x;
	push_constant.screen_size[1] = p_screen_size.y;
	push_constant.samples = p_samples;

	// setup our uniforms
	RID default_sampler = material_storage->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	RD::Uniform u_source_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, p_source_depth }));
	RD::Uniform u_source_normal_roughness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ default_sampler, p_source_normal_roughness }));
	RD::Uniform u_dest_depth(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ p_dest_depth }));
	RD::Uniform u_dest_normal_roughness(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ p_dest_normal_roughness }));

	ResolveMode mode = p_source_voxel_gi.is_valid() ? RESOLVE_MODE_GI_VOXEL_GI : RESOLVE_MODE_GI;
	RID shader = resolve.shader.version_get_shader(resolve.shader_version, mode);
	ERR_FAIL_COND(shader.is_null());

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, resolve.pipelines[mode]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_source_depth, u_source_normal_roughness), 0);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_dest_depth, u_dest_normal_roughness), 1);
	if (p_source_voxel_gi.is_valid()) {
		RD::Uniform u_source_voxel_gi(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, p_source_voxel_gi }));
		RD::Uniform u_dest_voxel_gi(RD::UNIFORM_TYPE_IMAGE, 0, p_dest_voxel_gi);

		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 2, u_source_voxel_gi), 2);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 3, u_dest_voxel_gi), 3);
	}

	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(ResolvePushConstant));

	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_screen_size.x, p_screen_size.y, 1);

	RD::get_singleton()->compute_list_end();
}

void Resolve::resolve_depth(RID p_source_depth, RID p_dest_depth, Vector2i p_screen_size, int p_samples) {
	ERR_FAIL_COND_MSG(!supports_storage, "Can't use the compute shader resolve with the mobile renderer.");

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	ResolvePushConstant push_constant;
	push_constant.screen_size[0] = p_screen_size.x;
	push_constant.screen_size[1] = p_screen_size.y;
	push_constant.samples = p_samples;

	// setup our uniforms
	RID default_sampler = material_storage->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	RD::Uniform u_source_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, p_source_depth }));
	RD::Uniform u_dest_depth(RD::UNIFORM_TYPE_IMAGE, 0, p_dest_depth);

	ResolveMode mode = RESOLVE_MODE_DEPTH;
	RID shader = resolve.shader.version_get_shader(resolve.shader_version, mode);
	ERR_FAIL_COND(shader.is_null());

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, resolve.pipelines[mode]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_source_depth), 0);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_dest_depth), 1);

	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(ResolvePushConstant));

	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_screen_size.x, p_screen_size.y, 1);

	RD::get_singleton()->compute_list_end();
}

void Resolve::resolve_depth_raster(RID p_source_rd_texture, RID p_dest_framebuffer, int p_samples, bool p_output_to_depth_buffer) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();

	memset(&resolve_raster.push_constant, 0, sizeof(ResolvePushConstant));
	resolve_raster.push_constant.samples = p_samples;

	RID default_sampler = material_storage->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	RD::Uniform u_source_rd_texture(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, p_source_rd_texture }));

	ResolveRasterMode mode = p_output_to_depth_buffer ? RESOLVE_RASTER_MODE_DEPTH_BUFFER : RESOLVE_RASTER_MODE_COLOR_BUFFER;
	RID shader = resolve_raster.shader.version_get_shader(resolve_raster.shader_version, mode);
	ERR_FAIL_COND(shader.is_null());

	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_dest_framebuffer);
	RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, resolve_raster.pipelines[mode].get_render_pipeline(RD::INVALID_ID, RD::get_singleton()->framebuffer_get_format(p_dest_framebuffer)));
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set_cache->get_cache(shader, 0, u_source_rd_texture), 0);

	RD::get_singleton()->draw_list_set_push_constant(draw_list, &resolve_raster.push_constant, sizeof(ResolvePushConstant));

	RD::get_singleton()->draw_list_draw(draw_list, false, 1u, 3u);
	RD::get_singleton()->draw_list_end();
}
