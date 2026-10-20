/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <cassert>
#include <functional>

namespace llvm { namespace cl { class OptionCategory; } }

namespace kernel {

class PipelineBuilder;
class StreamSet;

/*  Focused transforms: transforming only the regions of a text that may change.

    The stages are
      focus        finds the work regions (a work mask over the source bytes);
      filter       extracts the work regions into a short, focused stream;
      transform    transforms the focused stream;
      placement    marks where the transformed bytes go in the output (a placement
                   mask over the output);
      reintegrate  merges the transformed bytes with the unchanged source bytes.

    Placement is computed either from the source, independently of the transform
    (e.g., NFD, where the output length of each character is known from the source),
    or after the transform, from a valid mask the transform returns (e.g., NFC,
    where composition decides which positions of the transform's working space
    remain).  The valid mask is passed from the transform to the placement as is.

    The focus, placement and transform logic are given by the application; the
    filter and reintegration have several interchangeable implementations, selected
    by FocusOptions, so that they can be compared on different architectures and
    data mixes.  Each stage is labelled (PipelineBuilder::SetStageLabel).

    Work regions must consist of whole UTF-8 characters, and each must be replaced
    by at least one byte.  */

enum class FocusFilterMode {
    Bit,            // FilterByMask of the 8 basis bit streams
    Byte,           // FilterByMask of the source bytes, then transposition
    Run             // RunFilter of the source bytes, then transposition
};

enum class ReintegrationMode {
    Bit,            // FilterByMask and MergeByMask of basis bit streams, then P2S
    ByteSpread,     // spread unmodified and transformed bytes, then OR them
    ByteReplace,    // spread unmodified bytes, then ByteReplaceByMask
    Run             // RunMerge of unmodified and transformed bytes
};

enum class UnmodifiedFilterMode {
    Byte,           // FilterByMask of the source bytes
    Run             // RunFilter of the source bytes
};

struct FocusOptions {
    bool Focus = true;
    FocusFilterMode FocusFilter = FocusFilterMode::Run;
    ReintegrationMode Reintegration = ReintegrationMode::Run;
    UnmodifiedFilterMode UnmodifiedFilter = UnmodifiedFilterMode::Run;
    unsigned FillRunGaps = 0;

    // Whether the filter or reintegration stages need the source bytes (rather
    // than, or as well as, the source basis bits).
    bool needsSourceBytes() const {
        return Focus && (FocusFilter != FocusFilterMode::Bit || Reintegration != ReintegrationMode::Bit);
    }
    bool needsSourceBasis() const {
        return !Focus || FocusFilter == FocusFilterMode::Bit || Reintegration == ReintegrationMode::Bit;
    }

    // The options given by -focus-filter, -reintegrate, -unmodified-filter and
    // -fill-run-gaps.
    static FocusOptions fromCommandLine();
};

// The category of the focused-transform command line options, for
// codegen::ParseCommandLineOptions.
llvm::cl::OptionCategory & FocusedTransformOptions();

class FocusedTransform {
public:
    // basis -> work mask (whole UTF-8 characters, over the source bytes)
    using FocusFunction = std::function<void (PipelineBuilder & P, StreamSet * basis, StreamSet * workMask)>;
    // basis, work mask -> placement mask (over the output bytes)
    using PlacementFunction = std::function<void (PipelineBuilder & P, StreamSet * basis, StreamSet * workMask, StreamSet * placementMask)>;
    // focused (or, without focus, whole) basis -> transformed basis
    using TransformFunction = std::function<void (PipelineBuilder & P, StreamSet * basis, StreamSet * transformedBasis)>;
    // For placement after the transform:
    // focused (or, without focus, whole) basis -> transformed basis, valid mask
    using ValidatingTransformFunction = std::function<void (PipelineBuilder & P, StreamSet * basis, StreamSet * transformedBasis, StreamSet * validMask)>;
    // basis, work mask, valid mask -> placement mask (over the output bytes)
    using ValidatedPlacementFunction = std::function<void (PipelineBuilder & P, StreamSet * basis, StreamSet * workMask, StreamSet * validMask, StreamSet * placementMask)>;

    // Placement from the source.
    FocusedTransform(const FocusOptions & options, FocusFunction focus, PlacementFunction placement, TransformFunction transform)
    : mOptions(options), mFocus(std::move(focus)), mPlacement(std::move(placement)), mTransform(std::move(transform)) {}

    // Placement after the transform.
    FocusedTransform(const FocusOptions & options, FocusFunction focus, ValidatedPlacementFunction placement, ValidatingTransformFunction transform)
    : mOptions(options), mFocus(std::move(focus)), mValidatedPlacement(std::move(placement)), mValidatingTransform(std::move(transform)) {}

    const FocusOptions & getOptions() const { return mOptions; }

    bool placesAfterTransform() const { return static_cast<bool>(mValidatingTransform); }

    // The whole transform: source bytes and basis -> output bytes.  Either source
    // stream may be null if the options do not need it.
    StreamSet * build(PipelineBuilder & P, StreamSet * sourceBytes, StreamSet * sourceBasis) const;

    // The individual stages, for applications that place them in separate pipelines.
    StreamSet * focus(PipelineBuilder & P, StreamSet * sourceBasis) const;
    StreamSet * filter(PipelineBuilder & P, StreamSet * sourceBytes, StreamSet * sourceBasis, StreamSet * workMask) const;
    StreamSet * placement(PipelineBuilder & P, StreamSet * sourceBasis, StreamSet * workMask) const;
    StreamSet * transform(PipelineBuilder & P, StreamSet * focusedBasis) const;
    // placement after the transform; validMask as returned by transform
    StreamSet * validatedPlacement(PipelineBuilder & P, StreamSet * sourceBasis, StreamSet * workMask, StreamSet * validMask) const;
    StreamSet * reintegrate(PipelineBuilder & P, StreamSet * workMask, StreamSet * placementMask,
                            StreamSet * sourceBytes, StreamSet * sourceBasis, StreamSet * transformedBasis) const;

    // As above, writing into the given stream sets (for pipeline inputs/outputs).
    void focus(PipelineBuilder & P, StreamSet * sourceBasis, StreamSet * workMask) const;
    void filter(PipelineBuilder & P, StreamSet * sourceBytes, StreamSet * sourceBasis, StreamSet * workMask, StreamSet * focusedBasis) const;
    void placement(PipelineBuilder & P, StreamSet * sourceBasis, StreamSet * workMask, StreamSet * placementMask) const;
    void validatedPlacement(PipelineBuilder & P, StreamSet * sourceBasis, StreamSet * workMask, StreamSet * validMask, StreamSet * placementMask) const;
    // With placement after the transform, validMask (a single bit stream) receives
    // the transform's valid mask; it must be given exactly then.
    void transform(PipelineBuilder & P, StreamSet * focusedBasis, StreamSet * transformedBasis, StreamSet * validMask = nullptr) const;
    void reintegrate(PipelineBuilder & P, StreamSet * workMask, StreamSet * placementMask,
                     StreamSet * sourceBytes, StreamSet * sourceBasis, StreamSet * transformedBasis,
                     StreamSet * outputBytes) const;

private:
    FocusOptions mOptions;
    FocusFunction mFocus;
    PlacementFunction mPlacement;
    TransformFunction mTransform;
    ValidatedPlacementFunction mValidatedPlacement;
    ValidatingTransformFunction mValidatingTransform;
};

}
