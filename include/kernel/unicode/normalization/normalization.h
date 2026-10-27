/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <pablo/toolchain/pablo_toolchain.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <ucd/utf/utf_encoder.h>
#include <ucd/utf/transchar.h>
#include <pablo/pablo.h>
#include <ucd/algo/normalization.h>
#include <kernel/streamutils/focused_transform.h>
#include <memory>

using StreamSet = kernel::StreamSet;
using PipelineBuilder = kernel::PipelineBuilder;
namespace re {class CC;}
namespace pablo {class PabloBuilder; class PabloAST; class Var;}
namespace llvm { namespace cl { class OptionCategory; } }

// Hangul Composition Kernels for NFC (See Unicode section 3.12).
//
//  Hangul composables are individual Hangul L, V and T characters that
//  can be combined to produce precomposed LV or LVT combinations.   Note
//  also that LV combinations are composables, as they can potentially be
//  combined with trailing T character.
//  The Hangul_Composables kernel returns a set of 4 streams respectively
//  marking composable L, V, LV and T characters given a set of basis
//  streams.
//
//  This kernel may be used with a basis set of 21 bit streams for
//  Unicode-indexed data, 16 bit streams for UTF-16 indexed data or
//  of 8 bit streams for UTF-8 indexed data.   For UTF-8, calculations
//  are produced at the position of the first code unit with LookAhead
//  type bit movement (the default).

class Hangul_Composables : public pablo::PabloKernel {
public:
    enum HC_Kind : unsigned {L, V, LV, T, Count};

    Hangul_Composables(LLVMTypeSystemInterface & ts,
                       StreamSet * Basis, StreamSet * L_V_T_Composables,
                       pablo::BitMovementMode m = pablo::BitMovementMode::LookAhead);
protected:
    void generatePabloMethod() override;
private:
    pablo::BitMovementMode mBitMovement;
};

//
//  Transform sequences of Hangul composable characters into the equivalent
//  precomposed characters.  Three character L-V-T sequences are
//  transformed into the corresponding precomposed character at the V
//  position.   A sequence consisting of an LV precomposed character
//  followed by a T is transformed int the combined LVT character at
//  the LV position.   Finally, in the case of a two-character L-V
//  sequence not followed by a T, a transformed LV character is produced
//  at the LV position.
//
//  As a result of this transformation, leading L and trailing T
//  characters become redundant.   This is reflected by zeroing out
//  such positions in the produced output basis bit streams.
//
//  This kernel may be used with basis stream sets having 21 streams
//  (Unicode indexing), 16 streams (UTF-16 indexing) or 8 streams
//  (UTF-8 indexing).  In the case of UTF-8 indexing, this kernel
//  requires Hangul that the Hangul Composables stream be produced
//  at the first byte position of each such composable character.
//
class Hangul_Composition : public pablo::PabloKernel {
public:
    Hangul_Composition(LLVMTypeSystemInterface & ts,
                       StreamSet * Basis, StreamSet * Hangul_Composables,
                       StreamSet * Output_Basis);
protected:
    void generatePabloMethod() override;
};

//  The NFC_CandidateClass kernel produces the class of characters
//  that are relevant to NFC processing by virtue of being reorderable marks or
//  non-reorderable characters that can occur as the second character of a
//  composable sequence.
//
class NFC_CandidateClass : public pablo::PabloKernel {
public:
NFC_CandidateClass
    (LLVMTypeSystemInterface & ts, StreamSet * Basis,
                                   StreamSet * NFC_CandidateClass);
protected:
    void generatePabloMethod() override;
};

//
//  Although NFC generally compresses text by replacing decomposed
//  character sequences by their precomposed equivalents, there are
//  two cases where expansion is required.   The first is that any
//  precomposed characters with non-starter decompositions must be
//  decomposed and left in decomposed form.   The second is that
//  when the UTF-8 length of a precomposed character is longer than
//  the starter of its decomposed sequence.   The NFC_Initial_Insertion
//  kernel produces an insertion bixnum that indicates the maximum
//  insertion required at each position.
//
class NFC_Initial_Insertion : public pablo::PabloKernel {
public:
    NFC_Initial_Insertion
        (LLVMTypeSystemInterface & ts, StreamSet * Basis,
                                       StreamSet * InsertionBixNum,
                                       StreamSet * WorkMask = nullptr);
protected:
    void generatePabloMethod() override;
    bool mHasWorkMask;
};

