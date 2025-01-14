/* Copyright (c) 2019, Arm Limited and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 the "License";
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "command_buffer.h"

#include "command_pool.h"
#include "common/error.h"
#include "device.h"

namespace vkb
{
CommandBuffer::CommandBuffer(CommandPool &command_pool, VkCommandBufferLevel level) :
    command_pool{command_pool},
    level{level}
{
	VkCommandBufferAllocateInfo allocate_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};

	allocate_info.commandPool        = command_pool.get_handle();
	allocate_info.commandBufferCount = 1;
	allocate_info.level              = level;

	VkResult result = vkAllocateCommandBuffers(command_pool.get_device().get_handle(), &allocate_info, &handle);

	if (result != VK_SUCCESS)
	{
		throw VulkanException{result, "Failed to allocate command buffer"};
	}
}

CommandBuffer::~CommandBuffer()
{
	// Destroy command buffer
	if (handle != VK_NULL_HANDLE)
	{
		vkFreeCommandBuffers(command_pool.get_device().get_handle(), command_pool.get_handle(), 1, &handle);
	}
}

CommandBuffer::CommandBuffer(CommandBuffer &&other) :
    command_pool{other.command_pool},
    level{other.level},
    handle{other.handle},
    state{other.state}
{
	other.handle = VK_NULL_HANDLE;
	other.state  = State::Invalid;
}

Device &CommandBuffer::get_device()
{
	return command_pool.get_device();
}

const VkCommandBuffer &CommandBuffer::get_handle() const
{
	return handle;
}

bool CommandBuffer::is_recording() const
{
	return state == State::Recording;
}

VkResult CommandBuffer::begin(VkCommandBufferUsageFlags flags, CommandBuffer *primary_cmd_buf)
{
	assert(!is_recording() && "Command buffer is already recording, please call end before beginning again");

	if (is_recording())
	{
		return VK_NOT_READY;
	}

	state = State::Recording;

	// Reset state
	pipeline_state.reset();
	resource_binding_state.reset();
	descriptor_set_layout_state.clear();

	VkCommandBufferBeginInfo       begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	VkCommandBufferInheritanceInfo inheritance = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
	begin_info.flags                           = flags;

	if (level == VK_COMMAND_BUFFER_LEVEL_SECONDARY)
	{
		assert(primary_cmd_buf && "A primary command buffer pointer must be provided when calling begin from a secondary one");

		auto render_pass_binding        = primary_cmd_buf->get_current_render_pass();
		current_render_pass.render_pass = render_pass_binding.render_pass;
		current_render_pass.framebuffer = render_pass_binding.framebuffer;

		inheritance.renderPass  = current_render_pass.render_pass->get_handle();
		inheritance.framebuffer = current_render_pass.framebuffer->get_handle();
		inheritance.subpass     = pipeline_state.get_subpass_index();

		begin_info.pInheritanceInfo = &inheritance;
	}

	vkBeginCommandBuffer(handle, &begin_info);

	return VK_SUCCESS;
}

VkResult CommandBuffer::end()
{
	assert(is_recording() && "Command buffer is not recording, please call begin before end");

	if (!is_recording())
	{
		return VK_NOT_READY;
	}

	vkEndCommandBuffer(get_handle());

	state = State::Executable;

	return VK_SUCCESS;
}

void CommandBuffer::begin_render_pass(const RenderTarget &render_target, const std::vector<LoadStoreInfo> &load_store_infos, const std::vector<VkClearValue> &clear_values, VkSubpassContents contents, const std::vector<std::unique_ptr<Subpass>> &subpasses)
{
	// Reset state
	pipeline_state.reset();
	resource_binding_state.reset();
	descriptor_set_layout_state.clear();

	// Create render pass
	std::vector<SubpassInfo> subpass_infos(subpasses.size());
	auto                     subpass_info_it = subpass_infos.begin();
	for (auto &subpass : subpasses)
	{
		subpass_info_it->input_attachments  = subpass->get_input_attachments();
		subpass_info_it->output_attachments = subpass->get_output_attachments();

		++subpass_info_it;
	}
	current_render_pass.render_pass = &get_device().get_resource_cache().request_render_pass(render_target.get_attachments(), load_store_infos, subpass_infos);
	current_render_pass.framebuffer = &get_device().get_resource_cache().request_framebuffer(render_target, *current_render_pass.render_pass);

	// Begin render pass
	VkRenderPassBeginInfo begin_info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
	begin_info.renderPass        = current_render_pass.render_pass->get_handle();
	begin_info.framebuffer       = current_render_pass.framebuffer->get_handle();
	begin_info.renderArea.extent = render_target.get_extent();
	begin_info.clearValueCount   = to_u32(clear_values.size());
	begin_info.pClearValues      = clear_values.data();

	vkCmdBeginRenderPass(get_handle(), &begin_info, contents);

	// Update blend state attachments for first subpass
	auto blend_state = pipeline_state.get_color_blend_state();
	blend_state.attachments.resize(current_render_pass.render_pass->get_color_output_count(pipeline_state.get_subpass_index()));
	pipeline_state.set_color_blend_state(blend_state);
}

void CommandBuffer::next_subpass()
{
	// Increment subpass index
	pipeline_state.set_subpass_index(pipeline_state.get_subpass_index() + 1);

	// Update blend state attachments
	auto blend_state = pipeline_state.get_color_blend_state();
	blend_state.attachments.resize(current_render_pass.render_pass->get_color_output_count(pipeline_state.get_subpass_index()));
	pipeline_state.set_color_blend_state(blend_state);

	// Reset descriptor sets
	resource_binding_state.reset();
	descriptor_set_layout_state.clear();

	vkCmdNextSubpass(get_handle(), VK_SUBPASS_CONTENTS_INLINE);
}

void CommandBuffer::execute_commands(std::vector<CommandBuffer *> &secondary_command_buffers)
{
	std::vector<VkCommandBuffer> sec_cmd_buf_handles(secondary_command_buffers.size(), VK_NULL_HANDLE);
	std::transform(secondary_command_buffers.begin(), secondary_command_buffers.end(), sec_cmd_buf_handles.begin(),
	               [](const vkb::CommandBuffer *sec_cmd_buf) { return sec_cmd_buf->get_handle(); });
	vkCmdExecuteCommands(get_handle(), to_u32(sec_cmd_buf_handles.size()), sec_cmd_buf_handles.data());
}

void CommandBuffer::end_render_pass()
{
	vkCmdEndRenderPass(get_handle());
}

void CommandBuffer::bind_pipeline_layout(PipelineLayout &pipeline_layout)
{
	pipeline_state.set_pipeline_layout(pipeline_layout);
}

void CommandBuffer::set_specialization_constant(uint32_t constant_id, const std::vector<uint8_t> &data)
{
	pipeline_state.set_specialization_constant(constant_id, data);
}

void CommandBuffer::push_constants(uint32_t offset, const std::vector<uint8_t> &values)
{
	const PipelineLayout &pipeline_layout = pipeline_state.get_pipeline_layout();

	VkShaderStageFlags shader_stage = pipeline_layout.get_push_constant_range_stage(offset, to_u32(values.size()));

	if (shader_stage)
	{
		vkCmdPushConstants(get_handle(), pipeline_layout.get_handle(), shader_stage, offset, to_u32(values.size()), values.data());
	}
	else
	{
		LOGW("Push constant range [{}, {}] not found", offset, values.size());
	}
}

void CommandBuffer::bind_buffer(const core::Buffer &buffer, VkDeviceSize offset, VkDeviceSize range, uint32_t set, uint32_t binding, uint32_t array_element)
{
	resource_binding_state.bind_buffer(buffer, offset, range, set, binding, array_element);
}

void CommandBuffer::bind_image(const core::ImageView &image_view, const core::Sampler &sampler, uint32_t set, uint32_t binding, uint32_t array_element)
{
	resource_binding_state.bind_image(image_view, sampler, set, binding, array_element);
}

void CommandBuffer::bind_input(const core::ImageView &image_view, uint32_t set, uint32_t binding, uint32_t array_element)
{
	resource_binding_state.bind_input(image_view, set, binding, array_element);
}

void CommandBuffer::bind_vertex_buffers(uint32_t first_binding, const std::vector<std::reference_wrapper<const vkb::core::Buffer>> &buffers, const std::vector<VkDeviceSize> &offsets)
{
	std::vector<VkBuffer> buffer_handles(buffers.size(), VK_NULL_HANDLE);
	std::transform(buffers.begin(), buffers.end(), buffer_handles.begin(),
	               [](const core::Buffer &buffer) { return buffer.get_handle(); });
	vkCmdBindVertexBuffers(get_handle(), first_binding, to_u32(buffer_handles.size()), buffer_handles.data(), offsets.data());
}

void CommandBuffer::bind_index_buffer(const core::Buffer &buffer, VkDeviceSize offset, VkIndexType index_type)
{
	vkCmdBindIndexBuffer(get_handle(), buffer.get_handle(), offset, index_type);
}

void CommandBuffer::set_viewport_state(const ViewportState &state_info)
{
	pipeline_state.set_viewport_state(state_info);
}

void CommandBuffer::set_vertex_input_state(const VertexInputState &state_info)
{
	pipeline_state.set_vertex_input_state(state_info);
}

void CommandBuffer::set_input_assembly_state(const InputAssemblyState &state_info)
{
	pipeline_state.set_input_assembly_state(state_info);
}

void CommandBuffer::set_rasterization_state(const RasterizationState &state_info)
{
	pipeline_state.set_rasterization_state(state_info);
}

void CommandBuffer::set_multisample_state(const MultisampleState &state_info)
{
	pipeline_state.set_multisample_state(state_info);
}

void CommandBuffer::set_depth_stencil_state(const DepthStencilState &state_info)
{
	pipeline_state.set_depth_stencil_state(state_info);
}

void CommandBuffer::set_color_blend_state(const ColorBlendState &state_info)
{
	pipeline_state.set_color_blend_state(state_info);
}

void CommandBuffer::set_viewport(uint32_t first_viewport, const std::vector<VkViewport> &viewports)
{
	vkCmdSetViewport(get_handle(), first_viewport, to_u32(viewports.size()), viewports.data());
}

void CommandBuffer::set_scissor(uint32_t first_scissor, const std::vector<VkRect2D> &scissors)
{
	vkCmdSetScissor(get_handle(), first_scissor, to_u32(scissors.size()), scissors.data());
}

void CommandBuffer::set_line_width(float line_width)
{
	vkCmdSetLineWidth(get_handle(), line_width);
}

void CommandBuffer::set_depth_bias(float depth_bias_constant_factor, float depth_bias_clamp, float depth_bias_slope_factor)
{
	vkCmdSetDepthBias(get_handle(), depth_bias_constant_factor, depth_bias_clamp, depth_bias_slope_factor);
}

void CommandBuffer::set_blend_constants(const std::array<float, 4> &blend_constants)
{
	vkCmdSetBlendConstants(get_handle(), blend_constants.data());
}

void CommandBuffer::set_depth_bounds(float min_depth_bounds, float max_depth_bounds)
{
	vkCmdSetDepthBounds(get_handle(), min_depth_bounds, max_depth_bounds);
}

void CommandBuffer::draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance)
{
	flush_pipeline_state(VK_PIPELINE_BIND_POINT_GRAPHICS);

	flush_descriptor_state(VK_PIPELINE_BIND_POINT_GRAPHICS);

	vkCmdDraw(get_handle(), vertex_count, instance_count, first_vertex, first_instance);
}

void CommandBuffer::draw_indexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
{
	flush_pipeline_state(VK_PIPELINE_BIND_POINT_GRAPHICS);

	flush_descriptor_state(VK_PIPELINE_BIND_POINT_GRAPHICS);

	vkCmdDrawIndexed(get_handle(), index_count, instance_count, first_index, vertex_offset, first_instance);
}

void CommandBuffer::draw_indexed_indirect(const core::Buffer &buffer, VkDeviceSize offset, uint32_t draw_count, uint32_t stride)
{
	flush_pipeline_state(VK_PIPELINE_BIND_POINT_GRAPHICS);

	flush_descriptor_state(VK_PIPELINE_BIND_POINT_GRAPHICS);

	vkCmdDrawIndexedIndirect(get_handle(), buffer.get_handle(), offset, draw_count, stride);
}

void CommandBuffer::dispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)
{
	flush_pipeline_state(VK_PIPELINE_BIND_POINT_COMPUTE);

	flush_descriptor_state(VK_PIPELINE_BIND_POINT_COMPUTE);

	vkCmdDispatch(get_handle(), group_count_x, group_count_y, group_count_z);
}

void CommandBuffer::dispatch_indirect(const core::Buffer &buffer, VkDeviceSize offset)
{
	flush_pipeline_state(VK_PIPELINE_BIND_POINT_COMPUTE);

	flush_descriptor_state(VK_PIPELINE_BIND_POINT_COMPUTE);

	vkCmdDispatchIndirect(get_handle(), buffer.get_handle(), offset);
}

void CommandBuffer::update_buffer(const core::Buffer &buffer, VkDeviceSize offset, const std::vector<uint8_t> &data)
{
	vkCmdUpdateBuffer(get_handle(), buffer.get_handle(), offset, data.size(), data.data());
}

void CommandBuffer::blit_image(const core::Image &src_img, const core::Image &dst_img, const std::vector<VkImageBlit> &regions)
{
	vkCmdBlitImage(get_handle(), src_img.get_handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	               dst_img.get_handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	               to_u32(regions.size()), regions.data(), VK_FILTER_NEAREST);
}

void CommandBuffer::copy_buffer(const core::Buffer &src_buffer, const core::Buffer &dst_buffer, VkDeviceSize size)
{
	VkBufferCopy copyRegion = {};
	copyRegion.size         = size;
	vkCmdCopyBuffer(get_handle(), src_buffer.get_handle(), dst_buffer.get_handle(), 1, &copyRegion);
}

void CommandBuffer::copy_image(const core::Image &src_img, const core::Image &dst_img, const std::vector<VkImageCopy> &regions)
{
	vkCmdCopyImage(get_handle(), src_img.get_handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	               dst_img.get_handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	               to_u32(regions.size()), regions.data());
}

void CommandBuffer::copy_buffer_to_image(const core::Buffer &buffer, const core::Image &image, const std::vector<VkBufferImageCopy> &regions)
{
	vkCmdCopyBufferToImage(get_handle(), buffer.get_handle(),
	                       image.get_handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                       to_u32(regions.size()), regions.data());
}

void CommandBuffer::image_memory_barrier(const core::ImageView &image_view, const ImageMemoryBarrier &memory_barrier)
{
	VkImageMemoryBarrier image_memory_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	image_memory_barrier.oldLayout        = memory_barrier.old_layout;
	image_memory_barrier.newLayout        = memory_barrier.new_layout;
	image_memory_barrier.image            = image_view.get_image().get_handle();
	image_memory_barrier.subresourceRange = image_view.get_subresource_range();
	image_memory_barrier.srcAccessMask    = memory_barrier.src_access_mask;
	image_memory_barrier.dstAccessMask    = memory_barrier.dst_access_mask;

	VkPipelineStageFlags src_stage_mask = memory_barrier.src_stage_mask;
	VkPipelineStageFlags dst_stage_mask = memory_barrier.dst_stage_mask;

	vkCmdPipelineBarrier(
	    get_handle(),
	    src_stage_mask,
	    dst_stage_mask,
	    0,
	    0, nullptr,
	    0, nullptr,
	    1,
	    &image_memory_barrier);
}

void CommandBuffer::buffer_memory_barrier(const core::Buffer &buffer, VkDeviceSize offset, VkDeviceSize size, const BufferMemoryBarrier &memory_barrier)
{
	VkBufferMemoryBarrier buffer_memory_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
	buffer_memory_barrier.srcAccessMask = memory_barrier.src_access_mask;
	buffer_memory_barrier.dstAccessMask = memory_barrier.dst_access_mask;
	buffer_memory_barrier.buffer        = buffer.get_handle();
	buffer_memory_barrier.offset        = offset;
	buffer_memory_barrier.size          = size;

	VkPipelineStageFlags src_stage_mask = memory_barrier.src_stage_mask;
	VkPipelineStageFlags dst_stage_mask = memory_barrier.dst_stage_mask;

	vkCmdPipelineBarrier(
	    get_handle(),
	    src_stage_mask,
	    dst_stage_mask,
	    0,
	    0, nullptr,
	    1, &buffer_memory_barrier,
	    0, nullptr);
}

void CommandBuffer::flush_pipeline_state(VkPipelineBindPoint pipeline_bind_point)
{
	// Create a new pipeline only if the graphics state changed
	if (!pipeline_state.is_dirty())
	{
		return;
	}

	pipeline_state.clear_dirty();

	// Create and bind pipeline
	if (pipeline_bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS)
	{
		pipeline_state.set_render_pass(*current_render_pass.render_pass);
		auto &pipeline = get_device().get_resource_cache().request_graphics_pipeline(pipeline_state);

		vkCmdBindPipeline(get_handle(),
		                  pipeline_bind_point,
		                  pipeline.get_handle());
	}
	else if (pipeline_bind_point == VK_PIPELINE_BIND_POINT_COMPUTE)
	{
		auto &pipeline = get_device().get_resource_cache().request_compute_pipeline(pipeline_state);

		vkCmdBindPipeline(get_handle(),
		                  pipeline_bind_point,
		                  pipeline.get_handle());
	}
	else
	{
		throw "Only graphics and compute pipeline bind points are supported now";
	}
}

void CommandBuffer::flush_descriptor_state(VkPipelineBindPoint pipeline_bind_point)
{
	PipelineLayout &pipeline_layout = const_cast<PipelineLayout &>(pipeline_state.get_pipeline_layout());

	const auto &set_bindings = pipeline_layout.get_bindings();

	std::unordered_set<uint32_t> update_sets;

	// Iterate over pipeline layout sets
	for (auto &set_it : set_bindings)
	{
		auto descriptor_set_layout_it = descriptor_set_layout_state.find(set_it.first);

		// Check if set was bound before
		if (descriptor_set_layout_it != descriptor_set_layout_state.end())
		{
			// Add set to later update it if is different from the current pipeline layout's set
			if (descriptor_set_layout_it->second->get_handle() != pipeline_layout.get_set_layout(set_it.first).get_handle())
			{
				update_sets.emplace(set_it.first);
			}
		}
	}

	// Remove bound descriptor set layouts which don't exists in the pipeline layout
	for (auto set_it = descriptor_set_layout_state.begin();
	     set_it != descriptor_set_layout_state.end();)
	{
		if (!pipeline_layout.has_set_layout(set_it->first))
		{
			set_it = descriptor_set_layout_state.erase(set_it);
		}
		else
		{
			++set_it;
		}
	}

	// Check if descriptor set needs to be created
	if (resource_binding_state.is_dirty() || !update_sets.empty())
	{
		// Clear dirty bit flag
		resource_binding_state.clear_dirty();

		// Iterate over all set bindings
		for (auto &set_it : resource_binding_state.get_set_bindings())
		{
			// Skip if set bindings don't have changes
			if (!set_it.second.is_dirty() && (update_sets.find(set_it.first) == update_sets.end()))
			{
				continue;
			}

			// Clear dirty flag for binding set
			resource_binding_state.clear_dirty(set_it.first);

			// Skip set layout if it doesn't exists
			if (!pipeline_layout.has_set_layout(set_it.first))
			{
				continue;
			}

			DescriptorSetLayout &descriptor_set_layout = pipeline_layout.get_set_layout(set_it.first);

			// Make descriptor set layout bound for current set
			descriptor_set_layout_state[set_it.first] = &descriptor_set_layout;

			BindingMap<VkDescriptorBufferInfo> buffer_infos;
			BindingMap<VkDescriptorImageInfo>  image_infos;

			std::vector<uint32_t> dynamic_offsets;

			// Iterate over all resource bindings
			for (auto &binding_it : set_it.second.get_resource_bindings())
			{
				auto  binding_index     = binding_it.first;
				auto &binding_resources = binding_it.second;

				VkDescriptorSetLayoutBinding binding_info;

				// Check if binding exists in the pipeline layout
				if (!descriptor_set_layout.get_layout_binding(binding_index, binding_info))
				{
					continue;
				}

				// Iterate over all binding resources
				for (auto &element_it : binding_resources)
				{
					auto  arrayElement  = element_it.first;
					auto &resource_info = element_it.second;

					// Pointer references
					auto &buffer     = resource_info.buffer;
					auto &sampler    = resource_info.sampler;
					auto &image_view = resource_info.image_view;

					// Get buffer info
					if (buffer != nullptr && is_buffer_descriptor_type(binding_info.descriptorType))
					{
						VkDescriptorBufferInfo buffer_info{};

						buffer_info.buffer = resource_info.buffer->get_handle();
						buffer_info.offset = resource_info.offset;
						buffer_info.range  = resource_info.range;

						if (is_dynamic_buffer_descriptor_type(binding_info.descriptorType))
						{
							dynamic_offsets.push_back(to_u32(buffer_info.offset));

							buffer_info.offset = 0;
						}

						buffer_infos[binding_index][arrayElement] = buffer_info;
					}

					// Get image info
					else if (image_view != nullptr || sampler != VK_NULL_HANDLE)
					{
						// Can be null for input attachments
						VkDescriptorImageInfo image_info{};
						image_info.sampler   = sampler ? sampler->get_handle() : VK_NULL_HANDLE;
						image_info.imageView = image_view->get_handle();

						if (image_view != nullptr)
						{
							const auto &image_view = *resource_info.image_view;

							// Add image layout info based on descriptor type
							switch (binding_info.descriptorType)
							{
								case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
								case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
									if (is_depth_stencil_format(image_view.get_format()))
									{
										image_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
									}
									else
									{
										image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
									}
									break;
								case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
									image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
									break;

								default:
									continue;
							}
						}

						image_infos[binding_index][arrayElement] = std::move(image_info);
					}
				}
			}

			auto &descriptor_set = get_device().get_resource_cache().request_descriptor_set(descriptor_set_layout, buffer_infos, image_infos);

			VkDescriptorSet descriptor_set_handle = descriptor_set.get_handle();

			// Bind descriptor set
			vkCmdBindDescriptorSets(get_handle(),
			                        pipeline_bind_point,
			                        pipeline_layout.get_handle(),
			                        set_it.first,
			                        1, &descriptor_set_handle,
			                        to_u32(dynamic_offsets.size()),
			                        dynamic_offsets.data());
		}
	}
}

const CommandBuffer::State CommandBuffer::get_state() const
{
	return state;
}

VkResult CommandBuffer::reset(ResetMode reset_mode)
{
	VkResult result = VK_SUCCESS;

	assert(reset_mode == command_pool.get_reset_mode() && "Command buffer reset mode must match the one used by the pool to allocate it");

	state = State::Initial;

	if (reset_mode == ResetMode::ResetIndividually)
	{
		result = vkResetCommandBuffer(handle, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
	}

	return result;
}
}        // namespace vkb
