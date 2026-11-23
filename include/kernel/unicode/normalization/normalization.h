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

//  What NFC (and NFKC) may assume of its input (within the mask, if any: each
//  run of it).  With InputIsNFD the input is in NFD; with InputIsNFKD it is in
//  NFKD, and so in NFD too.  No character of such an input has a canonical
//  decomposition, and its marks are in canonical order: NFC then skips the
//  decomposition stages (the excluded composites and the singletons) and the
//  canonical ordering, as composition keeps the order of the marks that remain,
//  and inserts room only for the characters that may occur (those of NFD_QC=Y,
//  or NFKD_QC=Y).  NFKC with InputIsNFKD is NFC with InputIsNFKD; otherwise it
//  is NFKD, then NFC with InputIsNFKD (an NFD input still needs the
//  compatibility decompositions and their ordering).
enum class NormalizationInput {Any, InputIsNFD, InputIsNFKD};

//  The characters that may occur in an input of the mode (all, for Any).
UCD::UnicodeSet NormalizationInputSet(NormalizationInput mode);

//  The stages of NFC, by the kernels built in C++ from NFC_Tables
//  (lib/kernel/unicode/normalization/nfc_kernels.cpp):
//  - the insertion amounts: NFC expands where a precomposed character has a
//    nonstarter decomposition (it stays decomposed), and where a composite is
//    longer in UTF-8 than the starter of its decomposition;
//  - the excluded composites (decomposed in NFC as in NFD), and the singletons
//    (replaced by their canonical characters);
//  - the short composables: sequences of characters that are not reorderable,
//    composed only when adjacent (the first replaced by the composite, the
//    second zeroed), the self composables among them;
//  - the long composables, and the candidate classes: the reorderable marks
//    and the characters that may be the second of a composable sequence.
//  With a mask (the bytes of the characters within it), the stages change only
//  the characters within it, and the compositions are of characters within
//  one run of it (see NFC_Transform); the long composables' ccc_NR then also
//  marks the bytes outside the mask, which block them.
void NFC_InitialInsertion(PipelineBuilder & P, StreamSet * Basis, StreamSet * InsertionBixNum, StreamSet * WorkMask = nullptr,
                          NormalizationInput mode = NormalizationInput::Any);
void NFC_ExcludedComposites(PipelineBuilder & P, StreamSet * Basis, StreamSet * Result, StreamSet * Mask = nullptr);
void NFC_Singletons(PipelineBuilder & P, StreamSet * Basis, StreamSet * Result, StreamSet * Mask = nullptr);
void NFC_ShortComposables(PipelineBuilder & P, StreamSet * Basis, StreamSet * FinalBasis, StreamSet * Mask = nullptr);
void NFC_LongComposables(PipelineBuilder & P, StreamSet * Basis, StreamSet * ccc_NR, StreamSet * FinalBasis, StreamSet * Mask = nullptr);
//  The streams of In within the mask.
StreamSet * NFC_MaskStreams(PipelineBuilder & P, StreamSet * In, StreamSet * Mask);
//  NFC of a 21-bit basis (one position per character), with the kernels built
//  from NFC_Tables for that basis: no conversion to UTF-8.  With a Mask (a
//  stream of the positions of U21), each run of it normalized by itself, the
//  others unchanged; OutputMask, if given, receives the mask over the result.
//  Focused (the work regions filtered by character, transformed and placed back)
//  or in place.
StreamSet * NFC_U21(PipelineBuilder & P, StreamSet * U21, StreamSet * Mask = nullptr, StreamSet * OutputMask = nullptr,
                    bool focus = true, NormalizationInput mode = NormalizationInput::Any);
void NFC_CandidateClasses(PipelineBuilder & P, StreamSet * Basis, StreamSet * Candidates,
                          NormalizationInput mode = NormalizationInput::Any);
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
                          pablo::BitMovementMode m = pablo::BitMovementMode::Advance,
                          StreamSet * Mask = nullptr);

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
//  decomposed with NFD within them (NFD_U21 with the regions as its mask), give
//  the FCD.  With a Mask, each run of it is tested by itself: the characters
//  outside it have lccc = tccc = 0, so no region extends across them (regions
//  may still begin on one; intersect them with the mask).
//
void FCD_RegionMask(PipelineBuilder & P, StreamSet * U21, StreamSet * Regions, StreamSet * Mask = nullptr);

