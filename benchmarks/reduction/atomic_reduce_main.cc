// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <chrono>
#include <memory>
#include <numeric>

#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "benchmark/benchmark.h"
#include "uvkc/benchmark/fp16_util.h"
#include "uvkc/benchmark/main.h"
#include "uvkc/benchmark/status_util.h"
#include "uvkc/benchmark/vulkan_buffer_util.h"
#include "uvkc/benchmark/vulkan_context.h"
#include "uvkc/vulkan/device.h"
#include "uvkc/vulkan/pipeline.h"

using ::uvkc::benchmark::LatencyMeasureMode;

static const char kBenchmarkName[] = "atomic_reduce";

namespace atomic_loop_float_shader {
#include "atomic_reduce_loop_float_shader_spirv_permutation.inc"
}
namespace atomic_subgroup_float_shader {
#include "atomic_reduce_subgroup_float_shader_spirv_permutation.inc"
}
namespace atomic_loop_int_shader {
#include "atomic_reduce_loop_int_shader_spirv_permutation.inc"
}
namespace atomic_subgroup_int_shader {
#include "atomic_reduce_subgroup_int_shader_spirv_permutation.inc"
}

struct ShaderCode {
  const char *name;       // Test case name
  const uint32_t *code;   // SPIR-V code
  size_t code_num_bytes;  // Number of bytes for SPIR-V code
  size_t batch_elements;  // Number of elements in each batch
  bool is_integer;        // Whether the elements should be integers
};

