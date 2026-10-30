/*
 * Copyright (C) 2026 Ultranix
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"
#include "cogl/driver/vk/cogl-pipeline-vk.c"

static VkResult allocation_result;
static VkDeviceSize allocation_size;
static uintptr_t next_handle = 10;
static uintptr_t next_pool_handle = 1;
static uint64_t completed_point;
static unsigned int pool_resets;
static VkResult reset_result;
static unsigned int pool_used[4096];
static unsigned int set_pool[1000000];
static unsigned int pool_capacity = 4;
static unsigned int freed_sets;

static VkResult VKAPI_CALL
get_counter (VkDevice    device,
             VkSemaphore semaphore,
             uint64_t   *value)
{
  *value = completed_point;
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkResetDescriptorPool (VkDevice                   device,
                       VkDescriptorPool           pool,
                       VkDescriptorPoolResetFlags flags)
{
  pool_resets++;
  if (reset_result == VK_SUCCESS) pool_used[(uintptr_t) pool] = 0;
  return reset_result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorPool (VkDevice                          device,
                        const VkDescriptorPoolCreateInfo *info,
                        const VkAllocationCallbacks      *allocator,
                        VkDescriptorPool                 *pool)
{
  g_assert_cmpuint (next_pool_handle, <, G_N_ELEMENTS (pool_used));
  *pool = (VkDescriptorPool) next_pool_handle++;
  pool_used[(uintptr_t) *pool] = 0;
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateDescriptorSets (VkDevice                           device,
                          const VkDescriptorSetAllocateInfo *info,
                          VkDescriptorSet                   *sets)
{
  if (pool_used[(uintptr_t) info->descriptorPool] >= pool_capacity)
    return VK_ERROR_OUT_OF_POOL_MEMORY;
  pool_used[(uintptr_t) info->descriptorPool]++;
  *sets = (VkDescriptorSet) next_handle++;
  set_pool[(uintptr_t) *sets] = (uintptr_t) info->descriptorPool;
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkFreeDescriptorSets (VkDevice               device,
                      VkDescriptorPool       pool,
                      uint32_t               count,
                      const VkDescriptorSet *sets)
{
  for (uint32_t i = 0; i < count; i++)
    {
      g_assert_cmpuint (set_pool[(uintptr_t) sets[i]], ==, (uintptr_t) pool);
      g_assert_cmpuint (pool_used[(uintptr_t) pool], >, 0);
      pool_used[(uintptr_t) pool]--;
      set_pool[(uintptr_t) sets[i]] = 0;
      freed_sets++;
    }
  return VK_SUCCESS;
}

void cogl_vk_debug_log (const char *channel,
                        const char *format,
                        ...) {
}
void cogl_vk_device_note_vk_result (CoglVkDevice *device,
                                    VkResult      result) {
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateBuffer (VkDevice                     device,
                const VkBufferCreateInfo    *info,
                const VkAllocationCallbacks *allocator,
                VkBuffer                    *buffer)
{
  if (allocation_result != VK_SUCCESS)
    return allocation_result;
  allocation_size = info->size;
  *buffer = (VkBuffer) next_handle++;
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkGetBufferMemoryRequirements (VkDevice              device,
                               VkBuffer              buffer,
                               VkMemoryRequirements *requirements)
{
  *requirements = (VkMemoryRequirements) {
    .size = allocation_size, .alignment = 256, .memoryTypeBits = 1,
  };
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties (VkPhysicalDevice                  device,
                                     VkPhysicalDeviceMemoryProperties *properties)
{
  *properties = (VkPhysicalDeviceMemoryProperties) { .memoryTypeCount = 1 };
  properties->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateMemory (VkDevice                     device,
                  const VkMemoryAllocateInfo  *info,
                  const VkAllocationCallbacks *allocator,
                  VkDeviceMemory              *memory)
{
  *memory = (VkDeviceMemory) (uintptr_t) g_malloc0 (info->allocationSize);
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkBindBufferMemory (VkDevice       device,
                    VkBuffer       buffer,
                    VkDeviceMemory memory,
                    VkDeviceSize   offset)
{
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkMapMemory (VkDevice         device,
             VkDeviceMemory   memory,
             VkDeviceSize     offset,
             VkDeviceSize     size,
             VkMemoryMapFlags flags,
             void           **data)
{
  *data = (void *) (uintptr_t) memory;
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkUnmapMemory (VkDevice       device,
               VkDeviceMemory memory) {
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyBuffer (VkDevice                     device,
                 VkBuffer                     buffer,
                 const VkAllocationCallbacks *allocator) {
}

VKAPI_ATTR void VKAPI_CALL
vkFreeMemory (VkDevice                     device,
              VkDeviceMemory               memory,
              const VkAllocationCallbacks *allocator)
{
  g_free ((void *) (uintptr_t) memory);
}

static void
test_uniform_rotation (void)
{
  CoglVkFenceTicket ticket = { 0 };
  CoglVkPass pass = { .fence = &ticket };
  CoglDriverVk driver = { .current_pass = &pass };
  CoglVkDevice device = { .owner = &driver };
  CoglVkPipelineCache cache = {
    .device = &device, .uniform_size = 2 * COGL_VK_UBO_SLICE,
    .uniform_packed = COGL_VK_UBO_SLICE, .uniform_buffer = (VkBuffer) 1,
  };
  g_autoptr (GError) error = NULL;
  uint8_t *old;

  cache.uniform_mapped = g_malloc0 (cache.uniform_size);
  cache.uniform_memory = (VkDeviceMemory) (uintptr_t) cache.uniform_mapped;
  old = cache.uniform_mapped;
  memset (old, 0x31, COGL_VK_UBO_SLICE);
  allocation_result = VK_SUCCESS;
  g_assert_true (cogl_vk_pipeline_snapshot_uniforms (&cache, &error));
  memset (old, 0x72, COGL_VK_UBO_SLICE);

  allocation_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
  g_assert_false (cogl_vk_pipeline_snapshot_uniforms (&cache, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
  g_assert_true (cache.uniform_mapped == old);
  g_assert_null (ticket.transient_buffers);

  allocation_result = VK_SUCCESS;
  g_assert_true (cogl_vk_pipeline_snapshot_uniforms (&cache, &error));
  g_assert_true (cache.uniform_mapped != old);
  g_assert_cmpuint (ticket.transient_buffers->len, ==, 1);
  for (size_t i = 0; i < COGL_VK_UBO_SLICE; i++)
    {
      g_assert_cmpuint (old[COGL_VK_UBO_SLICE + i], ==, 0x31);
      g_assert_cmpuint (((uint8_t *) cache.uniform_mapped)[cache.uniform_dyn_offset + i],
                        ==, 0x72);
    }
  cache.uniform_packed = COGL_VK_UBO_SLICE + 1;
  g_assert_false (cogl_vk_pipeline_snapshot_uniforms (&cache, &error));
  g_assert_nonnull (error);
  g_free (old);
  g_free (cache.uniform_mapped);
  g_array_unref (ticket.transient_buffers);
}

static void
test_descriptor_retirement (void)
{
  CoglVkPass pass = { 0 };
  CoglDriverVk driver = { .current_pass = &pass };
  CoglVkDevice device = { .owner = &driver, .vkGetSemaphoreCounterValue = get_counter };
  CoglVkPipelineCache cache = { .device = &device };
  VkDescriptorSet set;

  freed_sets = 0;
  completed_point = 0;
  g_assert_true (cogl_vk_pipeline_alloc_texture_ds (&cache, FALSE, &set, NULL));
  cogl_vk_pipeline_cache_note_submit (&cache, (VkSemaphore) 1, 1);
  cogl_vk_pipeline_free_texture_ds (&cache, FALSE, set);
  cogl_vk_pipeline_cache_reclaim_pools (&cache);
  g_assert_cmpuint (freed_sets, ==, 0);
  cogl_vk_pipeline_cache_note_submit (&cache, (VkSemaphore) 1, 2);
  completed_point = 1;
  cogl_vk_pipeline_cache_reclaim_pools (&cache);
  g_assert_cmpuint (freed_sets, ==, 0);
  /* Later frames must not postpone retirement of this released descriptor. */
  cogl_vk_pipeline_cache_note_submit (&cache, (VkSemaphore) 1, 3);
  completed_point = 2;
  cogl_vk_pipeline_cache_reclaim_pools (&cache);
  g_assert_cmpuint (freed_sets, ==, 1);
  g_assert_cmpuint (cache.ds_retired->len, ==, 0);
  g_hash_table_unref (cache.ds_texture_owners);
  g_array_unref (cache.ds_retired);
  g_ptr_array_unref (cache.ds_pool_slots);
}

