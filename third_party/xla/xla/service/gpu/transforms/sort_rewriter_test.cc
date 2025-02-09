/* Copyright 2023 The OpenXLA Authors.

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

#include "xla/service/gpu/transforms/sort_rewriter.h"

#include <utility>

#include <gtest/gtest.h>
#include "xla/error_spec.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_module.h"
#include "xla/service/gpu/cublas_cudnn.h"
#include "xla/service/pattern_matcher.h"
#include "xla/service/pattern_matcher_gmock.h"
#include "xla/tests/hlo_test_base.h"
#include "xla/xla_data.pb.h"
#include "tsl/platform/statusor.h"
#include "tsl/platform/test.h"

namespace xla {
namespace gpu {
namespace {

namespace m = ::xla::match;

class SortRewriterTest : public HloTestBase {
 public:
  void SetUp() override {
    HloTestBase::SetUp();
    SortRewriter::SetSortSizeThresholdForTestingOnly(1000);
  }

  bool RunModuleAndPass(HloModule* module) {
    auto cloned = module->Clone();
    bool changed = SortRewriter().Run(module).value();
    if (changed) {
      // Here we run an end to end test to make sure that SortRewriter does
      // not introduce an incorrect rewrite. To do this, we need to clone the
      // original module because the interpreter cannot process the already
      // optimized module.
      EXPECT_TRUE(RunAndCompare(std::move(cloned), ErrorSpec{0, 0}));
    }
    return changed;
  }

  void ExpectDirection(const HloInstruction* instruction, bool descending) {
    auto config = instruction->backend_config<xla::SortOptions>();
    EXPECT_EQ(config->descending(), descending);
  }
};

// Basic sort: ascending.
TEST_F(SortRewriterTest, SortKeysLessThan) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs = f32[] parameter(0)
  %rhs = f32[] parameter(1)
  ROOT %lt = pred[] compare(%lhs, %rhs), direction=LT
}

ENTRY %main {
  %input = f32[1000] parameter(0)
  ROOT %sort = f32[1000] sort(%input), dimensions={0}, to_apply=%compare
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_TRUE(RunModuleAndPass(module.get()));
  EXPECT_THAT(
      module->entry_computation()->root_instruction(),
      GmockMatch(m::GetTupleElement(
          m::CustomCall({kCubDeviceRadixSortTarget}, m::Parameter()), 0)));
  ExpectDirection(module->entry_computation()->root_instruction()->operand(0),
                  /*descending=*/false);
}

// Basic sort: descending.
TEST_F(SortRewriterTest, SortKeysGreaterThan) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs = f32[] parameter(0)
  %rhs = f32[] parameter(1)
  ROOT %gt = pred[] compare(%lhs, %rhs), direction=GT
}

ENTRY %main {
  %input = f32[1000] parameter(0)
  ROOT %sort = f32[1000] sort(%input), dimensions={0}, to_apply=%compare
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_TRUE(RunModuleAndPass(module.get()));
  EXPECT_THAT(
      module->entry_computation()->root_instruction(),
      GmockMatch(m::GetTupleElement(
          m::CustomCall({kCubDeviceRadixSortTarget}, m::Parameter()), 0)));
  ExpectDirection(module->entry_computation()->root_instruction()->operand(0),
                  /*descending=*/true);
}

// Comparer swaps the parameter order -> direction is reversed.
TEST_F(SortRewriterTest, SortKeysGreaterThanSwapped) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs = f32[] parameter(1)
  %rhs = f32[] parameter(0)
  ROOT %gt = pred[] compare(%lhs, %rhs), direction=GT
}

ENTRY %main {
  %input = f32[1000] parameter(0)
  ROOT %sort = f32[1000] sort(%input), dimensions={0}, to_apply=%compare
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_TRUE(RunModuleAndPass(module.get()));
  EXPECT_THAT(
      module->entry_computation()->root_instruction(),
      GmockMatch(m::GetTupleElement(
          m::CustomCall({kCubDeviceRadixSortTarget}, m::Parameter()), 0)));
  ExpectDirection(module->entry_computation()->root_instruction()->operand(0),
                  /*descending=*/false);
}