// NFC Support
//
//  NFC as a focused transform, with placement after the transform: the
//  transform decides which positions of its working space remain.

// The focus of NFC: the UTF-8 sequences that may change (composable sequences
// and excluded composites).
void DetermineNFC_WorkSelection(PipelineBuilder & P, StreamSet * U8_Basis, StreamSet * WorkSelectionMask,
                                StreamSet * Mask = nullptr, NormalizationInput mode = NormalizationInput::Any);

// NFC of the UTF-8 basis bits U8_Basis.  The transform works in a working space,
// U8_Basis with room inserted for decompositions; ValidWorkMask marks the positions
// of the working space that remain in TransformedBasis.  Given a WorkMask, room is
// inserted only within the work regions (for transforming a whole text).
// Given a Mask (a stream over U8_Basis set on every byte of each character
// within it; the C++ kernels only), each run of the mask is normalized by itself:
// only its characters change, no character composes with or moves across one
// outside it, and those are unchanged (as with barrier substitution).
// OutputMask, if given, receives the mask over TransformedBasis.
void NFC_Transform(PipelineBuilder & P, StreamSet * U8_Basis, StreamSet * TransformedBasis,
                   StreamSet * ValidWorkMask, StreamSet * WorkMask = nullptr,
                   StreamSet * Mask = nullptr, StreamSet * OutputMask = nullptr,
                   NormalizationInput mode = NormalizationInput::Any);

// The placement of the transformed work regions in the output, given the
// ValidWorkMask from NFC_Transform of the work regions.
void NFC_WorkPlacement(PipelineBuilder & P, StreamSet * U8_Basis, StreamSet * WorkSelectionMask,
                       StreamSet * ValidWorkMask, StreamSet * WorkPlacementMask, StreamSet * Mask = nullptr,
                       NormalizationInput mode = NormalizationInput::Any);

// NFC within a mask (a stream over Basis set on every byte of each character
// within it; the C++ kernels only): each run of the mask normalized by itself,
// the characters outside it unchanged and acting as barriers.  Focused (as
// NFC_FocusedTransform, with the options' filter and reintegration modes) or,
// with options.Focus false, in place.  Returns the output bytes; OutputMask, if
// given, receives the mask over them (the normalized characters, and those
// outside the mask where they were).
StreamSet * NFC_Masked(PipelineBuilder & P, const kernel::FocusOptions & options, StreamSet * Bytes, StreamSet * Basis,
                       StreamSet * Mask, StreamSet * OutputMask = nullptr, NormalizationInput mode = NormalizationInput::Any);

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

    //  NFD (NFKD) of a 21-bit basis.  With a mask (a stream of the positions of
    //  U21_Basis), each run of the mask is normalized by itself: only its
    //  characters are decomposed and reordered, and the characters outside it
    //  are unchanged and act as barriers (no character moves across them).
    //  SpreadMask, if given, receives the spread mask of the positions inserted
    //  for the decompositions (0 at them; nullptr if none can be), each after
    //  the character it decomposes.
    StreamSet * NFD_U21_Pipeline(StreamSet * U21_Basis, StreamSet * Mask = nullptr, StreamSet ** SpreadMask = nullptr);

    StreamSet * NFKD_U21_Pipeline(StreamSet * U21_Basis, StreamSet * Mask = nullptr, StreamSet ** SpreadMask = nullptr);

    void DetermineNFD_WorkItems(StreamSet * U8_Basis, StreamSet * u8index, StreamSet * workItems, StreamSet * Mask = nullptr);

    // The focus of NFD: the UTF-8 sequences that may change.
    //  (With a mask, as NFD_Masked: the candidates within it, with the character
    //  after them even outside it.)
    void DetermineNFD_WorkSelection(StreamSet * BasisBits, StreamSet * WorkSelectionMask, StreamSet * Mask = nullptr);

    // DetermineNFD_WorkSelection, with gap filling, then FilterByMask of the basis bits.
    void NFD_FilterStage(StreamSet * BasisBits, StreamSet * WorkSelectionMask, StreamSet * WorkingBasis);

    // Fill gaps of at most this many unchanged bytes between work regions (0: none).
    void setFillRunGaps(unsigned maxGap) { mFillRunGaps = maxGap; }

    void ComputeWorkPlacementMask(StreamSet * BasisBits, StreamSet * WorkSelectionMask, StreamSet * FinalWorkPlacementMask,
                                  StreamSet * Mask = nullptr);

    //  (With a mask over WorkingBasis, only its characters decompose, each run of it
    //  ordered by itself; OutputMask receives the mask over TransformedBasis.)
    void NFD_U8_Pipeline(StreamSet * WorkingBasis, StreamSet * TransformedBasis, StreamSet * Mask = nullptr, StreamSet * OutputMask = nullptr);

    void NFKD_U8_Pipeline(StreamSet * WorkingBasis, StreamSet * TransformedBasis, StreamSet * Mask = nullptr, StreamSet * OutputMask = nullptr);

    //  NFD (or NFKD) within a mask of UTF-8 text (a stream over Basis set on every
    //  byte of each character within it), as NFC_Masked: each run of the mask
    //  decomposed and ordered by itself, the characters outside it unchanged and
    //  acting as barriers.  Focused (as NFD_FocusedTransform, with the options'
    //  modes: the candidates within the mask, with the character after them even
    //  outside it, which separates the work regions) or, with options.Focus false,
    //  in place.  Returns the output bytes; OutputMask, if given, receives the
    //  mask over them.
    StreamSet * NFD_Masked(UCD::DecompositionOptions form, const kernel::FocusOptions & options, StreamSet * Bytes, StreamSet * Basis,
                           StreamSet * Mask, StreamSet * OutputMask = nullptr);

    //  NFD (or NFKD) of a 21-bit basis, focused (the candidates and the
    //  characters after them filtered, decomposed, and placed back) or in place,
    //  within a mask if given.  OutputMask, if given, receives the mask over the
    //  result; SpreadMask, the spread mask of the positions inserted (0 at them),
    //  each after the character decomposed.
    StreamSet * NFD_U21(UCD::DecompositionOptions form, StreamSet * U21, StreamSet * Mask = nullptr, StreamSet * OutputMask = nullptr,
                        bool focus = true, StreamSet ** SpreadMask = nullptr);

    // The focus of NFKD: the characters with a decomposition (canonical or
    // compatibility) or a nonzero combining class, and the character after each.
    void DetermineNFKD_WorkSelection(StreamSet * BasisBits, StreamSet * WorkSelectionMask, StreamSet * Mask = nullptr);

    void ComputeNFKD_WorkPlacementMask(StreamSet * BasisBits, StreamSet * WorkSelectionMask, StreamSet * FinalWorkPlacementMask,
                                       StreamSet * Mask = nullptr);

