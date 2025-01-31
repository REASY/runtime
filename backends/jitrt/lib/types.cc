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

//===- types.cc - ---------------------------------------------------------===//
// Types supported at the JitRt function boundary.
//===----------------------------------------------------------------------===//

#include "tfrt/jitrt/types.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "llvm/ADT/STLExtras.h"
#include "mlir/Dialect/Async/IR/AsyncTypes.h"
#include "tfrt/support/error_util.h"
#include "third_party/tensorflow/compiler/xla/mlir/ir/runtime/rt_ops.h"

namespace tfrt {
namespace jitrt {

//===----------------------------------------------------------------------===//
// Pretty printing for canonical types.
//===----------------------------------------------------------------------===//

static raw_ostream& operator<<(raw_ostream& os, const ArrayRef<Index>& arr) {
  auto str = llvm::map_range(arr, [](Index i) { return std::to_string(i); });
  return os << llvm::join(str, "x") << (arr.empty() ? "" : "x");
}

raw_ostream& AsyncTokenType::print(raw_ostream& os) const {
  return os << "!async.token";
}

raw_ostream& AsyncValueType::print(raw_ostream& os) const {
  return os << "!async.value<" << value_type() << ">";
}

raw_ostream& RankedTensorType::print(raw_ostream& os) const {
  return os << "tensor<" << sizes() << element_type() << ">";
}

raw_ostream& UnrankedTensorType::print(raw_ostream& os) const {
  return os << "tensor<*x" << element_type() << ">";
}

raw_ostream& MemrefType::print(raw_ostream& os) const {
  return os << "memref<" << sizes() << element_type() << ">";
}

raw_ostream& UnrankedMemrefType::print(raw_ostream& os) const {
  return os << "memref<*x" << element_type() << ">";
}

raw_ostream& KernelContextOperandType::print(raw_ostream& os) const {
  return os << "!rt.kernel_context";
}

//===----------------------------------------------------------------------===//
// ABI definition for canonical types.
//===----------------------------------------------------------------------===//

using ArgumentAbi = Type::ArgumentAbi;
using ResultAbi = Type::ResultAbi;

// Async token returned as a pointer to the runtime async token.
mlir::FailureOr<ResultAbi> AsyncTokenType::AsResult() const {
  return ResultAbi{sizeof(void*)};
}

// Async value returned as a pointer to the runtime async token.
mlir::FailureOr<ResultAbi> AsyncValueType::AsResult() const {
  return ResultAbi{sizeof(void*)};
}

// Memref passed as an unrolled strided memref type.
mlir::FailureOr<ArgumentAbi> MemrefType::AsArgument() const {
  return ArgumentAbi{3 + 2 * rank()};
}

// TODO(ezhulenev): We should query the size of the `StridedMemrefType`
// directly, however it introduces dependency on the MLIR C runner utils.
//
// Memrefs are returned as StridedMemref<T, rank> type:
//   basePtr, data, offset, sizes[rank], strides[rank]
mlir::FailureOr<ResultAbi> MemrefType::AsResult() const {
  return ResultAbi{
      sizeof(void*) * 2 +           // pointers
      sizeof(int64_t) +             // offset
      sizeof(int64_t) * 2 * rank()  // sizes and strides
  };
}

// Kernel context passed as a single opaque pointer.
mlir::FailureOr<ArgumentAbi> KernelContextOperandType::AsArgument() const {
  return ArgumentAbi{1};
}

//===----------------------------------------------------------------------===//
// Compiled function signature types conversion from the MLIR types.
//===----------------------------------------------------------------------===//

// Type conversion for the canonical MLIR types supported by the runtime.
static std::unique_ptr<Type> ConvertCanonicalType(
    mlir::Type type, const TypeConverter& convert) {
  // KernelContextType -> KernelContextOperandType (both in tfrt::jitrt).
  if (auto ctx = type.dyn_cast<KernelContextType>())
    return std::make_unique<KernelContextOperandType>();

  // mlir::async::TokenType -> tfrt::jitrt::AsyncTokenType
  if (type.isa<mlir::async::TokenType>())
    return std::make_unique<AsyncTokenType>();

  // mlir::async::ValueType -> tfrt::jitrt::AsyncValueType
  if (auto value = type.dyn_cast<mlir::async::ValueType>()) {
    if (auto value_type = convert.Convert(value.getValueType()))
      return std::make_unique<AsyncValueType>(std::move(*value_type));
  }

  // mlir::RankedTensorType -> tfrt::jitrt::RankedTensorType
  if (auto tensor = type.dyn_cast<mlir::RankedTensorType>()) {
    if (auto dtype = TypeConverter::ConvertElementType(tensor.getElementType()))
      return std::make_unique<RankedTensorType>(tensor.getShape(), *dtype);
  }

  // mlir::UnrankedTensorType -> tfrt::jitrt::UnrankedTensorType
  if (auto tensor = type.dyn_cast<mlir::UnrankedTensorType>()) {
    if (auto dtype = TypeConverter::ConvertElementType(tensor.getElementType()))
      return std::make_unique<UnrankedTensorType>(*dtype);
  }

  // mlir::MemrefType -> tfrt::jitrt::MemrefType
  if (auto memref = type.dyn_cast<mlir::MemRefType>()) {
    if (auto dtype = TypeConverter::ConvertElementType(memref.getElementType()))
      return std::make_unique<MemrefType>(memref.getShape(), *dtype);
  }

  // mlir::UnrankedMemrefType -> tfrt::jitrt::UnrankedMemrefType
  if (auto memref = type.dyn_cast<mlir::UnrankedMemRefType>()) {
    if (auto dtype = TypeConverter::ConvertElementType(memref.getElementType()))
      return std::make_unique<UnrankedMemrefType>(*dtype);
  }

  // For non-canonical types the user must provide type conversion function.
  return {};
}

/*static*/ Expected<DType> TypeConverter::ConvertElementType(mlir::Type type) {
  if (type.isF32()) return DType::F32;
  if (type.isF64()) return DType::F64;
  if (type.isUnsignedInteger(8)) return DType::UI8;
  if (type.isUnsignedInteger(16)) return DType::UI16;
  if (type.isUnsignedInteger(32)) return DType::UI32;
  if (type.isUnsignedInteger(64)) return DType::UI64;
  if (type.isInteger(1)) return DType::I1;
  if (type.isInteger(8)) return DType::I8;
  if (type.isInteger(16)) return DType::I16;
  if (type.isInteger(32)) return DType::I32;
  if (type.isInteger(64)) return DType::I64;
  if (auto complex_type = type.dyn_cast<mlir::ComplexType>()) {
    auto element_type = complex_type.getElementType();
    if (element_type.isF32()) return DType::Complex64;
    if (element_type.isF64()) return DType::Complex128;
  }

  return MakeStringError("unsupported element type: ", type);
}

Expected<std::unique_ptr<Type>> TypeConverter::Convert(mlir::Type type) const {
  if (auto converted = ConvertCanonicalType(type, *this)) return converted;

  for (const ConversionFn& conversion : conversions_)
    if (auto converted = conversion(type)) return converted;

  return MakeStringError("can't convert type: ", type, " to the run time type");
}

Expected<FunctionType> TypeConverter::Convert(mlir::FunctionType type) const {
  assert(type && "function type must be not null");

  llvm::SmallVector<std::unique_ptr<Type>> operands;
  llvm::SmallVector<std::unique_ptr<Type>> results;

  operands.reserve(type.getNumInputs());
  results.reserve(type.getNumResults());

  auto error = [](string_view kind, unsigned i, mlir::Type type) {
    return MakeStringError("can't convert ", kind, " #", i, " type ", type,
                           " to the run time type");
  };

  for (unsigned i = 0; i < type.getNumInputs(); ++i) {
    Expected<std::unique_ptr<Type>> converted = Convert(type.getInput(i));
    if (!converted) return error("input", i, type.getInput(i));
    operands.push_back(std::move(*converted));
  }

  for (unsigned i = 0; i < type.getNumResults(); ++i) {
    Expected<std::unique_ptr<Type>> converted = Convert(type.getResult(i));
    if (!converted) return error("result", i, type.getResult(i));
    results.push_back(std::move(*converted));
  }

  return FunctionType(std::move(operands), std::move(results));
}

}  // namespace jitrt
}  // namespace tfrt
