/*
 * Copyright 2021 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <memory>
#include <utility>

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "llvm/ADT/SmallVector.h"
#include "tfrt/host_context/concurrent_work_queue.h"
#include "tfrt/host_context/host_allocator.h"
#include "tfrt/jitrt/jitrt.h"
#include "tfrt/jitrt/jitrt_compiler.h"
#include "tfrt/support/logging.h"

namespace tfrt {
namespace jitrt {

using ::llvm::SmallVector;

using SymbolicShape = SymbolicShapesResolver::SymbolicShape;

// -------------------------------------------------------------------------- //
// Performance benchmarks to measure specialized executable lookup overhead.
// -------------------------------------------------------------------------- //

static const char* mlir_module = R"(
    func.func @compute(%arg0: memref<?x?xf32>,
                       %arg1: memref<?x?xf32>,
                       %arg3: memref<?x?xf32>,
                       %arg4: memref<16x32xf32>) {
      func.return
    })";

static const char* entrypoint = "compute";

std::unique_ptr<HostContext> CreateSingleThreadedHostContext() {
  return std::make_unique<HostContext>(
      [](const tfrt::DecodedDiagnostic& diag) {
        TFRT_LOG(FATAL) << "Runtime error: " << diag.message << "\n";
      },
      tfrt::CreateMallocAllocator(), tfrt::CreateSingleThreadedWorkQueue());
}

// Create fake memref operands from the operands shapes.
SmallVector<MemrefDesc> GetFakeMemrefs(SmallVector<SymbolicShape> shapes) {
  SmallVector<MemrefDesc> memrefs;
  memrefs.reserve(shapes.size());

  for (auto& shape : shapes) {
    MemrefDesc desc(DType::F32, nullptr, 0, shape, shape /* fake strides */);
    memrefs.push_back(std::move(desc));
  }

  return memrefs;
}

void BenchmarkGetExecutable(benchmark::State& state,
                            SmallVector<MemrefDesc> operands) {
  auto host = CreateSingleThreadedHostContext();

  CompilationOptions opts;
  opts.specialization = CompilationOptions::Specialization::kAlways;
  opts.register_dialects = RegisterDefaultJitRtDialects;

  CompilationPipelineOptions copts;
  opts.create_compilation_pipeline = [copts](mlir::PassManager& pm) {
    CreateDefaultJitRtCompilationPipeline(pm, copts);
  };

  llvm::Expected<JitExecutable> jit_executable =
      JitExecutable::Instantiate(mlir_module, entrypoint, opts);
  if (auto err = jit_executable.takeError()) TFRT_LOG(FATAL) << err;

  // Initialize specialization cache.
  Expected<AsyncValuePtr<Executable>> initialize =
      jit_executable->GetExecutable(operands);
  if (auto err = initialize.takeError()) TFRT_LOG(FATAL) << err;

  // Check that compilation was successful.
  host->Quiesce();
  if (initialize->IsError()) TFRT_LOG(FATAL) << initialize->GetError();

  for (auto _ : state) {
    Expected<AsyncValuePtr<Executable>> specialize =
        jit_executable->GetExecutable(operands);
    benchmark::DoNotOptimize(specialize);
  }
}

void BenchmarkInitializeCallFrame(benchmark::State& state,
                                  SmallVector<MemrefDesc> operands,
                                  bool verify) {
  auto host = CreateSingleThreadedHostContext();

  CompilationOptions opts;
  opts.specialization = CompilationOptions::Specialization::kAlways;
  opts.register_dialects = RegisterDefaultJitRtDialects;

  CompilationPipelineOptions copts;
  opts.create_compilation_pipeline = [copts](mlir::PassManager& pm) {
    CreateDefaultJitRtCompilationPipeline(pm, copts);
  };

  llvm::Expected<JitExecutable> jit_executable =
      JitExecutable::Instantiate(mlir_module, entrypoint, opts);
  if (auto err = jit_executable.takeError()) TFRT_LOG(FATAL) << err;

  // Get the executable.
  Expected<AsyncValuePtr<Executable>> executable =
      jit_executable->GetExecutable(operands);
  if (auto err = executable.takeError()) TFRT_LOG(FATAL) << err;

  // Check that compilation was successful.
  host->Quiesce();
  if (executable->IsError()) TFRT_LOG(FATAL) << executable->GetError();

  for (auto _ : state) {
    Executable::CallFrame call_frame;
    auto err =
        (*executable)->InitializeCallFrame(operands, &call_frame, verify);
    benchmark::DoNotOptimize(call_frame);
  }
}

// -------------------------------------------------------------------------- //

#define BM_GetExecutable(NAME, OPERANDS)                        \
  static void BM_GetExecutable##NAME(benchmark::State& state) { \
    BenchmarkGetExecutable(state, OPERANDS);                    \
  }                                                             \
  BENCHMARK(BM_GetExecutable##NAME)

BM_GetExecutable(UniqueShapes,
                 GetFakeMemrefs({{10, 11}, {12, 13}, {14, 15}, {16, 32}}));

BM_GetExecutable(SameShapes,
                 GetFakeMemrefs({{10, 11}, {10, 11}, {10, 11}, {16, 32}}));

BM_GetExecutable(KnownShapes,
                 GetFakeMemrefs({{16, 32}, {16, 32}, {16, 32}, {16, 32}}));

// -------------------------------------------------------------------------- //

#define BM_InitializeCallFrame(NAME, OPERANDS, VERIFY)     \
  static void BM_InitializeCallFrame##NAME##_##VERIFY(     \
      benchmark::State& state) {                           \
    BenchmarkInitializeCallFrame(state, OPERANDS, VERIFY); \
  }                                                        \
  BENCHMARK(BM_InitializeCallFrame##NAME##_##VERIFY)

BM_InitializeCallFrame(UniqueShapes,
                       GetFakeMemrefs({{10, 11}, {12, 13}, {14, 15}, {16, 32}}),
                       true);

BM_InitializeCallFrame(SameShapes,
                       GetFakeMemrefs({{10, 11}, {10, 11}, {10, 11}, {16, 32}}),
                       true);

BM_InitializeCallFrame(KnownShapes,
                       GetFakeMemrefs({{16, 32}, {16, 32}, {16, 32}, {16, 32}}),
                       true);

BM_InitializeCallFrame(UniqueShapes,
                       GetFakeMemrefs({{10, 11}, {12, 13}, {14, 15}, {16, 32}}),
                       false);

BM_InitializeCallFrame(SameShapes,
                       GetFakeMemrefs({{10, 11}, {10, 11}, {10, 11}, {16, 32}}),
                       false);

BM_InitializeCallFrame(KnownShapes,
                       GetFakeMemrefs({{16, 32}, {16, 32}, {16, 32}, {16, 32}}),
                       false);

}  // namespace jitrt
}  // namespace tfrt
