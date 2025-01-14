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

#include "tfrt/jitrt/diagnostics.h"

#include <memory>
#include <string>
#include <utility>

#include "gtest/gtest.h"
#include "llvm/Support/raw_ostream.h"

namespace tfrt {
namespace jitrt {

TEST(DiagnosticEngineTest, Basic) {
  std::string message;

  DiagnosticEngine engine;
  engine.AddHandler([&](Diagnostic& diagnostic) {
    llvm::raw_string_ostream(message) << diagnostic.str();
    return mlir::success();
  });

  {  // Check that diagnostic is reported when InFlightDiagnostic is destructed.
    InFlightDiagnostic diagnostic = engine.Emit(DiagnosticSeverity::kError);
    diagnostic << "Oops";
  }

  EXPECT_EQ(message, "Oops");
}

}  // namespace jitrt
}  // namespace tfrt