static void
test_descriptor_no_early_reuse (void)
{
  CoglVkDevice device = { .vkGetSemaphoreCounterValue = get_counter };
  CoglVkPipelineCache cache = { .device = &device };
  VkDescriptorSet a, b;

  completed_point = 0;
  g_assert_true (cogl_vk_pipeline_alloc_texture_ds (&cache, FALSE, &a, NULL));
  cogl_vk_pipeline_cache_note_submit (&cache, (VkSemaphore) 1, 3);
  cogl_vk_pipeline_free_texture_ds (&cache, FALSE, a);
  g_assert_true (cogl_vk_pipeline_alloc_texture_ds (&cache, FALSE, &b, NULL));
  g_assert_true (a != b);
  g_assert_cmpuint (cache.ds_retired->len, ==, 1);
  completed_point = 3;
  cogl_vk_pipeline_cache_reclaim_pools (&cache);
  g_assert_cmpuint (cache.ds_retired->len, ==, 0);
  g_hash_table_unref (cache.ds_texture_owners);
  g_array_unref (cache.ds_retired);
  g_ptr_array_unref (cache.ds_pool_slots);
}

static void
test_descriptor_transient_retirement (void)
{
  CoglVkDevice device = { .vkGetSemaphoreCounterValue = get_counter };
  CoglVkPipelineCache cache = {
    .device = &device, .ds_layout_cold = (VkDescriptorSetLayout) 1,
  };
  CoglVkDsPoolSlot *slot;
  VkDescriptorSet set;

  completed_point = 0;
  pool_resets = 0;
  g_assert_true (cogl_vk_pipeline_cache_grow_pool (&cache, FALSE));
  g_assert_true (cogl_vk_pipeline_alloc_cold_ds (&cache, &set, NULL));
  slot = g_ptr_array_index (cache.ds_pool_slots, 0);
  slot->open = FALSE;
  cogl_vk_pipeline_cache_reclaim_pools (&cache);
  g_assert_cmpuint (pool_resets, ==, 0);
  cogl_vk_pipeline_cache_note_submit (&cache, (VkSemaphore) 1, 1);
  cogl_vk_pipeline_cache_note_submit (&cache, (VkSemaphore) 1, 2);
  g_assert_cmpuint (slot->busy_point, ==, 1);
  cogl_vk_pipeline_cache_reclaim_pools (&cache);
  g_assert_cmpuint (pool_resets, ==, 0);
  completed_point = 1;
  reset_result = VK_ERROR_UNKNOWN;
  cogl_vk_pipeline_cache_reclaim_pools (&cache);
  g_assert_false (slot->spare);
  g_assert_true (slot->timeline != VK_NULL_HANDLE);
  reset_result = VK_SUCCESS;
  cogl_vk_pipeline_cache_reclaim_pools (&cache);
  g_assert_cmpuint (pool_resets, ==, 2);
  g_assert_true (slot->spare);
  g_ptr_array_unref (cache.ds_pool_slots);
}

