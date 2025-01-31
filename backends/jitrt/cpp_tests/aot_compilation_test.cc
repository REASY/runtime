/*
 * Copyright 2022 The TensorFlow Runtime Authors
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
#include <vector>

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
namespace {

using ::llvm::SmallVector;

// Simple function that copies 4xf32 values from `arg0` to `arg1`.
static const char* mlir_module = R"(
    func.func @compute(%arg0: memref<?xf32>, %arg1: memref<?xf32>) {
      %c0 = arith.constant 0 : index
      %c1 = arith.constant 1 : index
      %c2 = arith.constant 2 : index
      %c3 = arith.constant 3 : index
      %0 = memref.load %arg0[%c0] : memref<?xf32>
      %1 = memref.load %arg0[%c1] : memref<?xf32>
      %2 = memref.load %arg0[%c2] : memref<?xf32>
      %3 = memref.load %arg0[%c3] : memref<?xf32>
      memref.store %0, %arg1[%c0] : memref<?xf32>
      memref.store %1, %arg1[%c1] : memref<?xf32>
      memref.store %2, %arg1[%c2] : memref<?xf32>
      memref.store %3, %arg1[%c3] : memref<?xf32>
      func.return
    })";

static const char* entrypoint = "compute";

TEST(AotCompilationTest, CompileSaveRestore) {
  CompilationOptions opts;
  opts.specialization = CompilationOptions::Specialization::kDisabled;
  opts.register_dialects = RegisterDefaultJitRtDialects;

  CompilationPipelineOptions copts;
  opts.create_compilation_pipeline = [&](mlir::PassManager& pm) {
    CreateDefaultJitRtCompilationPipeline(pm, copts);
  };

  llvm::Expected<JitExecutable> jit_executable =
      JitExecutable::Instantiate(mlir_module, entrypoint, opts);
  ASSERT_FALSE(jit_executable.takeError());

  AsyncValuePtr<Executable> executable = jit_executable->DefaultExecutable();
  Await(executable.value());  // make sure executable is available

  // Allocate storage for arguments.
  std::vector<float> arg0 = {1.0, 2.0, 3.0, 4.0};
  std::vector<float> arg1(4, 0.0);

  // Prepare memref descriptors for the executable.
  llvm::SmallVector<MemrefDesc> args;
  args.emplace_back(DType::F32, arg0.data(), 0, 4, 1);
  args.emplace_back(DType::F32, arg1.data(), 0, 4, 1);

  Executable::ExecuteOpts execute_opts;
  execute_opts.async_task_runner =
      reinterpret_cast<jitrt::AsyncTaskRunner*>(0XDEADBEEF);

  NoResultConverter converter;

  // Execute Jit compiled executable.
  ASSERT_FALSE(executable->Execute(args, converter, execute_opts));

  // Check that `arg0` was copied into `arg1`.
  EXPECT_EQ(arg1, arg0);

  // Reset `arg1` to zeroes.
  arg1.clear();
  arg1.resize(4, 0.0);
  EXPECT_EQ(arg1, std::vector<float>({0.0, 0.0, 0.0, 0.0}));

  // "Save" the object file behind the executable.
  auto obj_file = executable->obj_file();
  EXPECT_TRUE(obj_file);
  EXPECT_GT(obj_file->getBufferSize(), 0);

  // Load executable from an object file.
  llvm::SmallVector<std::unique_ptr<Type>> operands;
  operands.push_back(std::make_unique<MemrefType>(4, DType::F32));
  operands.push_back(std::make_unique<MemrefType>(4, DType::F32));

  llvm::SmallVector<std::unique_ptr<Type>> rt_operands;
  rt_operands.push_back(std::make_unique<KernelContextOperandType>());
  rt_operands.push_back(std::make_unique<MemrefType>(4, DType::F32));
  rt_operands.push_back(std::make_unique<MemrefType>(4, DType::F32));

  FunctionType signature(std::move(operands), /*results=*/{});
  FunctionType rt_signature(std::move(rt_operands), /*results=*/{});

  llvm::Expected<Executable> loaded = Executable::LoadFromObjFile(
      "aot", std::move(obj_file), entrypoint, std::move(signature),
      std::move(rt_signature), /*runtime_symbol_map=*/{}, "aot_mem_region");
  ASSERT_FALSE(loaded.takeError());

  // Execute AOT executable.
  ASSERT_FALSE(loaded->Execute(args, converter, execute_opts));

  // Check that `arg0` was copied into `arg1`.
  EXPECT_EQ(arg1, arg0);
}

}  // namespace
}  // namespace jitrt
}  // namespace tfrt