// Sort a pair of tensors, keys go first.
TEST_F(SortRewriterTest, SortPairs) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs_key = u32[] parameter(0)
  %rhs_key = u32[] parameter(1)
  %lhs_value = f32[] parameter(2)
  %rhs_value = f32[] parameter(3)
  ROOT %lt = pred[] compare(%lhs_key, %rhs_key), direction=LT
}

ENTRY %main {
  %input_keys = u32[1000] parameter(0)
  %input_values = f32[1000] parameter(1)
  ROOT %sort = (u32[1000], f32[1000]) sort(%input_keys, %input_values),
      dimensions={0}, to_apply=%compare
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_TRUE(RunModuleAndPass(module.get()));
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Tuple(m::GetTupleElement(m::CustomCall(), 0),
                                  m::GetTupleElement(m::CustomCall(), 1))));
}

// Sort a pair of tensors, keys go first. Same as SortPairs, but because the
// `input_values` operand is unused, we can simplify it and sort only
// `input_keys`.
TEST_F(SortRewriterTest, SortPairsCanBeRewrittenToCubCallIfValuesAreIgnored) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs_key = u32[] parameter(0)
  %rhs_key = u32[] parameter(1)
  %lhs_value = f32[] parameter(2)
  %rhs_value = f32[] parameter(3)
  ROOT %lt = pred[] compare(%lhs_key, %rhs_key), direction=LT
}

ENTRY %main {
  %input_keys = u32[1000] parameter(0)
  %input_values = f32[1000] parameter(1)
  %sort = (u32[1000], f32[1000]) sort(%input_keys, %input_values),
      dimensions={0}, to_apply=%compare
  ROOT %gte = u32[1000] get-tuple-element(%sort), index=0
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_TRUE(RunModuleAndPass(module.get()));
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::GetTupleElement(m::CustomCall(), 0)));
}

// Sort a pair of tensors, keys go last.
TEST_F(SortRewriterTest, SortPairsSwapped) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs_value = f32[] parameter(0)
  %rhs_value = f32[] parameter(1)
  %lhs_key = u32[] parameter(2)
  %rhs_key = u32[] parameter(3)
  ROOT %lt = pred[] compare(%lhs_key, %rhs_key), direction=LT
}

ENTRY %main {
  %input_values = f32[1000] parameter(0)
  %input_keys = u32[1000] parameter(1)
  ROOT %sort = (f32[1000], u32[1000]) sort(%input_values, %input_keys),
      dimensions={0}, to_apply=%compare
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_TRUE(RunModuleAndPass(module.get()));
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Tuple(m::GetTupleElement(m::CustomCall(), 1),
                                  m::GetTupleElement(m::CustomCall(), 0))));
}

// Sort a pair of tensors, keys go last. Almost same as SortPairsSwapped (but
// using f16 for `input_keys` for which we have no support as keys for
// SortPairs). Because the `input_values` operand is unused here, we can
// simplify it and sort only `input_keys`.
TEST_F(SortRewriterTest,
       SortPairsSwappedCanBeRewrittenToCubCallIfValuesAreIgnored) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs_value = f32[] parameter(0)
  %rhs_value = f32[] parameter(1)
  %lhs_key = f16[] parameter(2)
  %rhs_key = f16[] parameter(3)
  ROOT %lt = pred[] compare(%lhs_key, %rhs_key), direction=LT
}

ENTRY %main {
  %input_values = f32[1000] parameter(0)
  %input_keys = f16[1000] parameter(1)
  %sort = (f32[1000], f16[1000]) sort(%input_values, %input_keys),
      dimensions={0}, to_apply=%compare
  ROOT %gte = f16[1000] get-tuple-element(%sort), index=1
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_TRUE(RunModuleAndPass(module.get()));
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::GetTupleElement(m::CustomCall(), 0)));
}

// CUB sort doesn't support more than two tensors.
TEST_F(SortRewriterTest, NoRewriteManyTensors) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs = f32[] parameter(0)
  %rhs = f32[] parameter(1)
  %unused1 = f64[] parameter(2)
  %unused2 = f64[] parameter(3)
  %unused3 = u64[] parameter(4)
  %unused4 = u64[] parameter(5)
  ROOT %lt = pred[] compare(%lhs, %rhs), direction=LT
}