//
//  Apply the logic of nonstarter decomposition to any relevant
//  precomposed characters.   This kernel assumes that the Basis
//  bit streams have had sufficient zeroes inserted for the decomposed
//  sequence.
//
class NonStarterDecomposition : public pablo::PabloKernel {
public:
    NonStarterDecomposition
        (LLVMTypeSystemInterface & ts, StreamSet * Basis,
                                       StreamSet * NSD_Basis);
protected:
    void generatePabloMethod() override;
};

//
//  Excluded composites are those that are always normalized to
//  decomposed form with both NFD and NFC.
//
class ExcludedCompositeStage : public pablo::PabloKernel {
public:
    ExcludedCompositeStage
        (LLVMTypeSystemInterface & ts, StreamSet * Basis, StreamSet * EC_Basis);
protected:
    void generatePabloMethod() override;
};

//
//  Transform any characters with a singleton decomposition to their
//  canonical form.   In the case of UTF-8, if the singleton decomposition
//  produces a shorter transformed sequence, the extra positions will be
//  marked with zeroes for deletion using FilterByMask.
//
class SingletonCanonicalization : public pablo::PabloKernel {
public:
    SingletonCanonicalization
        (LLVMTypeSystemInterface & ts, StreamSet * Basis, StreamSet * XfrmBasis);
protected:
    void generatePabloMethod() override;
};

//
//  Self-Composable Logic
//  Given Unicode characters AA and A such that AA has a canonical
//  decomposition AA ==> [A, A], the character A is called a
//  self-composable, while the character AA is called a doubleton.
//
//  Two sets of such characters are:
//  0x16121 => [0x1611e, 0x1611e]
//  0x16d68 => [0x16d67, 0x16d67]
//
//  Conversion of sequences of self-composables and doubletons to NFC form
//  include the following examples.
//
//  A AA ==> AA A
//  A A A ==> AA A
//  A AA A ==> AA AA
//  A A AA A  ==> AA AA A
//

class SelfComposableCCs : public pablo::PabloKernel {
public:
    SelfComposableCCs
        (LLVMTypeSystemInterface & ts, StreamSet * Basis, StreamSet * short_composable_CCs);
protected:
    void generatePabloMethod() override;
};

class SelfComposableTranslation : public pablo::PabloKernel {
public:
    SelfComposableTranslation
        (LLVMTypeSystemInterface & ts, StreamSet * Basis, StreamSet * XfrmedBasis);
protected:
    void generatePabloMethod() override;
};

struct SCResults {
    pablo::PabloAST * A_to_convert_to_AA;
    pablo::PabloAST * A_or_AA_to_delete;
    pablo::PabloAST * AA_to_convert_to_A;
};

SCResults SelfComposableLogic
             (pablo::PabloBuilder & pb,
              std::vector<pablo::PabloAST *> Basis,
              unsigned A_len, unsigned AA_len,
              pablo::PabloAST * A, pablo::PabloAST * AA,
              pablo::PabloAST * A_ahead, pablo::PabloAST * AA_ahead);

//
//  Short composable sequences are those involving non reorderable
//  characters.   In this case, precomposition is only applied when
//  the characters are adjacent.   This kernel replaces the
//  first character of such short composable sequences with
//  the resulting precomposed character and zeroes out the
//  second.
//
void ShortComposablePipeline(PipelineBuilder & P,
                            StreamSet * Basis, StreamSet * FinalBasis);

void LongComposablePipeline(PipelineBuilder & P,
                            StreamSet * Basis, StreamSet * ccc_NR,
                            StreamSet * FinalBasis);