static void
test_descriptor_sustained (void)
{
  CoglVkPass pass = { 0 };
  CoglDriverVk driver = { .current_pass = &pass };
  CoglVkDevice device = { .owner = &driver, .vkGetSemaphoreCounterValue = get_counter };
  CoglVkPipelineCache cache = {
    .device = &device, .ds_layout_cold = (VkDescriptorSetLayout) 1,
  };
  VkDescriptorSet pinned, set;
  unsigned int peak = 0;

  completed_point = 0;
  reset_result = VK_SUCCESS;
  g_assert_true (cogl_vk_pipeline_cache_grow_pool (&cache, FALSE));
  g_assert_true (cogl_vk_pipeline_alloc_texture_ds (&cache, FALSE, &pinned, NULL));
  for (uint64_t frame = 1; frame <= 2000; frame++)
    {
      /* More than eight complete pools in a single recording; a permanent
       * texture and descriptor churn must not pin the transient pools.
       */
      for (unsigned int draw = 0; draw < 40; draw++)
        {
          g_assert_true (cogl_vk_pipeline_alloc_cold_ds (&cache, &set, NULL));
          cogl_vk_pipeline_free_cold_ds (&cache, set);
        }
      g_assert_true (cogl_vk_pipeline_alloc_texture_ds (&cache, FALSE, &set, NULL));
      cogl_vk_pipeline_free_texture_ds (&cache, FALSE, set);
      cogl_vk_pipeline_cache_note_submit (&cache, (VkSemaphore) 1, frame);
      completed_point = frame > 2 ? frame - 2 : 0;
      cogl_vk_pipeline_cache_reclaim_pools (&cache);
      if (frame == 10) peak = cache.ds_pool_slots->len;
      if (frame > 10) g_assert_cmpuint (cache.ds_pool_slots->len, ==, peak);
    }
  g_assert_cmpuint (peak, >, 8);
  g_assert_cmpuint (set_pool[(uintptr_t) pinned], >, 0);
  g_hash_table_unref (cache.ds_texture_owners);
  g_array_unref (cache.ds_retired);
  g_ptr_array_unref (cache.ds_pool_slots);
}