ENTRY %main {
  %input1 = f32[1000] parameter(0)
  %input2 = f64[1000] parameter(1)
  %input3 = u64[1000] parameter(2)
  ROOT %sort = (f32[1000], f64[1000], u64[1000]) sort(%input1, %input2, %input3),
      dimensions={0}, to_apply=%compare
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_FALSE(RunModuleAndPass(module.get()));
}

// Only 1D shapes are supported.
TEST_F(SortRewriterTest, NoRewriteNonMinorSortDimension) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs = f32[] parameter(0)
  %rhs = f32[] parameter(1)
  ROOT %lt = pred[] compare(%lhs, %rhs), direction=LT
}

ENTRY %main {
  %input = f32[1000,4] parameter(0)
  ROOT %sort = f32[1000,4] sort(%input), dimensions={0}, to_apply=%compare
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_FALSE(RunModuleAndPass(module.get()));
}

// Kernels are compiled for a subset of types.
TEST_F(SortRewriterTest, NoRewriteUnsupportedType) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs = pred[] parameter(0)
  %rhs = pred[] parameter(1)
  ROOT %lt = pred[] compare(%lhs, %rhs), direction=LT
}

ENTRY %main {
  %input = pred[1000] parameter(0)
  ROOT %sort = pred[1000] sort(%input), dimensions={0}, to_apply=%compare
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_FALSE(RunModuleAndPass(module.get()));
}

// Comparer must be a simple function.
TEST_F(SortRewriterTest, NoRewriteComplexComparer) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs = f32[] parameter(0)
  %lhs_scaled = f32[] multiply(%lhs, f32[] constant(2))
  %rhs = f32[] parameter(1)
  ROOT %lt = pred[] compare(%lhs_scaled, %rhs), direction=LT
}

ENTRY %main {
  %input = f32[1000] parameter(0)
  ROOT %sort = f32[1000] sort(%input), dimensions={0}, to_apply=%compare
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_FALSE(RunModuleAndPass(module.get()));
}

// Comparer must use adjacent input values.
TEST_F(SortRewriterTest, NoRewriteMixedKeysValues) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs_key = u32[] parameter(0)
  %rhs_key = u32[] parameter(1)
  %lhs_value = u32[] parameter(2)
  %rhs_value = u32[] parameter(3)
  ROOT %mixed = pred[] compare(%rhs_key, %lhs_value), direction=LT
}

ENTRY %main {
  %input_keys = u32[1000] parameter(0)
  %input_values = u32[1000] parameter(1)
  ROOT %sort = (u32[1000], u32[1000]) sort(%input_keys, %input_values),
      dimensions={0}, to_apply=%compare
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_FALSE(RunModuleAndPass(module.get()));
}

// Small shapes do not see improvement from CUB sort.
TEST_F(SortRewriterTest, NoRewriteSmallSize) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs = f32[] parameter(0)
  %rhs = f32[] parameter(1)
  ROOT %lt = pred[] compare(%lhs, %rhs), direction=LT
}

ENTRY %main {
  %input = f32[100] parameter(0)
  ROOT %sort = f32[100] sort(%input), dimensions={0}, to_apply=%compare
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_FALSE(RunModuleAndPass(module.get()));
}

// Basic sort: with batch dimension.
TEST_F(SortRewriterTest, SortWithBatchDim) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs = f32[] parameter(0)
  %rhs = f32[] parameter(1)
  ROOT %lt = pred[] compare(%lhs, %rhs), direction=LT
}

ENTRY %main {
  %input = f32[10,100] parameter(0)
  ROOT %sort = f32[10,100] sort(%input), dimensions={1}, to_apply=%compare
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_TRUE(RunModuleAndPass(module.get()));
  EXPECT_THAT(
      module->entry_computation()->root_instruction(),
      GmockMatch(m::GetTupleElement(
          m::CustomCall({kCubDeviceRadixSortTarget}, m::Parameter()), 0)));
  ExpectDirection(module->entry_computation()->root_instruction()->operand(0),
                  /*descending=*/false);
}

