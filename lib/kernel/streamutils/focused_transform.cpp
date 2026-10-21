/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/streamutils/focused_transform.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/streamutils/run_replace.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/bitwise/bixlogic.h>
#include <llvm/Support/CommandLine.h>

using namespace llvm;

namespace kernel {

static cl::OptionCategory FocusOptionCategory("Focused Transform Options",
    "Implementations of the filter and reintegration stages of focused transforms.");

static cl::opt<FocusFilterMode> FocusFilterOption("focus-filter", cl::init(FocusFilterMode::Run),
    cl::desc("Extraction of the work regions:"),
    cl::values(clEnumValN(FocusFilterMode::Bit, "bit", "FilterByMask of the basis bit streams"),
               clEnumValN(FocusFilterMode::Byte, "byte", "FilterByMask of the source bytes"),
               clEnumValN(FocusFilterMode::Run, "run", "RunFilter of the source bytes (default)")),
    cl::cat(FocusOptionCategory));

static cl::opt<ReintegrationMode> ReintegrationOption("reintegrate", cl::init(ReintegrationMode::Run),
    cl::desc("Merging of transformed and unmodified data:"),
    cl::values(clEnumValN(ReintegrationMode::Bit, "bit", "MergeByMask of basis bit streams"),
               clEnumValN(ReintegrationMode::ByteSpread, "byte-spread", "spread and OR of bytes"),
               clEnumValN(ReintegrationMode::ByteReplace, "byte-replace", "spread and ByteReplaceByMask of bytes"),
               clEnumValN(ReintegrationMode::Run, "run", "RunMerge of bytes (default)")),
    cl::cat(FocusOptionCategory));

static cl::opt<UnmodifiedFilterMode> UnmodifiedFilterOption("unmodified-filter", cl::init(UnmodifiedFilterMode::Run),
    cl::desc("Extraction of unmodified bytes for byte reintegration:"),
    cl::values(clEnumValN(UnmodifiedFilterMode::Byte, "byte", "FilterByMask"),
               clEnumValN(UnmodifiedFilterMode::Run, "run", "RunFilter (default)")),
    cl::cat(FocusOptionCategory));

static cl::opt<unsigned> FillRunGapsOption("fill-run-gaps", cl::init(0),
    cl::desc("Include gaps of at most N unmodified bytes between work regions in the work"),
    cl::cat(FocusOptionCategory));

static cl::opt<bool> NoFocusOption("NoFocus", cl::init(false),
    cl::desc("Process the entire text without filtering to narrow the focus of work"),
    cl::cat(FocusOptionCategory));

static cl::list<FocusStage> FocusLayersOption("focus-layers", cl::CommaSeparated,
    cl::desc("Place a pipeline phase boundary after each of the given stages:"),
    cl::values(clEnumValN(FocusStage::Focus, "focus", "after the focus"),
               clEnumValN(FocusStage::Filter, "filter", "after the filter"),
               clEnumValN(FocusStage::Placement, "placement", "after the placement"),
               clEnumValN(FocusStage::Transform, "transform", "after the transform")),
    cl::cat(FocusOptionCategory));

cl::OptionCategory & FocusedTransformOptions() {
    return FocusOptionCategory;
}

FocusOptions FocusOptions::fromCommandLine() {
    FocusOptions options;
    options.FocusFilter = FocusFilterOption;
    options.Reintegration = ReintegrationOption;
    options.UnmodifiedFilter = UnmodifiedFilterOption;
    options.FillRunGaps = FillRunGapsOption;
    options.Focus = !NoFocusOption;
    for (const auto stage : FocusLayersOption) {
        options.addLayerAfter(stage);
    }
    return options;
}

StreamSet * FocusedTransform::build(PipelineBuilder & P, StreamSet * sourceBytes, StreamSet * sourceBasis) const {
    if (!mOptions.Focus) {
        StreamSet * const transformedBasis = P.CreateStreamSet(8, 1);
        transform(P, sourceBasis, transformedBasis, placesAfterTransform() ? P.CreateStreamSet(1, 1) : nullptr);
        P.SetStageLabel("output");
        StreamSet * const outputBytes = P.CreateStreamSet(1, 8);
        P.CreateKernelCall<P2SKernel>(transformedBasis, outputBytes);
        return outputBytes;
    }
    auto layer = [&](FocusStage stage) {
        if (mOptions.layerAfter(stage)) {
            P.InsertPhaseBoundary();
        }
    };
    StreamSet * const workMask = focus(P, sourceBasis);
    layer(FocusStage::Focus);
    StreamSet * const focusedBasis = filter(P, sourceBytes, sourceBasis, workMask);
    layer(FocusStage::Filter);
    StreamSet * placementMask = nullptr;
    StreamSet * transformedBasis = nullptr;
    if (placesAfterTransform()) {
        StreamSet * const validMask = P.CreateStreamSet(1, 1);
        transformedBasis = P.CreateStreamSet(8, 1);
        transform(P, focusedBasis, transformedBasis, validMask);
        layer(FocusStage::Transform);
        placementMask = validatedPlacement(P, sourceBasis, workMask, validMask);
        layer(FocusStage::Placement);
    } else {
        placementMask = placement(P, sourceBasis, workMask);
        layer(FocusStage::Placement);
        transformedBasis = transform(P, focusedBasis);
        layer(FocusStage::Transform);
    }
    return reintegrate(P, workMask, placementMask, sourceBytes, sourceBasis, transformedBasis);
}

StreamSet * FocusedTransform::focus(PipelineBuilder & P, StreamSet * sourceBasis) const {
    StreamSet * const workMask = P.CreateStreamSet(1, 1);
    focus(P, sourceBasis, workMask);
    return workMask;
}

void FocusedTransform::focus(PipelineBuilder & P, StreamSet * sourceBasis, StreamSet * workMask) const {
    P.SetStageLabel("focus");
    if (mOptions.FillRunGaps > 0) {
        StreamSet * const regions = P.CreateStreamSet(1, 1);
        mFocus(P, sourceBasis, regions);
        FillRunGaps(P, regions, mOptions.FillRunGaps, workMask);
    } else {
        mFocus(P, sourceBasis, workMask);
    }
}

StreamSet * FocusedTransform::filter(PipelineBuilder & P, StreamSet * sourceBytes, StreamSet * sourceBasis, StreamSet * workMask) const {
    StreamSet * const focusedBasis = P.CreateStreamSet(8, 1);
    filter(P, sourceBytes, sourceBasis, workMask, focusedBasis);
    return focusedBasis;
}

void FocusedTransform::filter(PipelineBuilder & P, StreamSet * sourceBytes, StreamSet * sourceBasis, StreamSet * workMask, StreamSet * focusedBasis) const {
    P.SetStageLabel("filter");
    if (mOptions.FocusFilter == FocusFilterMode::Bit) {
        FilterByMask(P, workMask, sourceBasis, focusedBasis);
    } else {
        StreamSet * const focusedBytes = P.CreateStreamSet(1, 8);
        if (mOptions.FocusFilter == FocusFilterMode::Run) {
            RunFilter(P, workMask, sourceBytes, focusedBytes);
        } else {
            FilterByMask(P, workMask, sourceBytes, focusedBytes);
        }
        Selected_S2P(P, focusedBytes, focusedBasis);
    }
}

StreamSet * FocusedTransform::placement(PipelineBuilder & P, StreamSet * sourceBasis, StreamSet * workMask) const {
    StreamSet * const placementMask = P.CreateStreamSet(1, 1);
    placement(P, sourceBasis, workMask, placementMask);
    return placementMask;
}

void FocusedTransform::placement(PipelineBuilder & P, StreamSet * sourceBasis, StreamSet * workMask, StreamSet * placementMask) const {
    assert (!placesAfterTransform());
    P.SetStageLabel("placement");
    mPlacement(P, sourceBasis, workMask, placementMask);
}

StreamSet * FocusedTransform::validatedPlacement(PipelineBuilder & P, StreamSet * sourceBasis, StreamSet * workMask, StreamSet * validMask) const {
    StreamSet * const placementMask = P.CreateStreamSet(1, 1);
    validatedPlacement(P, sourceBasis, workMask, validMask, placementMask);
    return placementMask;
}

void FocusedTransform::validatedPlacement(PipelineBuilder & P, StreamSet * sourceBasis, StreamSet * workMask, StreamSet * validMask, StreamSet * placementMask) const {
    assert (placesAfterTransform());
    P.SetStageLabel("placement");
    mValidatedPlacement(P, sourceBasis, workMask, validMask, placementMask);
}

StreamSet * FocusedTransform::transform(PipelineBuilder & P, StreamSet * focusedBasis) const {
    assert (!placesAfterTransform());
    StreamSet * const transformedBasis = P.CreateStreamSet(8, 1);
    transform(P, focusedBasis, transformedBasis);
    return transformedBasis;
}

void FocusedTransform::transform(PipelineBuilder & P, StreamSet * focusedBasis, StreamSet * transformedBasis, StreamSet * validMask) const {
    assert ((validMask != nullptr) == placesAfterTransform());
    P.SetStageLabel("transform");
    if (placesAfterTransform()) {
        mValidatingTransform(P, focusedBasis, transformedBasis, validMask);
    } else {
        mTransform(P, focusedBasis, transformedBasis);
    }
}

StreamSet * FocusedTransform::reintegrate(PipelineBuilder & P, StreamSet * workMask, StreamSet * placementMask,
                                          StreamSet * sourceBytes, StreamSet * sourceBasis, StreamSet * transformedBasis) const {
    StreamSet * const outputBytes = P.CreateStreamSet(1, 8);
    reintegrate(P, workMask, placementMask, sourceBytes, sourceBasis, transformedBasis, outputBytes);
    return outputBytes;
}

void FocusedTransform::reintegrate(PipelineBuilder & P, StreamSet * workMask, StreamSet * placementMask,
                                   StreamSet * sourceBytes, StreamSet * sourceBasis, StreamSet * transformedBasis,
                                   StreamSet * outputBytes) const {
    P.SetStageLabel("reintegrate");
    StreamSet * const unmodifiedMask = P.CreateStreamSet(1, 1);
    Invert(P, workMask, unmodifiedMask);

    if (mOptions.Reintegration == ReintegrationMode::Bit) {
        StreamSet * const unmodifiedBasis = P.CreateStreamSet(8);
        FilterByMask(P, unmodifiedMask, sourceBasis, unmodifiedBasis);
        StreamSet * const outputBasis = P.CreateStreamSet(8);
        MergeByMask(P, placementMask, transformedBasis, unmodifiedBasis, outputBasis);
        P.CreateKernelCall<P2SKernel>(outputBasis, outputBytes);
        return;
    }

    StreamSet * const unmodified = P.CreateStreamSet(1, 8);
    if (mOptions.UnmodifiedFilter == UnmodifiedFilterMode::Run) {
        RunFilter(P, unmodifiedMask, sourceBytes, unmodified);
    } else {
        FilterByMask(P, unmodifiedMask, sourceBytes, unmodified);
    }
    StreamSet * const transformedBytes = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(transformedBasis, transformedBytes);

    if (mOptions.Reintegration == ReintegrationMode::Run) {
        RunMerge(P, placementMask, transformedBytes, unmodified, outputBytes);
        return;
    }

    StreamSet * const unmodifiedPlacementMask = P.CreateStreamSet(1, 1);
    Invert(P, placementMask, unmodifiedPlacementMask);
    StreamSet * const unmodifiedPlaced = P.CreateStreamSet(1, 8);
    SpreadByMask(P, unmodifiedPlacementMask, unmodified, unmodifiedPlaced);
    if (mOptions.Reintegration == ReintegrationMode::ByteReplace) {
        P.CreateKernelCall<ByteReplaceByMask>(placementMask, unmodifiedPlaced, transformedBytes, outputBytes);
    } else {
        StreamSet * const transformedPlaced = P.CreateStreamSet(1, 8);
        SpreadByMask(P, placementMask, transformedBytes, transformedPlaced);
        OrCombine(P, unmodifiedPlaced, transformedPlaced, outputBytes);
    }
}

}
