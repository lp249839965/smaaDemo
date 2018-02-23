// Copyright (c) 2017 Google Inc.
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

#include "ir_context.h"
#include "latest_version_glsl_std_450_header.h"
#include "log.h"
#include "mem_pass.h"
#include "reflect.h"
#include "spirv/1.0/GLSL.std.450.h"

#include <cstring>

namespace spvtools {
namespace ir {

void IRContext::BuildInvalidAnalyses(IRContext::Analysis set) {
  if (set & kAnalysisDefUse) {
    BuildDefUseManager();
  }
  if (set & kAnalysisInstrToBlockMapping) {
    BuildInstrToBlockMapping();
  }
  if (set & kAnalysisDecorations) {
    BuildDecorationManager();
  }
  if (set & kAnalysisCFG) {
    BuildCFG();
  }
  if (set & kAnalysisDominatorAnalysis) {
    ResetDominatorAnalysis();
  }
}

void IRContext::InvalidateAnalysesExceptFor(
    IRContext::Analysis preserved_analyses) {
  uint32_t analyses_to_invalidate = valid_analyses_ & (~preserved_analyses);
  InvalidateAnalyses(static_cast<IRContext::Analysis>(analyses_to_invalidate));
}

void IRContext::InvalidateAnalyses(IRContext::Analysis analyses_to_invalidate) {
  if (analyses_to_invalidate & kAnalysisDefUse) {
    def_use_mgr_.reset(nullptr);
  }
  if (analyses_to_invalidate & kAnalysisInstrToBlockMapping) {
    instr_to_block_.clear();
  }
  if (analyses_to_invalidate & kAnalysisDecorations) {
    decoration_mgr_.reset(nullptr);
  }
  if (analyses_to_invalidate & kAnalysisCombinators) {
    combinator_ops_.clear();
  }
  if (analyses_to_invalidate & kAnalysisCFG) {
    cfg_.reset(nullptr);
  }
  if (analyses_to_invalidate & kAnalysisDominatorAnalysis) {
    dominator_trees_.clear();
    post_dominator_trees_.clear();
  }

  valid_analyses_ = Analysis(valid_analyses_ & ~analyses_to_invalidate);
}

Instruction* IRContext::KillInst(ir::Instruction* inst) {
  if (!inst) {
    return nullptr;
  }

  KillNamesAndDecorates(inst);

  if (AreAnalysesValid(kAnalysisDefUse)) {
    get_def_use_mgr()->ClearInst(inst);
  }
  if (AreAnalysesValid(kAnalysisInstrToBlockMapping)) {
    instr_to_block_.erase(inst);
  }
  if (AreAnalysesValid(kAnalysisDecorations)) {
    if (inst->result_id() != 0) {
      decoration_mgr_->RemoveDecorationsFrom(inst->result_id());
    }
    if (inst->IsDecoration()) {
      decoration_mgr_->RemoveDecoration(inst);
    }
  }

  if (type_mgr_ && ir::IsTypeInst(inst->opcode())) {
    type_mgr_->RemoveId(inst->result_id());
  }

  Instruction* next_instruction = nullptr;
  if (inst->IsInAList()) {
    next_instruction = inst->NextNode();
    inst->RemoveFromList();
    delete inst;
  } else {
    // Needed for instructions that are not part of a list like OpLabels,
    // OpFunction, OpFunctionEnd, etc..
    inst->ToNop();
  }
  return next_instruction;
}

bool IRContext::KillDef(uint32_t id) {
  ir::Instruction* def = get_def_use_mgr()->GetDef(id);
  if (def != nullptr) {
    KillInst(def);
    return true;
  }
  return false;
}

bool IRContext::ReplaceAllUsesWith(uint32_t before, uint32_t after) {
  if (before == after) return false;

  // Ensure that |after| has been registered as def.
  assert(get_def_use_mgr()->GetDef(after) &&
         "'after' is not a registered def.");

  std::vector<std::pair<ir::Instruction*, uint32_t>> uses_to_update;
  get_def_use_mgr()->ForEachUse(
      before, [&uses_to_update](ir::Instruction* user, uint32_t index) {
        uses_to_update.emplace_back(user, index);
      });

  ir::Instruction* prev = nullptr;
  for (auto p : uses_to_update) {
    ir::Instruction* user = p.first;
    uint32_t index = p.second;
    if (prev == nullptr || prev != user) {
      ForgetUses(user);
      prev = user;
    }
    const uint32_t type_result_id_count =
        (user->result_id() != 0) + (user->type_id() != 0);

    if (index < type_result_id_count) {
      // Update the type_id. Note that result id is immutable so it should
      // never be updated.
      if (user->type_id() != 0 && index == 0) {
        user->SetResultType(after);
      } else if (user->type_id() == 0) {
        SPIRV_ASSERT(consumer_, false,
                     "Result type id considered as use while the instruction "
                     "doesn't have a result type id.");
        (void)consumer_;  // Makes the compiler happy for release build.
      } else {
        SPIRV_ASSERT(consumer_, false,
                     "Trying setting the immutable result id.");
      }
    } else {
      // Update an in-operand.
      uint32_t in_operand_pos = index - type_result_id_count;
      // Make the modification in the instruction.
      user->SetInOperand(in_operand_pos, {after});
    }
    AnalyzeUses(user);
  };

  return true;
}

bool IRContext::IsConsistent() {
#ifndef SPIRV_CHECK_CONTEXT
  return true;
#endif

  if (AreAnalysesValid(kAnalysisDefUse)) {
    opt::analysis::DefUseManager new_def_use(module());
    if (*get_def_use_mgr() != new_def_use) {
      return false;
    }
  }
  return true;
}

void spvtools::ir::IRContext::ForgetUses(Instruction* inst) {
  if (AreAnalysesValid(kAnalysisDefUse)) {
    get_def_use_mgr()->EraseUseRecordsOfOperandIds(inst);
  }
  if (AreAnalysesValid(kAnalysisDecorations)) {
    if (inst->IsDecoration()) {
      get_decoration_mgr()->RemoveDecoration(inst);
    }
  }
}

void IRContext::AnalyzeUses(Instruction* inst) {
  if (AreAnalysesValid(kAnalysisDefUse)) {
    get_def_use_mgr()->AnalyzeInstUse(inst);
  }
  if (AreAnalysesValid(kAnalysisDecorations)) {
    if (inst->IsDecoration()) {
      get_decoration_mgr()->AddDecoration(inst);
    }
  }
}

void IRContext::KillNamesAndDecorates(uint32_t id) {
  std::vector<ir::Instruction*> decorations =
      get_decoration_mgr()->GetDecorationsFor(id, true);

  for (Instruction* inst : decorations) {
    KillInst(inst);
  }

  Instruction* debug_inst = &*debug2_begin();
  while (debug_inst) {
    bool killed_inst = false;
    if (debug_inst->opcode() == SpvOpMemberName ||
        debug_inst->opcode() == SpvOpName) {
      if (debug_inst->GetSingleWordInOperand(0) == id) {
        debug_inst = KillInst(debug_inst);
        killed_inst = true;
      }
    }
    if (!killed_inst) {
      debug_inst = debug_inst->NextNode();
    }
  }
}

void IRContext::KillNamesAndDecorates(Instruction* inst) {
  const uint32_t rId = inst->result_id();
  if (rId == 0) return;
  KillNamesAndDecorates(rId);
}

void IRContext::AddCombinatorsForCapability(uint32_t capability) {
  if (capability == SpvCapabilityShader) {
    combinator_ops_[0].insert({
        SpvOpNop,
        SpvOpUndef,
        SpvOpVariable,
        SpvOpImageTexelPointer,
        SpvOpLoad,
        SpvOpAccessChain,
        SpvOpInBoundsAccessChain,
        SpvOpArrayLength,
        SpvOpVectorExtractDynamic,
        SpvOpVectorInsertDynamic,
        SpvOpVectorShuffle,
        SpvOpCompositeConstruct,
        SpvOpCompositeExtract,
        SpvOpCompositeInsert,
        SpvOpCopyObject,
        SpvOpTranspose,
        SpvOpSampledImage,
        SpvOpImageSampleImplicitLod,
        SpvOpImageSampleExplicitLod,
        SpvOpImageSampleDrefImplicitLod,
        SpvOpImageSampleDrefExplicitLod,
        SpvOpImageSampleProjImplicitLod,
        SpvOpImageSampleProjExplicitLod,
        SpvOpImageSampleProjDrefImplicitLod,
        SpvOpImageSampleProjDrefExplicitLod,
        SpvOpImageFetch,
        SpvOpImageGather,
        SpvOpImageDrefGather,
        SpvOpImageRead,
        SpvOpImage,
        SpvOpConvertFToU,
        SpvOpConvertFToS,
        SpvOpConvertSToF,
        SpvOpConvertUToF,
        SpvOpUConvert,
        SpvOpSConvert,
        SpvOpFConvert,
        SpvOpQuantizeToF16,
        SpvOpBitcast,
        SpvOpSNegate,
        SpvOpFNegate,
        SpvOpIAdd,
        SpvOpFAdd,
        SpvOpISub,
        SpvOpFSub,
        SpvOpIMul,
        SpvOpFMul,
        SpvOpUDiv,
        SpvOpSDiv,
        SpvOpFDiv,
        SpvOpUMod,
        SpvOpSRem,
        SpvOpSMod,
        SpvOpFRem,
        SpvOpFMod,
        SpvOpVectorTimesScalar,
        SpvOpMatrixTimesScalar,
        SpvOpVectorTimesMatrix,
        SpvOpMatrixTimesVector,
        SpvOpMatrixTimesMatrix,
        SpvOpOuterProduct,
        SpvOpDot,
        SpvOpIAddCarry,
        SpvOpISubBorrow,
        SpvOpUMulExtended,
        SpvOpSMulExtended,
        SpvOpAny,
        SpvOpAll,
        SpvOpIsNan,
        SpvOpIsInf,
        SpvOpLogicalEqual,
        SpvOpLogicalNotEqual,
        SpvOpLogicalOr,
        SpvOpLogicalAnd,
        SpvOpLogicalNot,
        SpvOpSelect,
        SpvOpIEqual,
        SpvOpINotEqual,
        SpvOpUGreaterThan,
        SpvOpSGreaterThan,
        SpvOpUGreaterThanEqual,
        SpvOpSGreaterThanEqual,
        SpvOpULessThan,
        SpvOpSLessThan,
        SpvOpULessThanEqual,
        SpvOpSLessThanEqual,
        SpvOpFOrdEqual,
        SpvOpFUnordEqual,
        SpvOpFOrdNotEqual,
        SpvOpFUnordNotEqual,
        SpvOpFOrdLessThan,
        SpvOpFUnordLessThan,
        SpvOpFOrdGreaterThan,
        SpvOpFUnordGreaterThan,
        SpvOpFOrdLessThanEqual,
        SpvOpFUnordLessThanEqual,
        SpvOpFOrdGreaterThanEqual,
        SpvOpFUnordGreaterThanEqual,
        SpvOpShiftRightLogical,
        SpvOpShiftRightArithmetic,
        SpvOpShiftLeftLogical,
        SpvOpBitwiseOr,
        SpvOpBitwiseXor,
        SpvOpBitwiseAnd,
        SpvOpNot,
        SpvOpBitFieldInsert,
        SpvOpBitFieldSExtract,
        SpvOpBitFieldUExtract,
        SpvOpBitReverse,
        SpvOpBitCount,
        SpvOpPhi,
        SpvOpImageSparseSampleImplicitLod,
        SpvOpImageSparseSampleExplicitLod,
        SpvOpImageSparseSampleDrefImplicitLod,
        SpvOpImageSparseSampleDrefExplicitLod,
        SpvOpImageSparseSampleProjImplicitLod,
        SpvOpImageSparseSampleProjExplicitLod,
        SpvOpImageSparseSampleProjDrefImplicitLod,
        SpvOpImageSparseSampleProjDrefExplicitLod,
        SpvOpImageSparseFetch,
        SpvOpImageSparseGather,
        SpvOpImageSparseDrefGather,
        SpvOpImageSparseTexelsResident,
        SpvOpImageSparseRead,
        SpvOpSizeOf
        // TODO(dneto): Add instructions enabled by ImageQuery
    });
  }
}

void IRContext::AddCombinatorsForExtension(ir::Instruction* extension) {
  assert(extension->opcode() == SpvOpExtInstImport &&
         "Expecting an import of an extension's instruction set.");
  const char* extension_name =
      reinterpret_cast<const char*>(&extension->GetInOperand(0).words[0]);
  if (!strcmp(extension_name, "GLSL.std.450")) {
    combinator_ops_[extension->result_id()] = {GLSLstd450Round,
                                               GLSLstd450RoundEven,
                                               GLSLstd450Trunc,
                                               GLSLstd450FAbs,
                                               GLSLstd450SAbs,
                                               GLSLstd450FSign,
                                               GLSLstd450SSign,
                                               GLSLstd450Floor,
                                               GLSLstd450Ceil,
                                               GLSLstd450Fract,
                                               GLSLstd450Radians,
                                               GLSLstd450Degrees,
                                               GLSLstd450Sin,
                                               GLSLstd450Cos,
                                               GLSLstd450Tan,
                                               GLSLstd450Asin,
                                               GLSLstd450Acos,
                                               GLSLstd450Atan,
                                               GLSLstd450Sinh,
                                               GLSLstd450Cosh,
                                               GLSLstd450Tanh,
                                               GLSLstd450Asinh,
                                               GLSLstd450Acosh,
                                               GLSLstd450Atanh,
                                               GLSLstd450Atan2,
                                               GLSLstd450Pow,
                                               GLSLstd450Exp,
                                               GLSLstd450Log,
                                               GLSLstd450Exp2,
                                               GLSLstd450Log2,
                                               GLSLstd450Sqrt,
                                               GLSLstd450InverseSqrt,
                                               GLSLstd450Determinant,
                                               GLSLstd450MatrixInverse,
                                               GLSLstd450ModfStruct,
                                               GLSLstd450FMin,
                                               GLSLstd450UMin,
                                               GLSLstd450SMin,
                                               GLSLstd450FMax,
                                               GLSLstd450UMax,
                                               GLSLstd450SMax,
                                               GLSLstd450FClamp,
                                               GLSLstd450UClamp,
                                               GLSLstd450SClamp,
                                               GLSLstd450FMix,
                                               GLSLstd450IMix,
                                               GLSLstd450Step,
                                               GLSLstd450SmoothStep,
                                               GLSLstd450Fma,
                                               GLSLstd450FrexpStruct,
                                               GLSLstd450Ldexp,
                                               GLSLstd450PackSnorm4x8,
                                               GLSLstd450PackUnorm4x8,
                                               GLSLstd450PackSnorm2x16,
                                               GLSLstd450PackUnorm2x16,
                                               GLSLstd450PackHalf2x16,
                                               GLSLstd450PackDouble2x32,
                                               GLSLstd450UnpackSnorm2x16,
                                               GLSLstd450UnpackUnorm2x16,
                                               GLSLstd450UnpackHalf2x16,
                                               GLSLstd450UnpackSnorm4x8,
                                               GLSLstd450UnpackUnorm4x8,
                                               GLSLstd450UnpackDouble2x32,
                                               GLSLstd450Length,
                                               GLSLstd450Distance,
                                               GLSLstd450Cross,
                                               GLSLstd450Normalize,
                                               GLSLstd450FaceForward,
                                               GLSLstd450Reflect,
                                               GLSLstd450Refract,
                                               GLSLstd450FindILsb,
                                               GLSLstd450FindSMsb,
                                               GLSLstd450FindUMsb,
                                               GLSLstd450InterpolateAtCentroid,
                                               GLSLstd450InterpolateAtSample,
                                               GLSLstd450InterpolateAtOffset,
                                               GLSLstd450NMin,
                                               GLSLstd450NMax,
                                               GLSLstd450NClamp};
  } else {
    // Map the result id to the empty set.
    combinator_ops_[extension->result_id()];
  }
}

void IRContext::InitializeCombinators() {
  for (auto& capability : module()->capabilities()) {
    AddCombinatorsForCapability(capability.GetSingleWordInOperand(0));
  }

  for (auto& extension : module()->ext_inst_imports()) {
    AddCombinatorsForExtension(&extension);
  }

  valid_analyses_ |= kAnalysisCombinators;
}

// Gets the dominator analysis for function |f|.
opt::DominatorAnalysis* IRContext::GetDominatorAnalysis(const ir::Function* f,
                                                        const ir::CFG& in_cfg) {
  if (!AreAnalysesValid(kAnalysisDominatorAnalysis)) {
    ResetDominatorAnalysis();
  }

  if (dominator_trees_.find(f) == dominator_trees_.end()) {
    dominator_trees_[f].InitializeTree(f, in_cfg);
  }

  return &dominator_trees_[f];
}

// Gets the postdominator analysis for function |f|.
opt::PostDominatorAnalysis* IRContext::GetPostDominatorAnalysis(
    const ir::Function* f, const ir::CFG& in_cfg) {
  if (!AreAnalysesValid(kAnalysisDominatorAnalysis)) {
    ResetDominatorAnalysis();
  }

  if (post_dominator_trees_.find(f) == post_dominator_trees_.end()) {
    post_dominator_trees_[f].InitializeTree(f, in_cfg);
  }

  return &post_dominator_trees_[f];
}

}  // namespace ir
}  // namespace spvtools