// Basic sort: with multiple batch dimensions.
TEST_F(SortRewriterTest, SortWithMultipleBatchDims) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs = f32[] parameter(0)
  %rhs = f32[] parameter(1)
  ROOT %lt = pred[] compare(%lhs, %rhs), direction=LT
}

ENTRY %main {
  %input = f32[10,10,10] parameter(0)
  ROOT %sort = f32[10,10,10] sort(%input), dimensions={2}, to_apply=%compare
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_TRUE(RunModuleAndPass(module.get()));
  EXPECT_THAT(
      module->entry_computation()->root_instruction(),
      GmockMatch(m::GetTupleElement(
          m::CustomCall({kCubDeviceRadixSortTarget}, m::Parameter()), 0)));
  ExpectDirection(module->entry_computation()->root_instruction()->operand(0),
                  /*descending=*/false);
}

// Sort a pair of tensors (values, indices generated by iota) with a complex
// compare.
TEST_F(SortRewriterTest, SortPairsIotaComparerSimple) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs = u16[] parameter(0)
  %rhs = u16[] parameter(1)
  %lhs_index = s32[] parameter(2)
  %rhs_index = s32[] parameter(3)

  cmp_indices = pred[] compare(%lhs_index, %rhs_index), direction=LT
  cmp_lr = pred[] compare(%lhs, %rhs), direction=GT
  cmp_eq = pred[] compare(%lhs, %rhs), direction=EQ

  ROOT %lt = pred[] select(cmp_eq, cmp_indices, cmp_lr)
}

ENTRY %main {
  %inputs = u16[1000] parameter(0)
  %iota = s32[1000] iota(), iota_dimension=0
  ROOT %sort = (u16[1000], s32[1000]) sort(%inputs, %iota),
      dimensions={0}, to_apply=%compare
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_TRUE(RunModuleAndPass(module.get()));
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Tuple(m::GetTupleElement(m::CustomCall(), 0),
                                  m::GetTupleElement(m::CustomCall(), 1))));
}

// Sort a pair of tensors (values, indices generated by iota) with a complex
// compare. Same as SortPairsIotaComparerSimple, but because the `iota`
// operand is unused, we can simplify it and sort only `input_keys`.
TEST_F(SortRewriterTest,
       SortPairsIotaComparerSimpleCanBeRewrittenToCubCallIfValuesAreIgnored) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs = u16[] parameter(0)
  %rhs = u16[] parameter(1)
  %lhs_index = s32[] parameter(2)
  %rhs_index = s32[] parameter(3)

  cmp_indices = pred[] compare(%lhs_index, %rhs_index), direction=LT
  cmp_lr = pred[] compare(%lhs, %rhs), direction=GT
  cmp_eq = pred[] compare(%lhs, %rhs), direction=EQ

  ROOT %lt = pred[] select(cmp_eq, cmp_indices, cmp_lr)
}

ENTRY %main {
  %inputs = u16[1000] parameter(0)
  %iota = s32[1000] iota(), iota_dimension=0
  %sort = (u16[1000], s32[1000]) sort(%inputs, %iota),
      dimensions={0}, to_apply=%compare
  ROOT %gte = u16[1000] get-tuple-element(%sort), index=0
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_TRUE(RunModuleAndPass(module.get()));
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::GetTupleElement(m::CustomCall(), 0)));
}

TEST_F(SortRewriterTest, SortPairsIotaComparerSimplePreservesMetadata) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs = u16[] parameter(0)
  %rhs = u16[] parameter(1)
  %lhs_index = s32[] parameter(2)
  %rhs_index = s32[] parameter(3)

  cmp_indices = pred[] compare(%lhs_index, %rhs_index), direction=LT
  cmp_lr = pred[] compare(%lhs, %rhs), direction=GT
  cmp_eq = pred[] compare(%lhs, %rhs), direction=EQ

  ROOT %lt = pred[] select(cmp_eq, cmp_indices, cmp_lr)
}

