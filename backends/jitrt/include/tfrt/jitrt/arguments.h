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

#ifndef TFRT_BACKENDS_JITRT_INCLUDE_TFRT_JITRT_ARGUMENTS_H_
#define TFRT_BACKENDS_JITRT_INCLUDE_TFRT_JITRT_ARGUMENTS_H_

#include <cstddef>
#include <type_traits>

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"
#include "tfrt/dtype/dtype.h"
#include "tfrt/jitrt/types.h"

namespace tfrt {
namespace jitrt {

//===----------------------------------------------------------------------===//
// A base class for JitRt compiled kernels arguments.
//===----------------------------------------------------------------------===//

class Argument : public llvm::RTTIExtends<Type, llvm::RTTIRoot> {
 public:
  static constexpr char ID = 0;  // NOLINT

  Argument() = default;

  // Verifies that the argument matches the expected type.
  virtual Error Verify(const Type& type) const = 0;

  // Packs argument into the `args` array starting at the given `offset`
  // according to the expected executable ABI. Return offset incremented by
  // the number of packed pointers, so that result will point to the offset for
  // packing the next argument.
  //
  // Arguments array is guaranteed to be properly sized to have space for all
  // arguments according to the arguments memory layout.
  virtual size_t Pack(MutableArrayRef<void*> args, size_t offset) const = 0;

  virtual raw_ostream& print(raw_ostream& os) const = 0;
};

inline raw_ostream& operator<<(raw_ostream& os, const Argument& arg) {
  return arg.print(os);
}

//===----------------------------------------------------------------------===//
// Owning container for storing arguments of different types.
//===----------------------------------------------------------------------===//

// Forward declare class defined below.
class ArgumentsRef;

// An owning container for the variadic arguments, optimized for storing all
// arguments of the declared types without dynamic memory allocations.
//
// Example:
//
//   Arguments<OpaqueArg, MemrefDesc> arguments;
//   arguments.emplace_back<OpaqueArg>(...);
//
// Variadic type parameter `Ts` specifies arguments of what types can be added
// to the container.
template <typename... Ts>
class Arguments {
 public:
  explicit Arguments(size_t num_args) : num_args_(num_args) {
    storage_.reserve(num_args);
  }

  ~Arguments() {
    for (size_t i = 0; i < storage_.size(); ++i) (*this)[i].~Argument();
  }

  template <typename T>
  T& push_back(T value) {
    static_assert(std::disjunction_v<std::is_same<T, Ts>...>,
                  "type is not supported by this instance of arguments");
    assert(storage_.size() < num_args_ && "arguments overflow");
    storage_.resize_for_overwrite(storage_.size() + 1);
    return *(new (&storage_.back()) T(std::forward<T>(value)));
  }

  template <typename T = std::tuple_element_t<0, std::tuple<Ts...>>,
            typename... Args>
  T& emplace_back(Args... args) {
    static_assert(std::disjunction_v<std::is_same<T, Ts>...>,
                  "type is not supported by this instance of arguments");
    assert(storage_.size() < num_args_ && "arguments overflow");
    storage_.resize_for_overwrite(storage_.size() + 1);
    return *(new (&storage_.back()) T(std::forward<Args>(args)...));
  }

  const Argument& operator[](size_t index) const {
    return *reinterpret_cast<const Argument*>(storage_[index].data);
  }

  size_t size() const { return storage_.size(); }

 private:
  friend class ArgumentsRef;

  static_assert(std::conjunction_v<std::is_base_of<Argument, Ts>...>,
                "all types must be arguments");

  // Arguments are not movable or copyable because we do manual memory
  // management using the `Storage` struct, and moving or copying bytes storing
  // the argument value is undefined behavior.
  Arguments(const Arguments&) = delete;
  Arguments& operator=(const Arguments&) = delete;
  Arguments(Arguments&&) = delete;
  Arguments& operator=(Arguments&&) = delete;