#define FLOAT_SHADER_CASE(kind, size)                                        \
  {                                                                          \
#kind "/batch=" #size, atomic_##kind##_float_shader::BATCH_SIZE_##size,  \
        sizeof(atomic_##kind##_float_shader::BATCH_SIZE_##size), size, false \
  }

#define INT_SHADER_CASE(kind, size)                                       \
  {                                                                       \
#kind "/batch=" #size, atomic_##kind##_int_shader::BATCH_SIZE_##size, \
        sizeof(atomic_##kind##_int_shader::BATCH_SIZE_##size), size, true \
  }

ShaderCode kShaders[] = {
    FLOAT_SHADER_CASE(loop, 16),      FLOAT_SHADER_CASE(loop, 32),
    FLOAT_SHADER_CASE(loop, 64),      FLOAT_SHADER_CASE(loop, 128),
    FLOAT_SHADER_CASE(loop, 256),     FLOAT_SHADER_CASE(loop, 512),
    FLOAT_SHADER_CASE(subgroup, 64),  FLOAT_SHADER_CASE(subgroup, 128),
    FLOAT_SHADER_CASE(subgroup, 256), FLOAT_SHADER_CASE(subgroup, 512),

    INT_SHADER_CASE(loop, 16),        INT_SHADER_CASE(loop, 32),
    INT_SHADER_CASE(loop, 64),        INT_SHADER_CASE(loop, 128),
    INT_SHADER_CASE(loop, 256),       INT_SHADER_CASE(loop, 512),
    INT_SHADER_CASE(subgroup, 64),    INT_SHADER_CASE(subgroup, 128),
    INT_SHADER_CASE(subgroup, 256),   INT_SHADER_CASE(subgroup, 512),
};

static void Reduce(::benchmark::State &state, ::uvkc::vulkan::Device *device,
                   const ::uvkc::benchmark::LatencyMeasure *latency_measure,
                   const uint32_t *code, size_t code_num_words,
                   size_t total_elements, size_t batch_elements,
                   bool is_integer) {
  //===-------------------------------------------------------------------===/
  // Create shader module, pipeline, and descriptor sets
  //===-------------------------------------------------------------------===/

  BM_CHECK_OK_AND_ASSIGN(auto shader_module,
                         device->CreateShaderModule(code, code_num_words));
  BM_CHECK_OK_AND_ASSIGN(auto pipeline,
                         device->CreatePipeline(*shader_module, "main", {}));
  BM_CHECK_OK_AND_ASSIGN(auto descriptor_pool,
                         device->CreateDescriptorPool(*shader_module));
  BM_CHECK_OK_AND_ASSIGN(auto layout_set_map,
                         descriptor_pool->AllocateDescriptorSets(
                             shader_module->descriptor_set_layouts()));

  //===-------------------------------------------------------------------===/
  // Create buffers
  //===-------------------------------------------------------------------===/

  const size_t src_buffer_size = total_elements * sizeof(float);
  const size_t dst_buffer_size = sizeof(float);

  BM_CHECK_OK_AND_ASSIGN(
      auto src_buffer,
      device->CreateBuffer(
          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, src_buffer_size));
  BM_CHECK_OK_AND_ASSIGN(
      auto dst_buffer,
      device->CreateBuffer(
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, dst_buffer_size));

  // Create a buffer for zeroing the destination buffer.
  BM_CHECK_OK_AND_ASSIGN(
      auto data_buffer,
      device->CreateBuffer(
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, dst_buffer_size));

  //===-------------------------------------------------------------------===/
  // Set source buffer data
  //===-------------------------------------------------------------------===/

  auto generate_float_data = [](size_t i) { return float(i % 9 - 4) * 0.5f; };
  auto generate_int_data = [](size_t i) { return i % 13 - 7; };

  BM_CHECK_OK(::uvkc::benchmark::SetDeviceBufferViaStagingBuffer(
      device, src_buffer.get(), src_buffer_size,
      [&](void *ptr, size_t num_bytes) {
        if (is_integer) {
          int *src_int_buffer = reinterpret_cast<int *>(ptr);
          for (size_t i = 0; i < num_bytes / sizeof(int); i++) {
            src_int_buffer[i] = generate_int_data(i);
          }
        } else {
          float *src_float_buffer = reinterpret_cast<float *>(ptr);
          for (size_t i = 0; i < num_bytes / sizeof(float); i++) {
            src_float_buffer[i] = generate_float_data(i);
          }
        }
      }));

  BM_CHECK_OK(::uvkc::benchmark::SetDeviceBufferViaStagingBuffer(
      device, data_buffer.get(), dst_buffer_size,
      [&](void *ptr, size_t num_bytes) {
        if (is_integer) {
          int *dst_int_buffer = reinterpret_cast<int *>(ptr);
          dst_int_buffer[0] = 0;
        } else {
          float *dst_float_buffer = reinterpret_cast<float *>(ptr);
          dst_float_buffer[0] = 0.f;
        }
      }));

  //===-------------------------------------------------------------------===/
  // Dispatch
  //===-------------------------------------------------------------------===/

  std::vector<::uvkc::vulkan::Device::BoundBuffer> bound_buffers = {
      {src_buffer.get(), /*set=*/0, /*binding=*/0},
      {dst_buffer.get(), /*set=*/0, /*binding=*/1},
  };
  BM_CHECK_OK(device->AttachBufferToDescriptor(
      *shader_module, layout_set_map,
      {bound_buffers.data(), bound_buffers.size()}));

  BM_CHECK_EQ(shader_module->descriptor_set_layouts().size(), 1)
      << "unexpected number of descriptor sets";
  auto descriptor_set_layout = shader_module->descriptor_set_layouts().front();

  std::vector<::uvkc::vulkan::CommandBuffer::BoundDescriptorSet>
      bound_descriptor_sets(1);
  bound_descriptor_sets[0].index = 0;
  bound_descriptor_sets[0].set = layout_set_map.at(descriptor_set_layout);
  BM_CHECK_OK_AND_ASSIGN(auto dispatch_cmdbuf, device->AllocateCommandBuffer());

  // Zeroing the output buffer
  BM_CHECK_OK(dispatch_cmdbuf->Begin());
  dispatch_cmdbuf->CopyBuffer(*data_buffer, 0, *dst_buffer, 0, dst_buffer_size);
  BM_CHECK_OK(dispatch_cmdbuf->End());
  BM_CHECK_OK(device->QueueSubmitAndWait(*dispatch_cmdbuf));
  BM_CHECK_OK(dispatch_cmdbuf->Reset());

  BM_CHECK_OK(dispatch_cmdbuf->Begin());
  dispatch_cmdbuf->BindPipelineAndDescriptorSets(
      *pipeline, {bound_descriptor_sets.data(), bound_descriptor_sets.size()});
  dispatch_cmdbuf->Dispatch(total_elements / batch_elements, 1, 1);
  BM_CHECK_OK(dispatch_cmdbuf->End());
  BM_CHECK_OK(device->QueueSubmitAndWait(*dispatch_cmdbuf));

  //===-------------------------------------------------------------------===/
  // Verify destination buffer data
  //===-------------------------------------------------------------------===/

  BM_CHECK_OK(::uvkc::benchmark::GetDeviceBufferViaStagingBuffer(
      device, dst_buffer.get(), dst_buffer_size,
      [&](void *ptr, size_t num_bytes) {
        if (is_integer) {
          int *dst_int_buffer = reinterpret_cast<int *>(ptr);
          int total = 0;
          for (size_t i = 0; i < total_elements; i++) {
            total += generate_int_data(i);
          };
          BM_CHECK_EQ(dst_int_buffer[0], total)
              << "destination buffer element #0 has incorrect value: "
                 "expected to be "
              << total << " but found " << dst_int_buffer[0];
        } else {
          float *dst_float_buffer = reinterpret_cast<float *>(ptr);
          float total = 0.f;
          for (size_t i = 0; i < total_elements; i++) {
            total += generate_float_data(i);
          };
          BM_CHECK_FLOAT_EQ(dst_float_buffer[0], total, 0.01f)
              << "destination buffer element #0 has incorrect value: "
                 "expected to be "
              << total << " but found " << dst_float_buffer[0];
        }
      }));

  //===-------------------------------------------------------------------===/
  // Benchmarking
  //===-------------------------------------------------------------------===/

  std::unique_ptr<::uvkc::vulkan::TimestampQueryPool> query_pool;
  bool use_timestamp =
      latency_measure->mode == LatencyMeasureMode::kGpuTimestamp;
  if (use_timestamp) {
    BM_CHECK_OK_AND_ASSIGN(query_pool, device->CreateTimestampQueryPool(2));
  }

  BM_CHECK_OK_AND_ASSIGN(auto cmdbuf, device->AllocateCommandBuffer());
  for (auto _ : state) {
    // Zeroing the output buffer
    BM_CHECK_OK(cmdbuf->Begin());
    cmdbuf->CopyBuffer(*data_buffer, 0, *dst_buffer, 0, dst_buffer_size);
    BM_CHECK_OK(cmdbuf->End());
    BM_CHECK_OK(device->QueueSubmitAndWait(*cmdbuf));
    BM_CHECK_OK(cmdbuf->Reset());

    BM_CHECK_OK(cmdbuf->Begin());
    if (use_timestamp) cmdbuf->ResetQueryPool(*query_pool);

    cmdbuf->BindPipelineAndDescriptorSets(
        *pipeline,
        {bound_descriptor_sets.data(), bound_descriptor_sets.size()});

    if (use_timestamp) {
      cmdbuf->WriteTimestamp(*query_pool, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
    }

    cmdbuf->Dispatch(total_elements / batch_elements, 1, 1);

    if (use_timestamp) {
      cmdbuf->WriteTimestamp(*query_pool, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             1);
    }

    BM_CHECK_OK(cmdbuf->End());

    auto start_time = std::chrono::high_resolution_clock::now();
    BM_CHECK_OK(device->QueueSubmitAndWait(*cmdbuf));
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(end_time -
                                                                  start_time);

    switch (latency_measure->mode) {
      case LatencyMeasureMode::kSystemDispatch: {
        state.SetIterationTime(elapsed_seconds.count() -
                               latency_measure->overhead_seconds);
      } break;
      case LatencyMeasureMode::kSystemSubmit: {
        state.SetIterationTime(elapsed_seconds.count());
      } break;
      case LatencyMeasureMode::kGpuTimestamp: {
        BM_CHECK_OK_AND_ASSIGN(
            double timestamp_seconds,
            query_pool->CalculateElapsedSecondsBetween(0, 1));
        state.SetIterationTime(timestamp_seconds);
      } break;
    }

    BM_CHECK_OK(cmdbuf->Reset());
  }

  state.SetBytesProcessed(state.iterations() * src_buffer_size);
  state.counters["FLOps"] =
      ::benchmark::Counter(total_elements,
                           ::benchmark::Counter::kIsIterationInvariant |
                               ::benchmark::Counter::kIsRate,
                           ::benchmark::Counter::kIs1000);

  // Reset the command pool to release all command buffers in the benchmarking
  // loop to avoid draining GPU resources.
  BM_CHECK_OK(device->ResetCommandPool());
}

namespace uvkc {
namespace benchmark {

absl::StatusOr<std::unique_ptr<VulkanContext>> CreateVulkanContext() {
  return CreateDefaultVulkanContext(kBenchmarkName);
}

bool RegisterVulkanOverheadBenchmark(
    const vulkan::Driver::PhysicalDeviceInfo &physical_device,
    vulkan::Device *device, double *overhead_seconds) {
  return false;
}

void RegisterVulkanBenchmarks(
    const vulkan::Driver::PhysicalDeviceInfo &physical_device,
    vulkan::Device *device, const LatencyMeasure *latency_measure) {
  const char *gpu_name = physical_device.v10_properties.deviceName;

  const size_t total_elements = 1 << 22;  // 4M
  for (const auto &shader : kShaders) {
    std::string test_name =
        absl::StrCat(gpu_name, "/", total_elements,
                     (shader.is_integer ? "xi32/" : "xf32/"), shader.name);
    ::benchmark::RegisterBenchmark(
        test_name.c_str(), Reduce, device, latency_measure, shader.code,
        shader.code_num_bytes / sizeof(uint32_t), total_elements,
        shader.batch_elements, shader.is_integer)
        ->UseManualTime()
        ->Unit(::benchmark::kMicrosecond);
  }
}

}  // namespace benchmark
}  // namespace uvkc