private:
    NFD_BixData & NFKD_Data();
    void U8_Decomposition(UCD::DecompositionOptions form, StreamSet * WorkingBasis, StreamSet * TransformedBasis,
                          StreamSet * Mask, StreamSet * OutputMask);

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
kernel::FocusedTransform NFC_FocusedTransform(const kernel::FocusOptions & options,
                                              NormalizationInput mode = NormalizationInput::Any);

//  NFKC (see NormalizationInput): NFKD by the builder (of the pipeline P), unless the input is in
//  NFKD, then NFC with InputIsNFKD.
//  - NFKC_Bytes: of UTF-8 text, focused as the options give (NFKD_FocusedTransform,
//    NFC_FocusedTransform); returns the output bytes.
//  - NFKC_Masked: within a mask, as NFC_Masked (NFD_Masked, then NFC_Masked within
//    the mask over its output); OutputMask, if given, receives the mask over the output.
//  - NFKC_U21: of a 21-bit basis, as NFC_U21 (NFD_U21, then NFC_U21).
StreamSet * NFKC_Bytes(PipelineBuilder & P, NFD_PipelineBuilder & builder, const kernel::FocusOptions & options, StreamSet * Bytes, StreamSet * Basis,
                       NormalizationInput mode = NormalizationInput::Any);
StreamSet * NFKC_Masked(PipelineBuilder & P, NFD_PipelineBuilder & builder, const kernel::FocusOptions & options, StreamSet * Bytes, StreamSet * Basis,
                        StreamSet * Mask, StreamSet * OutputMask = nullptr, NormalizationInput mode = NormalizationInput::Any);
StreamSet * NFKC_U21(PipelineBuilder & P, NFD_PipelineBuilder & builder, StreamSet * U21, StreamSet * Mask = nullptr, StreamSet * OutputMask = nullptr,
                     bool focus = true, NormalizationInput mode = NormalizationInput::Any);
