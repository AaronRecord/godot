/*************************************************************************/
/*  vrs.cpp                                                              */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2022 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2022 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#include "vrs.h"
#include "../renderer_compositor_rd.h"
#include "../storage_rd/texture_storage.h"
#include "../uniform_set_cache_rd.h"
#include "servers/xr_server.h"

using namespace RendererRD;

VRS::VRS() {
	{
		Vector<String> vrs_modes;
		vrs_modes.push_back("\n"); // VRS_DEFAULT
		vrs_modes.push_back("\n#define MULTIVIEW\n"); // VRS_MULTIVIEW

		vrs_shader.shader.initialize(vrs_modes);

		if (!RendererCompositorRD::singleton->is_xr_enabled()) {
			vrs_shader.shader.set_variant_enabled(VRS_MULTIVIEW, false);
		}

		vrs_shader.shader_version = vrs_shader.shader.version_create();

		//use additive

		for (int i = 0; i < VRS_MAX; i++) {
			if (vrs_shader.shader.is_variant_enabled(i)) {
				vrs_shader.pipelines[i].setup(vrs_shader.shader.version_get_shader(vrs_shader.shader_version, i), RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_disabled(), 0);
			} else {
				vrs_shader.pipelines[i].clear();
			}
		}
	}
}

VRS::~VRS() {
	vrs_shader.shader.version_free(vrs_shader.shader_version);
}

void VRS::copy_vrs(RID p_source_rd_texture, RID p_dest_framebuffer, bool p_multiview) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	// setup our uniforms
	RID default_sampler = material_storage->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	RD::Uniform u_source_rd_texture(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, p_source_rd_texture }));

	VRSMode mode = p_multiview ? VRS_MULTIVIEW : VRS_DEFAULT;

	RID shader = vrs_shader.shader.version_get_shader(vrs_shader.shader_version, mode);
	ERR_FAIL_COND(shader.is_null());

	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_dest_framebuffer, RD::INITIAL_ACTION_KEEP, RD::FINAL_ACTION_READ, RD::INITIAL_ACTION_KEEP, RD::FINAL_ACTION_DISCARD, Vector<Color>());
	RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, vrs_shader.pipelines[mode].get_render_pipeline(RD::INVALID_ID, RD::get_singleton()->framebuffer_get_format(p_dest_framebuffer)));
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set_cache->get_cache(shader, 0, u_source_rd_texture), 0);
	RD::get_singleton()->draw_list_bind_index_array(draw_list, material_storage->get_quad_index_array());
	// RD::get_singleton()->draw_list_set_push_constant(draw_list, &vrs_shader.push_constant, sizeof(VRSPushConstant));
	RD::get_singleton()->draw_list_draw(draw_list, true);
	RD::get_singleton()->draw_list_end();
}

void VRS::create_vrs_texture(const int p_base_width, const int p_base_height, const uint32_t p_view_count, RID &p_vrs_texture, RID &p_vrs_fb) {
	// TODO find a way to skip this if VRS is not supported, but we don't have access to VulkanContext here, even though we're in vulkan.. hmmm

	// TODO we should find some way to store this properly, we're assuming 16x16 as this seems to be the standard but in our vrs_capacities we
	// obtain a minimum and maximum size, and we should choose something within this range and then make sure that is consistantly set when creating
	// our frame buffer. Also it is important that we make the resulting size we calculate down below available to the end user so they know the size
	// of the VRS buffer to supply.
	Size2i texel_size = Size2i(16, 16);

	RD::TextureFormat tf;
	if (p_view_count > 1) {
		tf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	} else {
		tf.texture_type = RD::TEXTURE_TYPE_2D;
	}
	tf.format = RD::DATA_FORMAT_R8_UINT;
	tf.width = p_base_width / texel_size.x;
	if (p_base_width % texel_size.x != 0) {
		tf.width++;
	}
	tf.height = p_base_height / texel_size.y;
	if (p_base_height % texel_size.y != 0) {
		tf.height++;
	}
	tf.array_layers = p_view_count; // create a layer for every view
	tf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_VRS_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	tf.samples = RD::TEXTURE_SAMPLES_1;

	p_vrs_texture = RD::get_singleton()->texture_create(tf, RD::TextureView());

	// by default VRS is assumed to be our VRS attachment, but if we need to write into it, we need a bit more control
	Vector<RID> fb;
	fb.push_back(p_vrs_texture);

	RD::FramebufferPass pass;
	pass.color_attachments.push_back(0);

	Vector<RD::FramebufferPass> passes;
	passes.push_back(pass);

	p_vrs_fb = RD::get_singleton()->framebuffer_create_multipass(fb, passes, RenderingDevice::INVALID_ID, p_view_count);
}

void VRS::update_vrs_texture(RID p_vrs_fb, RID p_render_target) {
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	RS::ViewportVRSMode vrs_mode = texture_storage->render_target_get_vrs_mode(p_render_target);

	if (vrs_mode != RS::VIEWPORT_VRS_DISABLED) {
		RD::get_singleton()->draw_command_begin_label("VRS Setup");

		// TODO figure out if image has changed since it was last copied so we can save some resources..

		if (vrs_mode == RS::VIEWPORT_VRS_TEXTURE) {
			RID vrs_texture = texture_storage->render_target_get_vrs_texture(p_render_target);
			if (vrs_texture.is_valid()) {
				Texture *texture = texture_storage->get_texture(vrs_texture);
				if (texture) {
					// Copy into our density buffer
					copy_vrs(texture->rd_texture, p_vrs_fb, texture->layers > 1);
				}
			}
		} else if (vrs_mode == RS::VIEWPORT_VRS_XR) {
			Ref<XRInterface> interface = XRServer::get_singleton()->get_primary_interface();
			if (interface.is_valid()) {
				RID vrs_texture = interface->get_vrs_texture();
				if (vrs_texture.is_valid()) {
					Texture *texture = texture_storage->get_texture(vrs_texture);
					if (texture) {
						// Copy into our density buffer
						copy_vrs(texture->rd_texture, p_vrs_fb, texture->layers > 1);
					}
				}
			}
		}

		RD::get_singleton()->draw_command_end_label();
	}
}