//
//  Compute a mask for final work placement, given
//  (a) a set of ccs that define the insertion amounts (as a BixNum)
//      required for the work to be carried out,
//  (b) a set of ccs that define the deletion amounts (as a BixNum)
//      required to remove unneeeded positiona after work has been
//      performed,
//  (c) a source UTF-8 basis bits stream,
//  (d) a mask to select only those portions of the basis that are
//      relevant to the work to be performed.

//  The insertion and deletion amounts are computed at the last byte of each UTF-8
//  sequence (Advance) or at its first byte (LookAhead).
void ComputeWorkPlacement(PipelineBuilder & P,
                          std::vector<re::CC *> insertionBixNumCCs,
                          std::vector<re::CC *> deletionBixNumCCs,
                          StreamSet * U8_Basis, StreamSet * WorkSelectionMask,
                          StreamSet * WorkPlacementMask,
                          pablo::BitMovementMode m = pablo::BitMovementMode::Advance);

struct BitXfrmSpec {
    unsigned BitXfrmIndex;
    unsigned position;
    unsigned bit;
};

void UpdateBitXfrms(pablo::PabloBuilder & pb,
                    std::vector<pablo::Var *> BitXfrmBasis,
                    pablo::PabloAST * marker,
                    std::vector<pablo::PabloAST *> & sets,
                    std::vector<BitXfrmSpec> & xfrmSpecs);

void UpdateBitXfrms(pablo::PabloBuilder & pb,
                    std::vector<pablo::PabloAST *> BitXfrmBasis,
                    pablo::PabloAST * marker,
                    std::vector<pablo::PabloAST *> & sets,
                    std::vector<BitXfrmSpec> & xfrmSpecs);

//
//  FCD (as ICU's FCD normalizer and the Any-FCD transform): the text is left
//  as it is where it passes the FCD test, and elsewhere decomposed (NFD) in
//  regions.  A violation is a character v with leading canonical combining class
//  lccc(v) != 0 below the trailing class tccc of the character before it (the
//  classes of the first and last characters of their NFD).  Its region begins
//  at the last character before it with lccc = 0, or after the last one with
//  tccc <= 1 if later, and ends before the next character with lccc = 0.
//  FCD_RegionMask marks the regions of a U21-indexed text; the regions,
//  decomposed with NFD (with the other characters as barriers), give the FCD.
//
void FCD_RegionMask(PipelineBuilder & P, StreamSet * U21, StreamSet * Regions);

// NFC Support
//
//  NFC as a focused transform, with placement after the transform: the
//  transform decides which positions of its working space remain.

// The focus of NFC: the UTF-8 sequences that may change (composable sequences
// and excluded composites).
void DetermineNFC_WorkSelection(PipelineBuilder & P, StreamSet * U8_Basis, StreamSet * WorkSelectionMask);

// NFC of the UTF-8 basis bits U8_Basis.  The transform works in a working space,
// U8_Basis with room inserted for decompositions; ValidWorkMask marks the positions
// of the working space that remain in TransformedBasis.  Given a WorkMask, room is
// inserted only within the work regions (for transforming a whole text).
void NFC_Transform(PipelineBuilder & P, StreamSet * U8_Basis, StreamSet * TransformedBasis,
                   StreamSet * ValidWorkMask, StreamSet * WorkMask = nullptr);

// The placement of the transformed work regions in the output, given the
// ValidWorkMask from NFC_Transform of the work regions.
void NFC_WorkPlacement(PipelineBuilder & P, StreamSet * U8_Basis, StreamSet * WorkSelectionMask,
                       StreamSet * ValidWorkMask, StreamSet * WorkPlacementMask);

// NFD Support

