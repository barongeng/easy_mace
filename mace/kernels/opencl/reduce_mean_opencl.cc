//
// Copyright (c) 2017 XiaoMi All rights reserved.
//

#ifdef MACE_ENABLE_OPENCL

#include "mace/kernels/reduce_mean.h"
#include "mace/core/runtime/opencl/cl2_header.h"
#include "mace/core/runtime/opencl/opencl_runtime.h"
#include "mace/kernels/opencl/helper.h"
#include "mace/utils/tuner.h"

namespace mace {
namespace kernels {

template <typename T>
MaceStatus ReduceMeanFunctor<DeviceType::GPU, T>::operator()(
    const Tensor *input,
    Tensor *output,
    StatsFuture *future) {
  MACE_CHECK_NOTNULL(input);
//  MACE_CHECK(keep_dims_, "reduce mean gpu only support keep dims.");
  MACE_CHECK(input->dim_size() == 4,
             "reduce mean gpu only support 4-dim input");
  MACE_CHECK(axis_.size() == 2 && axis_[0] == 1 && axis_[1] == 2,
             "reduce mean gpu only support 1,2-axis reduce");
  index_t batch = input->dim(0);
  const index_t in_height = input->dim(1);
  const index_t in_width = input->dim(2);
  const index_t channels = input->dim(3);
  const index_t channel_blocks = RoundUpDiv4(channels);
  const uint32_t image_size = static_cast<uint32_t >(in_height * in_width);

  auto runtime = OpenCLRuntime::Global();
  std::vector<uint32_t> gws(3);
  std::vector<uint32_t> lws(3);
  std::vector<index_t> output_shape{batch, 1, 1, channels};
  std::vector<size_t> output_image_shape;
  CalImage2DShape(output_shape, BufferType::IN_OUT_CHANNEL,
                  &output_image_shape);
  MACE_RETURN_IF_ERROR(output->ResizeImage(output_shape, output_image_shape));
  if (kernel_.get() == nullptr) {
    const DataType dt = DataTypeToEnum<T>::value;
    std::set<std::string> built_options;
    std::string kernel_name = MACE_OBFUSCATE_SYMBOL("reduce_mean");
    built_options.emplace("-Dreduce_mean=" + kernel_name);
    built_options.emplace("-DDATA_TYPE=" + DtToUpstreamCLDt(dt));
    built_options.emplace("-DCMD_DATA_TYPE=" + DtToUpstreamCLCMDDt(dt));
    if (runtime->gpu_type() != GPUType::QUALCOMM_ADRENO) {
      built_options.emplace("-DNON_QUALCOMM_ADRENO");
    }
    if (runtime->IsOutOfRangeCheckEnabled()) {
      built_options.emplace("-DOUT_OF_RANGE_CHECK");
      kernel_error_ = std::move(std::unique_ptr<Buffer>(
          new Buffer(GetDeviceAllocator(DeviceType::GPU))));
      MACE_RETURN_IF_ERROR(kernel_error_->Allocate(1));
      kernel_error_->Map(nullptr);
      *(kernel_error_->mutable_data<char>()) = 0;
      kernel_error_->UnMap();
    }
    kwg_size_ =
        static_cast<uint32_t>(runtime->GetKernelMaxWorkGroupSize(kernel_));

    if (runtime->IsNonUniformWorkgroupsSupported()) {
      built_options.emplace("-DNON_UNIFORM_WORK_GROUP");
    }
    kernel_ = runtime->BuildKernel("reduce_mean", kernel_name, built_options);
  }

  if (runtime->gpu_type() == GPUType::QUALCOMM_ADRENO) {
    const uint32_t wave_size =
        static_cast<uint32_t>(runtime->GetKernelWaveSize(kernel_));
    gws = {4, (wave_size / 4), static_cast<uint32_t>(batch * channel_blocks)};
  } else {
    gws = {4, 16, static_cast<uint32_t>(batch * channel_blocks)};
  }
  lws = {gws[0], gws[1], 1};
  const int group_size = lws[0] * lws[1] * lws[2];
  const int partial_len = (image_size + group_size - 1) / group_size;
  const int remain_index = image_size % group_size;
  const float in_width_reciprocal = 1.f / in_width;
  const float img_size_reciprocal = 1.f / (in_width * in_height);
  const float channel_blk_reciprocal = 1.f / channel_blocks;

  if (!IsVecEqual(input_shape_, input->shape())) {
    uint32_t idx = 0;
    if (runtime->IsOutOfRangeCheckEnabled()) {
      kernel_.setArg(idx++,
                     *(static_cast<cl::Buffer *>(kernel_error_->buffer())));
    }
    if (!runtime->IsNonUniformWorkgroupsSupported()) {
      kernel_.setArg(idx++, gws[0]);
      kernel_.setArg(idx++, gws[1]);
      kernel_.setArg(idx++, gws[2]);
    }
    kernel_.setArg(idx++, *(input->opencl_image()));
    kernel_.setArg(idx++, (group_size * 4 * sizeof(T)),
                   nullptr);
    kernel_.setArg(idx++, static_cast<int32_t>(group_size));
    kernel_.setArg(idx++, static_cast<int32_t>(partial_len));
    kernel_.setArg(idx++, static_cast<int32_t>(remain_index));
    kernel_.setArg(idx++, static_cast<int32_t>(batch));
    kernel_.setArg(idx++, static_cast<int32_t>(in_height));
    kernel_.setArg(idx++, static_cast<int32_t>(in_width));
    kernel_.setArg(idx++, img_size_reciprocal);
    kernel_.setArg(idx++, in_width_reciprocal);
    kernel_.setArg(idx++, static_cast<int32_t>(channel_blocks));
    kernel_.setArg(idx++, channel_blk_reciprocal);
    kernel_.setArg(idx++, *(output->opencl_image()));

    input_shape_ = input->shape();
  }

  cl::Event event;
  cl_int error;
  if (runtime->IsNonUniformWorkgroupsSupported()) {
    error = runtime->command_queue().enqueueNDRangeKernel(
        kernel_, cl::NullRange, cl::NDRange(gws[0], gws[1], gws[2]),
        cl::NDRange(lws[0], lws[1], lws[2]), nullptr, &event);
  } else {
    std::vector<uint32_t> roundup_gws(lws.size());
    for (size_t i = 0; i < lws.size(); ++i) {
      roundup_gws[i] = RoundUp(gws[i], lws[i]);
    }
    error = runtime->command_queue().enqueueNDRangeKernel(
        kernel_, cl::NullRange,
        cl::NDRange(roundup_gws[0], roundup_gws[1], roundup_gws[2]),
        cl::NDRange(lws[0], lws[1], lws[2]), nullptr, &event);
  }
  if (runtime->IsOutOfRangeCheckEnabled()) {
    kernel_error_->Map(nullptr);
    char *kerror_code = kernel_error_->mutable_data<char>();
    MACE_CHECK(*kerror_code == 0) << "Kernel error code: " << *kerror_code;
    kernel_error_->UnMap();
  }
  MACE_CHECK(error == CL_SUCCESS) << "Error code: " << error;

  if (future != nullptr) {
    future->wait_fn = [runtime, event](CallStats *stats) {
      event.wait();
      if (stats != nullptr) {
        runtime->GetCallStats(event, stats);
      }
    };
  }

  return MACE_SUCCESS;
}

template struct ReduceMeanFunctor<DeviceType::GPU, float>;
template struct ReduceMeanFunctor<DeviceType::GPU, half>;
}  // namespace kernels
}  // namespace mace

#endif