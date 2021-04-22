// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "gtest/gtest.h"

#include "core/graph/contrib_ops/contrib_defs.h"
#include "test/contrib_ops/function_test_util.h"

using namespace ::onnxruntime::common;

namespace onnxruntime {
namespace test {

static void RegisterSchemas() {
  static bool registered = false;
  if (!registered) {
    onnxruntime::contrib::RegisterContribSchemas();
    registered = true;
  }
}

template <typename T, typename U, bool RunTest>
void CheckLayerNorm() {
  FunctionTestCase testCase("LayerNormalization", kOnnxDomain);
  std::vector<int64_t> shape1{8, 16};
  std::vector<int64_t> shape2{16};

  testCase.AddInput<T, RunTest>("x", shape1);
  testCase.AddInput<T, RunTest>("scale", shape2);
  testCase.AddInput<T, RunTest>("bias", shape2);
  testCase.AddOutput("y");
  testCase.AddOutput("mean");
  testCase.AddOutput("invstddev");
  testCase.AddAttribute("stash_type", data_types_internal::ToTensorDataType<U>());
  if (RunTest)
    testCase.RunTest();
  else
    testCase.CreateModel(true);
}

TEST(LayerNormExpansionTest, Test0) {
  RegisterSchemas();
  // Test expand-and-run
  CheckLayerNorm<float, float, true>();
  // Test expand-and-check-only
  CheckLayerNorm<MLFloat16, BFloat16, false>();
}

}  // namespace test
}  // namespace onnxruntime