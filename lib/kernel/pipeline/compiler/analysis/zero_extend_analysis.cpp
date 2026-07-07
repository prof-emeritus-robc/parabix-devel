#include "pipeline_analysis.hpp"
#include "lexographic_ordering.hpp"

namespace kernel {

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief identifyZeroExtendedStreamSets
 *
 * Determine whether there are any zero extend attributes on any kernel and verify that every kernel with
 * zero extend attributes have at least one input that is not transitively dependent on a zero extended
 * input stream.
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineAnalysis::identifyZeroExtendedStreamSets() {

    #ifndef DISABLE_ZERO_EXTEND
    using Graph = adjacency_list<vecS, vecS, bidirectionalS>;

    for (unsigned kernel = FirstKernel; kernel <= LastKernel; ++kernel) {

        const auto numOfInputs = in_degree(kernel, mBufferGraph);

        // A kernel input with a ZeroExtend attribute performs additional checks to determine whether it has
        // exhausted its actual data stream then switches to a "null stream" (assuming the producer is
        // finished.) This adds complexity to the overall pipeline but is unnecessary when we can guarantee
        // the ZeroExtend-ed stream cannot end before the other inputs. An easy test for this is to check
        // whether *any* of the inputs to a kernel are inputs to the partition. Any such input would not
        // dictate the dataflow within the partition until after this kernel has been executed.

        Graph H(numOfInputs + 1);

        for (const auto input : make_iterator_range(in_edges(kernel, mBufferGraph))) {
            BufferPort & inputData = mBufferGraph[input];
            if (inputData.isZeroExtended()) {
                const auto streamSet = source(input, mBufferGraph);
                const BufferNode & bn = mBufferGraph[streamSet];
                if (LLVM_UNLIKELY(bn.isConstant())) {
                    SmallVector<char, 256> tmp;
                    raw_svector_ostream msg(tmp);
                    const Binding & binding = inputData.Binding;
                    msg << getKernel(kernel)->getName() << '.' << binding.getName()
                        << " cannot ZeroExtend a repeating streamset";
                    report_fatal_error(StringRef(msg.str()));
                }
                if (LLVM_UNLIKELY(inputData.isPrincipal())) {
                    SmallVector<char, 256> tmp;
                    raw_svector_ostream msg(tmp);
                    const Binding & binding = inputData.Binding;
                    msg << getKernel(kernel)->getName() << '.' << binding.getName()
                        << " cannot have both ZeroExtend and Principal attributes";
                    report_fatal_error(StringRef(msg.str()));
                }

                // TODO: once we can determine what inter-partition channels can bound the number of strides for
                // a partition, we can filter the ones that will never be zero-extended.

                if (bn.isThreadLocal()) {
                    inputData.Flags &= ~BufferPortType::IsZeroExtended;
                } else {
                    add_edge(inputData.Port.Number, numOfInputs, H);
                }
            }
        }

        if (LLVM_LIKELY(in_degree(numOfInputs, H) == 0)) {
            continue;
        }


        // First verify whether the ZeroExtend attributes are correct in the unmodified
        // system (to reduce the possibility of future programmer error.)

        // enumerate the input relations
        for (const auto e : make_iterator_range(in_edges(kernel, mStreamGraph))) {
            const auto k = source(e, mStreamGraph);
            const RelationshipNode & rn = mStreamGraph[k];
            assert (rn.Type == RelationshipNode::IsBinding);
            const Binding & binding = rn.Binding;
            const RelationshipType & port = mStreamGraph[e];

            if (LLVM_UNLIKELY(in_degree(k, mStreamGraph) != 1)) {
                graph_traits<RelationshipGraph>::in_edge_iterator ei, ei_end;
                std::tie(ei, ei_end) = in_edges(k, mStreamGraph);
                assert (std::distance(ei, ei_end) == 2);
                const auto f = *(ei + 1);
                const RelationshipType & ref = mStreamGraph[f];
                assert (ref.Reason == ReasonType::Reference);
                add_edge(ref.Number, port.Number, H);
            }
        }

        if (LLVM_UNLIKELY(in_degree(numOfInputs, H) == numOfInputs)) {
            report_fatal_error(StringRef(getKernel(kernel)->getName())
                               + " requires at least one non-zero-extended input");
        }

        // Identify all transitive dependencies on zero-extended inputs
        transitive_closure_dag(H);

        if (LLVM_UNLIKELY(in_degree(numOfInputs, H) == numOfInputs)) {
            report_fatal_error(StringRef(getKernel(kernel)->getName())
                               + " requires at least one non-zero-extended input"
                                 " that does not refer to a zero-extended input");
        }





    }
    #endif
}

}
