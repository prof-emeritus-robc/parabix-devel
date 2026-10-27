#include "pipeline_analysis.hpp"
#include "lexographic_ordering.hpp"
#include <toolchain/toolchain.h>
#include <boost/dynamic_bitset/dynamic_bitset.hpp>
#include <boost/graph/connected_components.hpp>
#include <queue>
#include <z3.h>

#if Z3_VERSION_INTEGER >= LLVM_VERSION_CODE(4, 7, 0)
    typedef int64_t Z3_int64;
#else
    typedef long long int        Z3_int64;
#endif

// #define PRINT_INTRA_PARTITION_VECTOR_GRAPH

namespace kernel {

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief computeIntraPartitionRepetitionVectors
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineAnalysis::computeIntraPartitionRepetitionVectors(PartitionGraph & P) {

    const auto numOfPartitions = num_vertices(P);

    // Each fixed-rate edge within a partition (or on a streamset that joins partitions) is a balance
    // equation r(producer) * expectedOutput = r(consumer) * expectedInput. The solutions of each
    // connected set of these equations are the multiples of a single rational vector: propagate it
    // from a root, check that every edge that closes a cycle agrees, then scale each partition to
    // its least positive integer solution.

    const auto numOfNodes = num_vertices(Relationships);

    // Balance[u] holds (v, r(v) / r(u)) for every equation between u and v.
    std::vector<std::vector<std::pair<unsigned, Rational>>> Balance(numOfNodes);

    auto add_balance_equation = [&](const unsigned producer, const Rational & expectedOutput,
                                    const unsigned consumer, const Rational & expectedInput) {
        assert (expectedOutput.numerator() > 0 && expectedInput.numerator() > 0);
        Balance[producer].emplace_back(consumer, expectedOutput / expectedInput);
        Balance[consumer].emplace_back(producer, expectedInput / expectedOutput);
    };

    {
    flat_set<size_t> joiningStreamSets;

    for (unsigned producerPartitionId = 0; producerPartitionId < numOfPartitions; ++producerPartitionId) {
        const PartitionData & N = P[producerPartitionId];
        const auto & K = N.Kernels;

        assert (joiningStreamSets.empty());

        for (auto e : make_iterator_range(in_edges(producerPartitionId, P))) {
            const PartitionStreamSet & ss = P[e];
            if (ss.Type == 1) {
                joiningStreamSets.insert(ss.Id);
            }
        }

        for (auto e : make_iterator_range(out_edges(producerPartitionId, P))) {
            const PartitionStreamSet & ss = P[e];
            if (ss.Type == 1) {
                joiningStreamSets.insert(ss.Id);
            }
        }

        for (const auto producer : K) {
            assert (Relationships[producer].Type == RelationshipNode::IsKernel);
            for (const auto e : make_iterator_range(out_edges(producer, Relationships))) {
                const auto binding = target(e, Relationships);
                if (Relationships[binding].Type == RelationshipNode::IsBinding) {
                    const auto f = first_out_edge(binding, Relationships);
                    assert (Relationships[f].Reason != ReasonType::Reference);
                    const auto streamSet = target(f, Relationships);
                    if (Relationships[streamSet].Type == RelationshipNode::IsStreamSet) {
                        const RelationshipNode & output = Relationships[binding];
                        assert (output.Type == RelationshipNode::IsBinding);

                        const Binding & outputBinding = output.Binding;
                        const ProcessingRate & outputRate = outputBinding.getRate();
                        // ignore unknown output rates; we cannot reason about them here.
                        if (LLVM_LIKELY(outputRate.isFixed())) {

                            const RelationshipNode & producerNode = Relationships[producer];
                            assert (producerNode.Type == RelationshipNode::IsKernel);
                            const auto expectedOutput = outputRate.getRate() * producerNode.Kernel->getStride();

                            for (const auto e : make_iterator_range(out_edges(streamSet, Relationships))) {
                                const auto binding = target(e, Relationships);
                                const RelationshipNode & input = Relationships[binding];
                                if (LLVM_LIKELY(input.Type == RelationshipNode::IsBinding)) {

                                    const Binding & inputBinding = input.Binding;
                                    const ProcessingRate & inputRate = inputBinding.getRate();

                                    if (LLVM_LIKELY(inputRate.isFixed())) {

                                        const auto f = first_out_edge(binding, Relationships);
                                        assert (Relationships[f].Reason != ReasonType::Reference);
                                        const unsigned consumer = target(f, Relationships);

                                        const auto c = PartitionIds.find(consumer);
                                        assert (c != PartitionIds.end());
                                        const auto consumerPartitionId = c->second;
                                        assert (producerPartitionId <= consumerPartitionId);

                                        if (producerPartitionId == consumerPartitionId || joiningStreamSets.count(streamSet)) {
                                            const RelationshipNode & consumerNode = Relationships[consumer];
                                            assert (consumerNode.Type == RelationshipNode::IsKernel);
                                            const auto expectedInput = inputRate.getRate() * consumerNode.Kernel->getStride();
                                            add_balance_equation(producer, expectedOutput, consumer, expectedInput);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        joiningStreamSets.clear();
    }
    }

    // A zero entry marks a kernel that has not yet been reached.
    std::vector<Rational> RelativeRepetitions(numOfNodes, Rational{0});

    std::queue<unsigned> Q;

    for (unsigned partitionId = 0; partitionId < numOfPartitions; ++partitionId) {
        for (const auto root : P[partitionId].Kernels) {
            if (RelativeRepetitions[root].numerator() != 0) {
                continue;
            }
            RelativeRepetitions[root] = Rational{1};
            assert (Q.empty());
            Q.push(root);
            while (!Q.empty()) {
                const auto u = Q.front();
                Q.pop();
                for (const auto & eq : Balance[u]) {
                    const auto v = eq.first;
                    const auto expected = RelativeRepetitions[u] * eq.second;
                    auto & R = RelativeRepetitions[v];
                    if (R.numerator() == 0) {
                        R = expected;
                        Q.push(v);
                    } else if (LLVM_UNLIKELY(R != expected)) {
                        SmallVector<char, 256> tmp;
                        raw_svector_ostream msg(tmp);
                        msg << "Inconsistent fixed rates between "
                            << Relationships[u].Kernel->getName() << " and "
                            << Relationships[v].Kernel->getName()
                            << ": no repetition vector balances their dataflow";
                        report_fatal_error(StringRef(msg.str()));
                    }
                }
            }
        }
    }

    for (unsigned partitionId = 0; partitionId < numOfPartitions; ++partitionId) {
        PartitionData & N = P[partitionId];
        const auto & K = N.Kernels;
        const auto n = K.size();
        assert (n > 0);
        size_t denomLCM = 1;
        for (const auto k : K) {
            denomLCM = boost::lcm<size_t>(denomLCM, RelativeRepetitions[k].denominator());
        }
        size_t numGCD = 0;
        for (const auto k : K) {
            const auto & R = RelativeRepetitions[k];
            numGCD = boost::gcd<size_t>(numGCD, R.numerator() * (denomLCM / R.denominator()));
        }
        N.Repetitions.resize(n);
        for (unsigned i = 0; i < n; ++i) {
            const auto & R = RelativeRepetitions[K[i]];
            const auto r = (R.numerator() * (denomLCM / R.denominator())) / numGCD;
            if (LLVM_UNLIKELY(r > std::numeric_limits<unsigned>::max())) {
                report_fatal_error("Repetition vector entry exceeds the range of an unsigned integer");
            }
            N.Repetitions[i] = r;
        }
    }

    #ifdef PRINT_INTRA_PARTITION_VECTOR_GRAPH
    auto & out = errs();

    out << "digraph \"V\" {\n";
    for (unsigned partitionId = 0; partitionId < numOfPartitions; ++partitionId) {
        const PartitionData & N = P[partitionId];
        const auto & K = N.Kernels;
        const auto n = K.size();
        for (unsigned i = 0; i < n; ++i) {
            const auto k = K[i];
            const RelationshipNode & node = Relationships[k];
            assert (node.Type == RelationshipNode::IsKernel);
            out << "v" << k << " [label=\""
                << k << ". " << node.Kernel->getName()
                << "  (" << N.Repetitions[i] << ")\"];\n";
        }
    }

    for (unsigned u = 0; u < numOfNodes; ++u) {
        for (const auto & eq : Balance[u]) {
            if (u < eq.first) {
                const auto & V = eq.second;
                out << "v" << u << " -> v" << eq.first <<
                       " [label=\"" << V.numerator() << "/" << V.denominator() << "\"];\n";
            }
        }
    }

    out << "}\n\n";
    out.flush();
    #endif



}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief identifyInterPartitionSymbolicRates
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineAnalysis::identifyInterPartitionSymbolicRates() {



}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief calculatePartialSumStepFactors
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineAnalysis::calculatePartialSumStepFactors(KernelBuilder & b) {

    PartialSumStepFactorGraph G(LastStreamSet + 1);

    for (auto kernel = FirstKernel; kernel <= LastKernel; ++kernel) {
        auto checkForPopCountRef = [&](const BufferGraph::edge_descriptor io) {
            const BufferPort & port = mBufferGraph[io];
            const Binding & binding = port.Binding;
            const ProcessingRate & rate = binding.getRate();
            if (LLVM_UNLIKELY(rate.isPartialSum())) {
                const auto inputRefPort = getReference(kernel, port.Port);
                const auto streamSet = getInputBufferVertex(kernel, inputRefPort);
                assert (streamSet > LastKernel);
                const auto refOuput = in_edge(streamSet, mBufferGraph);
                const BufferPort & refOutputRate = mBufferGraph[refOuput];
                const BufferPort & refInputRate = getInputPort(kernel, inputRefPort);
                const auto ratio = refInputRate.Minimum / refOutputRate.Minimum;
                assert (ratio.denominator() == 1 && ratio.numerator() > 0);
                assert (!edge(streamSet, kernel, G).second);
                add_edge(streamSet, kernel, ratio.numerator(), G);
            }
        };

        for (const auto input : make_iterator_range(in_edges(kernel, mBufferGraph))) {
            checkForPopCountRef(input);
        }
        for (const auto output : make_iterator_range(out_edges(kernel, mBufferGraph))) {
            checkForPopCountRef(output);
        }
    }

    const auto bw = b.getBitBlockWidth();
    const auto fw = b.getSizeTy()->getIntegerBitWidth();
    assert ((bw % fw) == 0 && bw >= fw);
    const auto stepsPerBlock = bw / fw;

    for (auto kernel = FirstKernel; kernel <= LastKernel; ++kernel) {
        for (const auto output : make_iterator_range(out_edges(kernel, mBufferGraph))) {
            const auto streamSet = target(output, mBufferGraph);
            if (out_degree(streamSet, G) != 0) {
                auto maxStepFactor = StrideRepetitionVector[kernel];
                for (const auto e : make_iterator_range(out_edges(streamSet, G))) {
                    const auto consumer = target(e, G);
                    assert (consumer > kernel && consumer <= LastKernel);
                    const auto k = G[e] * StrideRepetitionVector[consumer];
                    maxStepFactor = std::max(maxStepFactor, k);
                }



                const BufferPort & bp = mBufferGraph[output];
                maxStepFactor = round_up_to(maxStepFactor + bp.RequiredOverflowSpace, stepsPerBlock);
                const auto spanLength = maxStepFactor / stepsPerBlock;
                add_edge(kernel, streamSet, spanLength, G);
                assert (spanLength > 0);

                BufferNode & bn = mBufferGraph[streamSet];
                bn.PartialSumSpanLength = std::max(bn.PartialSumSpanLength, spanLength + 1);
                bn.Type |= BufferType::PopCountPartialSumStream;
            }
        }

    }

    mPartialSumStepFactorGraph = G;
}


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief calculateRelativeToInputDataTransferIORates
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineAnalysis::calculateRelativeToInputDataTransferIORates() {

    if (LLVM_UNLIKELY(FirstStreamSet == PipelineOutput)) {
        assert (LastStreamSet == PipelineOutput);
        return;
    }

    using NonFixedTaintGraph = adjacency_list<hash_setS, vecS, bidirectionalS, boost::dynamic_bitset<size_t>>;

    using Graph = adjacency_list<hash_setS, vecS, undirectedS>;

    const auto cfg = Z3_mk_config();
    Z3_set_param_value(cfg, "model", "true");
    Z3_set_param_value(cfg, "proof", "false");
    const auto ctx = Z3_mk_context(cfg);
    Z3_del_config(cfg);
    const auto solver = Z3_mk_optimize(ctx);
    Z3_optimize_inc_ref(ctx, solver);

    auto hard_assert = [&](Z3_ast c) {
        Z3_optimize_assert(ctx, solver, c);
    };

    auto soft_assert = [&](Z3_ast c) {
        Z3_optimize_assert_soft(ctx, solver, c, "1", nullptr);
    };

    auto check = [&]() -> Z3_lbool {
        #if Z3_VERSION_INTEGER >= LLVM_VERSION_CODE(4, 5, 0)
        return Z3_optimize_check(ctx, solver, 0, nullptr);
        #else
        return Z3_optimize_check(ctx, solver);
        #endif
    };

    const auto varType = Z3_mk_real_sort(ctx);

    auto constant_real = [&](const Rational & value) {
        return Z3_mk_real(ctx, value.numerator(), value.denominator());
    };

    auto multiply =[&](Z3_ast X, const Rational & value) {
        if ((value.numerator() == 1) && (value.denominator() == 1)) {
            return X;
        }
        Z3_ast args[2] = { X, constant_real(value) };
        return Z3_mk_mul(ctx, 2, args);
    };


    NonFixedTaintGraph T(PartitionCount);
    for (unsigned prodId = 0; prodId < PartitionCount; ++prodId) {
        T[prodId].resize(PartitionCount, false);
    }

    const auto z3_ZERO = constant_real(0);

    const auto z3_ONE = constant_real(1);

    std::vector<Z3_ast> VarList(LastStreamSet + 1U, nullptr);

    std::vector<Z3_ast> PartitionVarList(PartitionCount, nullptr);

    for (unsigned i = 0; i < PartitionCount; ++i) {
        auto rootVar = Z3_mk_fresh_const(ctx, nullptr, varType);
        hard_assert(Z3_mk_gt(ctx, rootVar, z3_ZERO));
        PartitionVarList[i] = rootVar;
    }

    // we cannot predict how much data will be passed to a pipeline
    if (out_degree(PipelineInput, mBufferGraph) > 0) {
        for (const auto input : make_iterator_range(out_edges(PipelineInput, mBufferGraph))) {
            Z3_ast expOutRate = Z3_mk_fresh_const(ctx, nullptr, varType);
            hard_assert(Z3_mk_gt(ctx, expOutRate, z3_ZERO));
            const auto streamSet = target(input, mBufferGraph);
            VarList[streamSet] = expOutRate;
        }

        // if the pipeline has input, allow the pipeline to adjust the segment size
        assert (KernelPartitionId[PipelineInput] == 0);
        T[0].set(0);
    }


    assert (KernelPartitionId[PipelineOutput] == PartitionCount - 1);
    assert (PipelineOutput == FirstKernelInPartition[PartitionCount - 1]);

    for (unsigned i = PipelineInput; i <= PipelineOutput; ++i) {
        const auto partId = KernelPartitionId[i];
        assert (partId < PartitionCount);
        auto rootVar = PartitionVarList[partId];
        VarList[i] = multiply(rootVar, StrideRepetitionVector[i]);
    }

    for (auto kernel = FirstKernel; kernel <= PipelineOutput; ++kernel) {

        for (const auto input : make_iterator_range(in_edges(kernel, mBufferGraph))) {
            const auto streamSet = source(input, mBufferGraph);
            assert (VarList[streamSet] != nullptr);
            const auto producer = parent(streamSet, mBufferGraph);
            const auto prodPartId = KernelPartitionId[producer];
            const auto consPartId = KernelPartitionId[kernel];
            if (prodPartId != consPartId) {
                const BufferPort & port = mBufferGraph[input];
                const Binding & bind = port.Binding;
                const ProcessingRate & iRate = bind.getRate();
                if (LLVM_UNLIKELY(iRate.isGreedy())) {
                    // VarList[streamSet] == VarList[streamSet]
                } else {
                    const auto expectedInput = (port.Minimum + port.Maximum) * Rational{1, 2};
                    assert (expectedInput.numerator() > 0);
                    Z3_ast expInRate = multiply(VarList[kernel], expectedInput);
                    auto r = Z3_mk_eq(ctx, expInRate, VarList[streamSet]);
//                    if (iRate.isFixed()) {
//                        hard_assert(r);
//                    } else {
                        soft_assert(r);
//                    }
                }
                add_edge(prodPartId, consPartId, T);
                if (port.Minimum != port.Maximum) {
                    assert (consPartId > 0);
                    T[consPartId].set(consPartId);
                }
            }
        }

        for (const auto output : make_iterator_range(out_edges(kernel, mBufferGraph))) {
            const auto streamSet = target(output, mBufferGraph);
            assert (VarList[streamSet] == nullptr);
            const BufferPort & port = mBufferGraph[output];
            const Binding & bind = port.Binding;
            const ProcessingRate & oRate = bind.getRate();

            Z3_ast expOutRate = nullptr;

            if (LLVM_UNLIKELY(oRate.isUnknown())) {
                // we cannot predict how much data will be passed to a pipeline
                expOutRate = Z3_mk_fresh_const(ctx, nullptr, varType);
            } else {
                const auto expectedOutput = (port.Minimum + port.Maximum) * Rational{1, 2};
                assert (expectedOutput.numerator() > 0);
                expOutRate = multiply(VarList[kernel], expectedOutput);
            }

            hard_assert(Z3_mk_gt(ctx, expOutRate, z3_ZERO));
            VarList[streamSet] = expOutRate;
            if (port.Minimum != port.Maximum) {
                for (const auto input : make_iterator_range(out_edges(streamSet, mBufferGraph))) {
                    const auto consumer = target(input, mBufferGraph);
                    const auto consPartId = KernelPartitionId[consumer];
                    assert (consPartId != KernelPartitionId[kernel]);
                    assert (consPartId > 0);
                    T[consPartId].set(consPartId);
                }
            }
        }
    }

    SmallVector<Z3_ast, 2> fakeIOVars;
    for (unsigned kernel = FirstKernel; kernel <= LastKernel; ++kernel) {
        if (in_degree(kernel, mBufferGraph) == 0) {
            fakeIOVars.push_back(VarList[kernel]);
        }
    }

    if (fakeIOVars.empty()) {
        fakeIOVars.push_back(VarList[PipelineInput]);
    }

    const auto m = fakeIOVars.size(); assert (m > 0);

    SmallVector<Z3_ast, 2> fakeIOConstraint(m);
    for (unsigned i = 0; i < m; ++i) {
        fakeIOConstraint[i] = Z3_mk_eq(ctx, fakeIOVars[i], z3_ONE);
    }

    Z3_ast constraint;
    if (m == 1) {
        constraint = fakeIOConstraint[0];
    } else {
        constraint = Z3_mk_or(ctx, m, fakeIOConstraint.data());
    }
    hard_assert(constraint);

    if (LLVM_UNLIKELY(check() == Z3_L_FALSE)) {
        report_fatal_error("Z3 failed to find a solution to the maximum permitted dataflow problem");
    }

    const auto model = Z3_optimize_get_model(ctx, solver);
    Z3_model_inc_ref(ctx, model);

    for (auto streamSet = FirstStreamSet; streamSet <= LastStreamSet; ++streamSet) {
        Z3_ast value;
        if (LLVM_UNLIKELY(Z3_model_eval(ctx, model, VarList[streamSet], Z3_L_TRUE, &value) != Z3_L_TRUE)) {
            report_fatal_error("Unexpected Z3 error when attempting to obtain value from model!");
        }

        Z3_int64 num, denom;
        if (LLVM_UNLIKELY(Z3_get_numeral_rational_int64(ctx, value, &num, &denom) != Z3_L_TRUE)) {
            report_fatal_error("Unexpected Z3 error when attempting to convert model value to number!");
        }

        assert (denom > 0);
        assert (num > 0);

        BufferNode & bn = mBufferGraph[streamSet];
        bn.RelativeIORate = Rational{num, denom};
    }

    Z3_model_dec_ref(ctx, model);
    Z3_optimize_dec_ref(ctx, solver);
    Z3_del_context(ctx);
    Z3_reset_memory();


    #ifndef NDEBUG
    const reverse_traversal ordering(PartitionCount);
    assert (is_valid_topological_sorting(ordering, T));
    #endif

    // iterate through the graph in topological order to determine what portions of
    // the program are not strictly fixed rate

    Graph P(PartitionCount);

    for (unsigned partId = 0; partId < PartitionCount; ++partId) {
        const auto & X = T[partId];
        if (X.any()) {
            const auto firstKernel = FirstKernelInPartition[partId];
            const auto firstKernelInNextPartition = FirstKernelInPartition[partId + 1];
            for (auto kernel = firstKernel + 1; kernel < firstKernelInNextPartition; ++kernel) {
                for (const auto input : make_iterator_range(in_edges(kernel, mBufferGraph))) {
                    const auto streamSet = source(input, mBufferGraph);
                    const auto producer = parent(streamSet, mBufferGraph);
                    const auto prodPartId = KernelPartitionId[producer];
                    if ((prodPartId != partId) && (X != T[prodPartId])) {
                        add_edge(partId, prodPartId, P);
                    }
                }
            }
            for (const auto output : make_iterator_range(out_edges(partId, T))) {
                T[target(output, T)] |= X;
            }
        }
    }

    for (auto kernel = PipelineInput; kernel <= PipelineOutput; ++kernel) {
        const auto partId = KernelPartitionId[kernel];
        assert (partId < PartitionCount);
        const auto R = StrideRepetitionVector[kernel]; // partitionRepVector[partId] * StrideRepetitionVector[kernel] * lcmOfDenom;
        const auto & X = T[partId];
        if (X.any()) {
            MinimumNumOfStrides[kernel] = 0;
            MaximumNumOfStrides[kernel] = R * 2;
        } else {
            MinimumNumOfStrides[kernel] = R;
            MaximumNumOfStrides[kernel] = R;
        }
    }

#if 0
    if (LLVM_UNLIKELY(P[0].ExpectedStridesPerSegment != Rational{1})) {
        auto checkIO = [](const Bindings & bindings) -> bool {
            for (const Binding & binding : bindings) {
                if (isCountable(binding) && !binding.isDeferred()) {
                    return true;
                }
            }
            return false;
        };
        if (checkIO(mPipelineKernel->getInputStreamSetBindings()) || checkIO(mPipelineKernel->getOutputStreamSetBindings())) {
            errs() << "WARNING! Pipeline "
                   << mPipelineKernel->getName() <<
                      " requires more than one stride of input but has at least one "
                      "non-deferred Countable I/O rate. This may cause I/O errors.\n\n"
                      "Check -PrintPipelineGraph for details.\n";
        }
    }
#endif

}

} // end of kernel namespace
