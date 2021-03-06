/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/lite/kernels/cpu_backend_support.h"

#include <memory>

#include "tensorflow/lite/c/c_api_internal.h"
#include "tensorflow/lite/external_cpu_backend_context.h"
#include "tensorflow/lite/kernels/cpu_backend_context.h"
#include "tensorflow/lite/kernels/op_macros.h"

namespace tflite {
namespace cpu_backend_support {

// TODO(b/130950871): Remove all refrences to the following two no-op functions
// once the new ExternalCpuBackendContext class is checked in.
void IncrementUsageCounter(TfLiteContext* context) {}
void DecrementUsageCounter(TfLiteContext* context) {}

CpuBackendContext* GetFromContext(TfLiteContext* context) {
  auto* external_context = static_cast<ExternalCpuBackendContext*>(
      context->GetExternalContext(context, kTfLiteCpuBackendContext));

  if (external_context == nullptr) {
    TF_LITE_FATAL(
        "ExternalCpuBackendContext isn't properly initialized during TFLite "
        "interpreter initialization.");
  }

  auto* cpu_backend_context = static_cast<CpuBackendContext*>(
      external_context->internal_backend_context());
  if (cpu_backend_context == nullptr) {
    // We do the lazy initialization here for the TfLiteInternalBackendContext
    // that's wrapped inside ExternalCpuBackendContext.
    cpu_backend_context = new CpuBackendContext();
    if (context->recommended_num_threads != -1) {
      cpu_backend_context->set_max_num_threads(
          context->recommended_num_threads);
    }
    external_context->set_internal_backend_context(
        std::unique_ptr<TfLiteInternalBackendContext>(cpu_backend_context));
  }

  return cpu_backend_context;
}

}  // namespace cpu_backend_support
}  // namespace tflite
