#include "pipeline_analysis.hpp"

namespace kernel {

void PipelineAnalysis::scanFamilyKernelBindings(KernelBuilder & b) {

    // Any non-call-by-family kernel K initialization (termination) function ought to be compiled into the
    // initialization (termination) of its parent pipeline P but when K contains call-by-family kernels itself,
    // the initialization functions must still be passed to it from the entire chain of kernels leading from the
    // “main” function. Those intermediate kernels do not care about the initialization (termination) functions
    // but we must still track the arguments as to where they go.

    // To avoid the expense of instantiating a pipeline compiler when generating the
    // "main" function for the pipeline on a subsequent run, family args are passed in
    // order of the original kernels in the pipeline; however, the order of these kernels
    // are actually executed may be shuffled around in the pipeline itself. So to map the
    // original order to the pipeline order, we just search for a matching kernel object.

    FamilyScalarGraph G(PipelineOutput * 2 + 1);

    const auto n = mPipelineKernel->getNumOfNestedKernelFamilyCalls();

    unsigned inputNum = 0;

    if (n > 0) {

        // TODO: will need to update this if more than pipelines have families; e.g., optimization branch

        std::function<void(const Kernels &, unsigned)> mapPipeline = [&](const Kernels & N, const unsigned root) {

            for (unsigned i = 0, j = 0; i < N.size(); ++i) {

                const auto & ref = N[i];
                Kernel * const obj = ref.Object;

                const auto m = obj->getNumOfNestedKernelFamilyCalls();

                if (ref.isFamilyCall() || m > 0) {
                    if (ref.isFamilyCall()) {

                        obj->ensureLoaded(b);

                        unsigned flags = 0;
                        if (LLVM_LIKELY(obj->isStateful())) {
                            flags |= FamilyScalarData::CaptureSharedStateObject;
                        }
                        if (obj->hasThreadLocal()) {
                            flags |= FamilyScalarData::CaptureThreadLocal;
                        }
                        if (obj->allocatesInternalStreamSets()) {
                            flags |= FamilyScalarData::CaptureAllocateInternal;
                        }

                        add_edge(PipelineInput, root, FamilyScalarData{inputNum++, j++, flags}, G);
                    }
                    if (m > 0) {
                        mapPipeline(cast<PipelineKernel>(obj)->getKernels(), root);
                    }
                }
            }

        };


        const Kernels & V = mPipelineKernel->getKernels();

        for (unsigned i = 0, j = 0; i < V.size(); ++i) {

            const auto & ref = V[i];
            Kernel * const obj = ref.Object;

            const auto m = obj->getNumOfNestedKernelFamilyCalls();

            if (ref.isFamilyCall() || m > 0) {
                auto kernelId = FirstKernel;
                for (; kernelId <= LastKernel; kernelId++) {
                    if (obj == getKernel(kernelId)) {
                        goto found_kernel_in_graph;
                    }
                }
                // We still need to consume the input argument even if this kernel has been
                // removed from the program. However, it now refers to a "deleted" entry node.
                assert (kernelId == PipelineOutput);
                BEGIN_SCOPED_REGION
                SmallVector<char, 256> tmp;
                raw_svector_ostream out(tmp);

                out << "Warning: " << obj->getName()
                    << " is explicitly called "
                    << mPipelineKernel->getName();

                if (ref.isFamilyCall()) {
                    out << " as a family kernel";
                } else {
                    out << " and contains family kernel calls";
                }

                out << " but it was removed from the pipeline due to having no returned or"
                       " internally read outputs nor being marked as side effecting.\n";
                errs() << out.str();
                END_SCOPED_REGION
found_kernel_in_graph:
                if (ref.isFamilyCall()) {

                   // obj->ensureLoaded(b);

                    unsigned flags = FamilyScalarData::CaptureStoreInKernelState;
                    if (LLVM_LIKELY(obj->isStateful())) {
                        flags |= FamilyScalarData::CaptureSharedStateObject;
                    }
                    if (obj->hasThreadLocal()) {
                        flags |= FamilyScalarData::CaptureThreadLocal;
                    }
                    if (obj->allocatesInternalStreamSets()) {
                        flags |= FamilyScalarData::CaptureAllocateInternal;
                    }

                    add_edge(PipelineInput, kernelId, FamilyScalarData{inputNum++, j++, flags}, G);
                } else if (m > 0) {
                    mapPipeline(cast<PipelineKernel>(obj)->getKernels(), PipelineOutput + kernelId);
                }
            }
        }
    }

#if 0
    if (LLVM_UNLIKELY(inputNum != n)) {
        SmallVector<char, 256> tmp;
        raw_svector_ostream out(tmp);
        out << "Pipeline definition error: "
            << mPipelineKernel->getName()
            << " invokes a kernel with a family call but getNumOfNestedKernelFamilyCalls()"
               " returns 0. This flag cannot be reliably set by the PipelineCompiler"
               " since it is instantiated only for non-cached pipelines.";
        report_fatal_error(StringRef(out.str()));
    }
#endif

    mFamilyScalarGraph = G;
}


}