  // Avoid dynamic memory allocation for storing arguments of different types
  // by storing them in the properly aligned byte array.
  struct Storage {
    alignas(Ts...) std::byte data[std::max({sizeof(Ts)...})];
  };

  // To guarantee safe conversion between pointer to `Storage` and pointer to
  // the first byte (Argument), the storage struct must have standard layout.
  static_assert(std::is_standard_layout_v<Storage>,
                "storage must have standard layout");

  size_t num_args_;
  llvm::SmallVector<Storage> storage_;
};

// A constant reference to an array of arguments, somewhat similar to the
// `ArrayRef<Argument>`, however because `ArrayRef` of a virtual base is not
// possible, we have our own type that is constructible from the `Arguments`
// and array reference or vector of any argument subtype.
class ArgumentsRef {
  template <typename T>
  static constexpr bool is_argument = std::is_base_of_v<Argument, T>;

 public:
  template <typename... Ts>
  ArgumentsRef(const Arguments<Ts...>& args)  // NOLINT
      : data_(reinterpret_cast<const Argument*>(args.storage_.data())),
        size_(args.size()),
        stride_(sizeof(typename Arguments<Ts...>::Storage)) {}

  template <typename T, std::enable_if_t<is_argument<T>>* = nullptr>
  ArgumentsRef(llvm::ArrayRef<T> ref)  // NOLINT
      : data_(ref.data()), size_(ref.size()), stride_(sizeof(T)) {}

  template <typename T, std::enable_if_t<is_argument<T>>* = nullptr>
  ArgumentsRef(const llvm::SmallVectorImpl<T>& vec)  // NOLINT
      : ArgumentsRef(ArrayRef<T>(vec)) {}

  template <typename T, std::enable_if_t<is_argument<T>>* = nullptr>
  ArgumentsRef(const std::vector<T>& vec)  // NOLINT
      : ArgumentsRef(ArrayRef<T>(vec)) {}

  template <typename T, size_t n, std::enable_if_t<is_argument<T>>* = nullptr>
  ArgumentsRef(const std::array<T, n>& arr)  // NOLINT
      : ArgumentsRef(ArrayRef<T>(arr)) {}

  const Argument& operator[](size_t index) const {
    assert(index < size_ && "index out of bounds");
    auto* ptr = reinterpret_cast<const std::byte*>(data_) + index * stride_;
    return *reinterpret_cast<const Argument*>(ptr);
  }

  size_t size() const { return size_; }

 private:
  // Arguments stored in the contiguous memory starting at `data_` pointer,
  // with the given `stride_` in bytes.
  const Argument* data_;
  size_t size_;
  size_t stride_;
};

//===----------------------------------------------------------------------===//
// Canonical types for passing compiled kernel arguments.
//===----------------------------------------------------------------------===//

// By default we provide a set of types for passing common arguments to the
// compiled kernel. The type hierarchy is open, and users can extend it by
// definining new `Type` and `Argument` with the corresponding MLIR types and
// MLIR passes to lower types and operations to the LLVM dialect.

//===----------------------------------------------------------------------===//
// OpaqueArg for passing `!llvm.ptr` (opaque pointer) arguments.
//===----------------------------------------------------------------------===//

class OpaqueArg final : public llvm::RTTIExtends<OpaqueArg, Argument> {
 public:
  static constexpr char ID = 0;  // NOLINT

  explicit OpaqueArg(void* ptr) : ptr_(ptr) {}

  void* ptr() const { return ptr_; }

  Error Verify(const Type& type) const final;
  size_t Pack(MutableArrayRef<void*> args, size_t offset) const final;
  raw_ostream& print(raw_ostream& os) const final;

 private:
  void* ptr_;
};

//===----------------------------------------------------------------------===//
// MemrefDesc for passing `memref` arguments.
//===----------------------------------------------------------------------===//

class MemrefDesc final : public llvm::RTTIExtends<MemrefDesc, Argument> {
 public:
  static constexpr char ID = 0;  // NOLINT

