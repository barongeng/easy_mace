// Copyright 2018 Xiaomi, Inc.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifdef MACE_ENABLE_OPENCL

#include "mace/ops/buffer_to_image.h"

namespace mace {
namespace ops {

void Register_BufferToImage(OperatorRegistry *op_registry) {
  MACE_REGISTER_OPERATOR(op_registry, OpKeyBuilder("BufferToImage")
                                          .Device(DeviceType::GPU)
                                          .TypeConstraint<float>("T")
                                          .Build(),
                         BufferToImageOp<DeviceType::GPU, float>);

  MACE_REGISTER_OPERATOR(op_registry, OpKeyBuilder("BufferToImage")
                                          .Device(DeviceType::GPU)
                                          .TypeConstraint<half>("T")
                                          .Build(),
                         BufferToImageOp<DeviceType::GPU, half>);
}

}  // namespace ops
}  // namespace mace

#endif