// The data of the UTF-8 placement of NFD (or, with UCD::DecompositionOptions::NFKD,
// of NFKD).
class NFD_BixData {
public:
    NFD_BixData(UCD::DecompositionOptions opt = UCD::DecompositionOptions::NFD);
    // The characters that the decomposition changes.
    re::CC * Decomposable_CC();
    std::vector<re::CC *> NFD_Insertion_BixNumCCs();
    std::vector<re::CC *> UTF8_Insertion_BixNumCCs();
    std::vector<re::CC *> UTF8_Deletion_BixNumCCs();
    unicode::BitTranslationSets NFD_1st_BitXorCCs();
    unicode::BitTranslationSets NFD_2nd_BitCCs();
    unicode::BitTranslationSets NFD_3rd_BitCCs();
    unicode::BitTranslationSets NFD_4th_BitCCs();
private:
    UTF_Encoder mU8_encoder;
    std::unordered_map<codepoint_t, unsigned> mNFD_expansion;
    std::unordered_map<codepoint_t, unsigned> mUTF8_expansion;
    std::unordered_map<codepoint_t, unsigned> mUTF8_deletion;
    unicode::TranslationMap mNFD_CharMap[4];
    UCD::UnicodeSet mHangul_Precomposed_LV;
    UCD::UnicodeSet mHangul_Precomposed_LVT;
    UCD::UnicodeSet mDecomposable;
};

// The category of the normalization command line options (-u8-bit-movement), for
// codegen::ParseCommandLineOptions.
llvm::cl::OptionCategory & NormalizationOptions();

class NFD_PipelineBuilder {
public:
    // The marks of the UTF-8 kernels are placed as given by -u8-bit-movement.
    NFD_PipelineBuilder(PipelineBuilder & P);

    // Mark the last byte (Advance) or the first byte (LookAhead) of each UTF-8 sequence.
    void setBitMovement(pablo::BitMovementMode m) { mBitMovement = m; }

    StreamSet * NFD_U21_Pipeline(StreamSet * U21_Basis);

    StreamSet * NFKD_U21_Pipeline(StreamSet * U21_Basis);

    void DetermineNFD_WorkItems(StreamSet * U8_Basis, StreamSet * u8index, StreamSet * workItems);

    // The focus of NFD: the UTF-8 sequences that may change.
    void DetermineNFD_WorkSelection(StreamSet * BasisBits, StreamSet * WorkSelectionMask);

    // DetermineNFD_WorkSelection, with gap filling, then FilterByMask of the basis bits.
    void NFD_FilterStage(StreamSet * BasisBits, StreamSet * WorkSelectionMask, StreamSet * WorkingBasis);

    // Fill gaps of at most this many unchanged bytes between work regions (0: none).
    void setFillRunGaps(unsigned maxGap) { mFillRunGaps = maxGap; }

    void ComputeWorkPlacementMask(StreamSet * BasisBits, StreamSet * WorkSelectionMask, StreamSet * FinalWorkPlacementMask);

    void NFD_U8_Pipeline(StreamSet * WorkingBasis, StreamSet * TransformedBasis);

    void NFKD_U8_Pipeline(StreamSet * WorkingBasis, StreamSet * TransformedBasis);

    // The focus of NFKD: the characters with a decomposition (canonical or
    // compatibility) or a nonzero combining class, and the character after each.
    void DetermineNFKD_WorkSelection(StreamSet * BasisBits, StreamSet * WorkSelectionMask);

    void ComputeNFKD_WorkPlacementMask(StreamSet * BasisBits, StreamSet * WorkSelectionMask, StreamSet * FinalWorkPlacementMask);

private:
    NFD_BixData & NFKD_Data();

    unsigned mFillRunGaps = 0;
    pablo::BitMovementMode mBitMovement;
    PipelineBuilder & mPB;
    NFD_BixData NFD_Data;
    std::unique_ptr<NFD_BixData> mNFKD_Data;
};

//  The normalization forms as focused transforms (see kernel/streamutils/focused_transform.h),
//  built with the stages above.  The NFD and NFKD transforms use the given
//  NFD_PipelineBuilder, which must outlive them.
kernel::FocusedTransform NFD_FocusedTransform(NFD_PipelineBuilder & builder, const kernel::FocusOptions & options);
kernel::FocusedTransform NFKD_FocusedTransform(NFD_PipelineBuilder & builder, const kernel::FocusOptions & options);
kernel::FocusedTransform NFC_FocusedTransform(const kernel::FocusOptions & options);