static void
test_uniform_repeated_declaration (void)
{
  g_autoptr (GArray) members =
    g_array_new (FALSE, TRUE, sizeof (CoglVkUboMember));
  size_t offset = 0;

  g_array_set_clear_func (members, ubo_member_clear);
  append_uniform_decls_from_text (
    "uniform vec3 shared;\n"
    "uniform float tail;\n"
    "uniform float extra;\n", NULL, members, &offset);
  /* The vertex stage repeats a fragment uniform before adding its own. */
  append_uniform_decls_from_text (
    "uniform vec3 shared;\n"
    "uniform float scale;\n", NULL, members, &offset);

  g_assert_cmpuint (members->len, ==, 4);
  g_assert_cmpuint (g_array_index (members, CoglVkUboMember, 1).offset, ==, 12);
  g_assert_cmpuint (g_array_index (members, CoglVkUboMember, 3).offset, ==, 20);
  g_assert_cmpuint (offset, ==, 24);
}

static void
test_device_layer_limits (void)
{
  VkPhysicalDeviceLimits limits = {
    .maxPerStageDescriptorSamplers = 64,
    .maxPerStageDescriptorSampledImages = 64,
    .maxDescriptorSetSamplers = 64,
    .maxDescriptorSetSampledImages = 64,
    .maxPerStageResources = 128,
    .maxVertexOutputComponents = 128,
    .maxFragmentInputComponents = 128,
    .maxUniformBufferRange = 16384,
  };
  VkPhysicalDeviceLimits changed;

  g_assert_cmpint (max_layers_for_limits (&limits), ==, 31);
  changed = limits;
  changed.maxPerStageDescriptorSamplers = 12;
  g_assert_cmpint (max_layers_for_limits (&changed), ==, 12);
  changed = limits;
  changed.maxPerStageDescriptorSampledImages = 10;
  g_assert_cmpint (max_layers_for_limits (&changed), ==, 10);
  changed = limits;
  changed.maxDescriptorSetSamplers = 9;
  g_assert_cmpint (max_layers_for_limits (&changed), ==, 9);
  changed = limits;
  changed.maxDescriptorSetSampledImages = 8;
  g_assert_cmpint (max_layers_for_limits (&changed), ==, 8);
  changed = limits;
  changed.maxPerStageResources = 16;
  g_assert_cmpint (max_layers_for_limits (&changed), ==, 15);
  changed = limits;
  changed.maxVertexOutputComponents = 64;
  g_assert_cmpint (max_layers_for_limits (&changed), ==, 15);
  changed = limits;
  changed.maxFragmentInputComponents = 64;
  g_assert_cmpint (max_layers_for_limits (&changed), ==, 15);
  changed = limits;
  changed.maxUniformBufferRange = 1024;
  g_assert_cmpint (max_layers_for_limits (&changed), ==, 15);
  changed = (VkPhysicalDeviceLimits) {0};
  g_assert_cmpint (max_layers_for_limits (&changed), ==, 0);
}

int main (int    argc,
          char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/vk-uniform/rotation-and-failure", test_uniform_rotation);
  g_test_add_func ("/vk-uniform/repeated-declaration", test_uniform_repeated_declaration);
  g_test_add_func ("/vk-descriptor/retirement", test_descriptor_retirement);
  g_test_add_func ("/vk-descriptor/no-early-reuse", test_descriptor_no_early_reuse);
  g_test_add_func ("/vk-descriptor/sustained", test_descriptor_sustained);
  g_test_add_func ("/vk-descriptor/transient-retirement", test_descriptor_transient_retirement);
  g_test_add_func ("/vk-limits/layers", test_device_layer_limits);
  return g_test_run ();
}