ENTRY %main {
  %inputs = u16[1000] parameter(0)
  %iota = s32[1000] iota(), iota_dimension=0
  ROOT %sort = (u16[1000], s32[1000]) sort(%inputs, %iota),
      dimensions={0}, to_apply=%compare, metadata={op_type="sort" op_name="sort" source_file="path/to/test.cc" source_line=68}
})";
  constexpr char kExpectedPattern[] = R"(
    // CHECK: %[[CC:.*]] = (u16[1000]{0}, s32[1000]{0}, u8[1]{0}) custom-call({{.*}}), custom_call_target="__cub$DeviceRadixSort", metadata={op_type="sort" op_name="sort" source_file="path/to/test.cc" source_line=68}, backend_config={"descending":true}
    // CHECK: %[[GTE0:.*]] = u16[1000]{0} get-tuple-element(%[[CC]]), index=0, metadata={op_type="sort" op_name="sort" source_file="path/to/test.cc" source_line=68}
    // CHECK: %[[GTE1:.*]] = s32[1000]{0} get-tuple-element(%[[CC]]), index=1, metadata={op_type="sort" op_name="sort" source_file="path/to/test.cc" source_line=68}
    // CHECK: ROOT %{{.*}} = (u16[1000]{0}, s32[1000]{0}) tuple(%[[GTE0]], %[[GTE1]]), metadata={op_type="sort" op_name="sort" source_file="path/to/test.cc" source_line=68}
  )";
  RunAndFilecheckHloRewrite(kHlo, SortRewriter(), kExpectedPattern);
}

// Sort a pair of tensors (values, indices generated by iota) with a complex
// compare computation that matches the output of the StableSortExpander pass.
TEST_F(SortRewriterTest, SortPairsIotaComparerLikeStableSortExpander) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs = u16[] parameter(0)
  %rhs = u16[] parameter(1)
  %lhs_index = s32[] parameter(2)
  %rhs_index = s32[] parameter(3)

  cmp_indices = pred[] compare(%lhs_index, %rhs_index), direction=LT
  cmp_lr = pred[] compare(%lhs, %rhs), direction=GT
  cmp_rl = pred[] compare(%rhs, %lhs), direction=GT
  cmp_eq = pred[] compare(cmp_lr, cmp_rl), direction=EQ

  ROOT %lt = pred[] select(cmp_eq, cmp_indices, cmp_lr)
}

ENTRY %main {
  %inputs = u16[1000] parameter(0)
  %iota = s32[1000] iota(), iota_dimension=0
  ROOT %sort = (u16[1000], s32[1000]) sort(%inputs, %iota),
      dimensions={0}, to_apply=%compare
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_TRUE(RunModuleAndPass(module.get()));
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Tuple(m::GetTupleElement(m::CustomCall(), 0),
                                  m::GetTupleElement(m::CustomCall(), 1))));
}

// Make sure we don't match if the comparator is a stable sort, but the values
// operand is not iota.
TEST_F(SortRewriterTest, SortPairsComparerLikeStableSortExpanderNoIota) {
  constexpr char kHlo[] = R"(
HloModule TestModule

%compare {
  %lhs = u16[] parameter(0)
  %rhs = u16[] parameter(1)
  %lhs_index = s32[] parameter(2)
  %rhs_index = s32[] parameter(3)

  cmp_indices = pred[] compare(%lhs_index, %rhs_index), direction=LT
  cmp_lr = pred[] compare(%lhs, %rhs), direction=GT
  cmp_rl = pred[] compare(%rhs, %lhs), direction=GT
  cmp_eq = pred[] compare(cmp_lr, cmp_rl), direction=EQ

  ROOT %lt = pred[] select(cmp_eq, cmp_indices, cmp_lr)
}

ENTRY %main {
  %inputs = u16[1000] parameter(0)
  %values = s32[1000] parameter(1)
  ROOT %sort = (u16[1000], s32[1000]) sort(%inputs, %values),
      dimensions={0}, to_apply=%compare
})";

  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(kHlo));
  EXPECT_FALSE(RunModuleAndPass(module.get()));
}

TEST_F(SortRewriterTest, SortSizeThresholdIsSet) {
  EXPECT_EQ(SortRewriter::SortSizeThreshold(), 1000);
}

}  // namespace
}  // namespace gpu
}  // namespace xla