  MemrefDesc(DType dtype, void* data, Index offset, ArrayRef<Index> sizes,
             ArrayRef<Index> strides)
      : rank_(sizes.size()), dtype_(dtype), data_(data), offset_(offset) {
    assert(sizes.size() == strides.size() && "invalid sizes and strides pair");
    sizes_and_strides_.reserve(2 * rank_);
    sizes_and_strides_.append(sizes.begin(), sizes.end());
    sizes_and_strides_.append(strides.begin(), strides.end());
  }

  // Constructs MemrefDesc of the given rank and calls user-provided callback to
  // initialize sizes and strides.
  //
  // Expected `InitializeSizesAndStrides` callback signature:
  //
  //   void operator()(MutableArrayRef<Index> sizes,
  //                   MutableArrayRef<Index> strides);
  //
  // We pass the init callback as a template argument to be able to
  // inline it at the call site, because MemrefDesc construction is on a hot
  // path.
  template <typename InitializeSizesAndStrides>
  MemrefDesc(unsigned rank, DType dtype, void* data, Index offset,
             InitializeSizesAndStrides initialize);

  // Ensure that MemrefDesc is always moved around instead of copying.
  MemrefDesc(const MemrefDesc&) = delete;
  MemrefDesc& operator=(const MemrefDesc&) = delete;
  MemrefDesc(MemrefDesc&&) = default;
  MemrefDesc& operator=(MemrefDesc&&) = default;

  unsigned rank() const { return rank_; }
  DType dtype() const { return dtype_; }

  void* data() const { return data_; }
  Index offset() const { return offset_; }

  Index size(size_t index) const { return sizes_and_strides_[index]; }
  Index stride(size_t index) const { return sizes_and_strides_[rank_ + index]; }

  ArrayRef<Index> sizes() const { return {sizes_and_strides_.data(), rank_}; }
  ArrayRef<Index> strides() const {
    return {sizes_and_strides_.data() + rank_, rank_};
  }

  Error Verify(const Type& type) const final;
  size_t Pack(MutableArrayRef<void*> args, size_t offset) const final;
  raw_ostream& print(raw_ostream& os) const final;

 private:
  unsigned rank_;
  DType dtype_;
  void* data_;
  Index offset_;
  // We keep sizes and strides in a single container to save one potential
  // memory allocation for memrefs of higher ranks, and to save one vector
  // constructor/destructor call.
  llvm::SmallVector<Index, 8> sizes_and_strides_;
};

template <typename InitializeSizesAndStrides>
MemrefDesc::MemrefDesc(unsigned rank, DType dtype, void* data, Index offset,
                       InitializeSizesAndStrides initialize)
    : rank_(rank), dtype_(dtype), data_(data), offset_(offset) {
  sizes_and_strides_.resize(2 * rank_);
  llvm::MutableArrayRef<Index> ref = sizes_and_strides_;
  initialize(ref.drop_back(rank_), ref.drop_front(rank_));
}

//===----------------------------------------------------------------------===//
// Verify that operands types are matching runtime arguments.
//===----------------------------------------------------------------------===//

// We pass operand index to all verification functions to get a user-friendly
// error messages in case of an error.

Error VerifyMemrefOperand(unsigned index, DType element_type,
                          Optional<ArrayRef<Index>> sizes,
                          const MemrefDesc& memref);

Error VerifyMemrefOperand(unsigned index, const RankedTensorType& type,
                          const MemrefDesc& memref);

Error VerifyMemrefOperand(unsigned index, const MemrefType& type,
                          const MemrefDesc& memref);

Error VerifyMemrefOperand(unsigned index, mlir::ShapedType type,
                          const MemrefDesc& memref);

}  // namespace jitrt
}  // namespace tfrt

#endif  // TFRT_BACKENDS_JITRT_INCLUDE_TFRT_JITRT_ARGUMENTS_H_
