#include "pipeline_analysis.hpp"
#include "lexographic_ordering.hpp"
#include <toolchain/toolchain.h>
#include <boost/dynamic_bitset/dynamic_bitset.hpp>
#include <boost/graph/connected_components.hpp>
#include <numeric>
#include <queue>

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


namespace {

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief RatioEquation
 *
 * value(V) = value(U) * Ratio, over positive node values. An exact equation relates fixed rates; any
 * other relates expected rates.
 ** ------------------------------------------------------------------------------------------------------------- */
struct RatioEquation {
    unsigned U;
    unsigned V;
    Rational Ratio;
    bool Exact;
};

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief RatioForest
 *
 * Node values implied by a consistent set of ratio equations: each component holds the values of its nodes
 * relative to one another.
 ** ------------------------------------------------------------------------------------------------------------- */
class RatioForest {
public:

    explicit RatioForest(const unsigned numOfNodes)
    : Component(numOfNodes)
    , Value(numOfNodes, Rational{1})
    , Members(numOfNodes) {
        for (unsigned i = 0; i < numOfNodes; ++i) {
            Component[i] = i;
            Members[i].push_back(i);
        }
    }

    // Adds the equation if it agrees with those already added; returns whether it does.
    bool add(const RatioEquation & eq) {
        const auto cu = Component[eq.U];
        const auto cv = Component[eq.V];
        const auto expected = Value[eq.U] * eq.Ratio;
        if (cu == cv) {
            return Value[eq.V] == expected;
        }
        // merge the smaller component into the larger
        auto from = cv, into = cu;
        auto scale = expected / Value[eq.V];
        if (Members[cv].size() > Members[cu].size()) {
            std::swap(from, into);
            scale = Rational{1} / scale;
        }
        for (const auto w : Members[from]) {
            Value[w] *= scale;
            Component[w] = into;
        }
        Members[into].insert(Members[into].end(), Members[from].begin(), Members[from].end());
        Members[from].clear();
        return true;
    }

    std::vector<unsigned> Component;
    std::vector<Rational> Value;
    std::vector<std::vector<unsigned>> Members;
};

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief equationBlocks
 *
 * Partitions the equations into the biconnected blocks of the multigraph they form over the nodes (Tarjan's
 * algorithm, tracking the equation that reached each node so that parallel equations form a block).
 ** ------------------------------------------------------------------------------------------------------------- */
std::vector<std::vector<unsigned>> equationBlocks(const unsigned numOfNodes, const std::vector<RatioEquation> & equations) {

    std::vector<std::vector<std::pair<unsigned, unsigned>>> incident(numOfNodes); // (equation, other node)
    for (unsigned i = 0; i < equations.size(); ++i) {
        const auto & eq = equations[i];
        incident[eq.U].emplace_back(i, eq.V);
        incident[eq.V].emplace_back(i, eq.U);
    }

    constexpr unsigned Unvisited = std::numeric_limits<unsigned>::max();
    std::vector<unsigned> order(numOfNodes, Unvisited);
    std::vector<unsigned> low(numOfNodes, 0);
    std::vector<bool> seen(equations.size(), false);
    std::vector<unsigned> stack;
    std::vector<std::vector<unsigned>> blocks;
    unsigned counter = 0;

    struct Frame {
        unsigned Node;
        unsigned ParentEquation;
        unsigned Next;
    };
    std::vector<Frame> dfs;

    for (unsigned root = 0; root < numOfNodes; ++root) {
        if (order[root] != Unvisited) {
            continue;
        }
        order[root] = low[root] = counter++;
        dfs.push_back(Frame{root, Unvisited, 0});
        while (!dfs.empty()) {
            auto & F = dfs.back();
            const auto u = F.Node;
            if (F.Next < incident[u].size()) {
                const auto eq = incident[u][F.Next].first;
                const auto v = incident[u][F.Next].second;
                ++F.Next;
                if (eq == F.ParentEquation || seen[eq]) {
                    continue;
                }
                seen[eq] = true;
                stack.push_back(eq);
                if (order[v] == Unvisited) {
                    order[v] = low[v] = counter++;
                    dfs.push_back(Frame{v, eq, 0});
                } else {
                    low[u] = std::min(low[u], order[v]);
                }
            } else {
                const auto parentEquation = F.ParentEquation;
                dfs.pop_back();
                if (dfs.empty()) {
                    continue;
                }
                const auto p = dfs.back().Node;
                low[p] = std::min(low[p], low[u]);
                if (low[u] >= order[p]) {
                    // p separates the equations above parentEquation on the stack from the rest
                    std::vector<unsigned> block;
                    for (;;) {
                        const auto eq = stack.back();
                        stack.pop_back();
                        block.push_back(eq);
                        if (eq == parentEquation) {
                            break;
                        }
                    }
                    std::sort(block.begin(), block.end());
                    blocks.emplace_back(std::move(block));
                }
            }
        }
    }
    assert (stack.empty());
    return blocks;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief maximumConsistentEquations
 *
 * Returns which equations to keep: a largest set of them that some positive node values satisfy. Every cycle
 * of equations lies within one biconnected block, so a set is consistent if and only if its restriction to each
 * block is, and each block is maximized on its own by trying to drop 0, 1, 2, ... of its equations. Among
 * equally large sets, it keeps exact equations over expected ones, then earlier equations over later ones. A
 * block whose search exceeds a fixed budget instead keeps each equation, in that order of preference, that
 * agrees with those already kept.
 ** ------------------------------------------------------------------------------------------------------------- */
std::vector<bool> maximumConsistentEquations(const unsigned numOfNodes, const std::vector<RatioEquation> & equations) {

    constexpr size_t MAXIMUM_SUBSETS_PER_BLOCK = 1U << 16;

    std::vector<bool> keep(equations.size(), true);

    for (const auto & block : equationBlocks(numOfNodes, equations)) {
        const auto n = block.size();
        if (n == 1) {
            continue;
        }

        // order the block's equations from most to least preferred to keep
        std::vector<unsigned> preferred(block);
        std::stable_sort(preferred.begin(), preferred.end(), [&](const unsigned a, const unsigned b) {
            return equations[a].Exact && !equations[b].Exact;
        });

        // renumber the block's nodes so that each check is proportional to its size
        flat_map<unsigned, unsigned> local;
        std::vector<RatioEquation> E;
        E.reserve(n);
        for (const auto i : preferred) {
            const auto & eq = equations[i];
            const auto u = local.emplace(eq.U, local.size()).first->second;
            const auto v = local.emplace(eq.V, local.size()).first->second;
            E.push_back(RatioEquation{u, v, eq.Ratio, eq.Exact});
        }
        const unsigned m = local.size();

        std::vector<bool> drop(n, false);

        auto consistent = [&]() {
            RatioForest F(m);
            for (unsigned i = 0; i < n; ++i) {
                if (!drop[i] && !F.add(E[i])) {
                    return false;
                }
            }
            return true;
        };

        bool found = consistent();
        size_t subsets = 1;
        // choose the k equations to drop as c[0] < ... < c[k - 1] over reversed indices, in lexicographic order
        std::vector<unsigned> c;
        for (unsigned k = 1; !found && k < n && subsets < MAXIMUM_SUBSETS_PER_BLOCK; ++k) {
            c.resize(k);
            std::iota(c.begin(), c.end(), 0U);
            for (;;) {
                std::fill(drop.begin(), drop.end(), false);
                for (const auto j : c) {
                    drop[n - 1 - j] = true;
                }
                if (consistent()) {
                    found = true;
                    break;
                }
                if (++subsets >= MAXIMUM_SUBSETS_PER_BLOCK) {
                    break;
                }
                // advance to the next combination
                int i = k - 1;
                while (i >= 0 && c[i] == n - k + i) {
                    --i;
                }
                if (i < 0) {
                    break;
                }
                ++c[i];
                for (unsigned j = i + 1; j < k; ++j) {
                    c[j] = c[j - 1] + 1;
                }
            }
        }

        if (!found) {
            RatioForest F(m);
            for (unsigned i = 0; i < n; ++i) {
                drop[i] = !F.add(E[i]);
            }
        }

        for (unsigned i = 0; i < n; ++i) {
            keep[preferred[i]] = !drop[i];
        }
    }

    return keep;
}

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

    NonFixedTaintGraph T(PartitionCount);
    for (unsigned prodId = 0; prodId < PartitionCount; ++prodId) {
        T[prodId].resize(PartitionCount, false);
    }

    // The expected rates are the values of a set of nodes: one per partition, scaling the stride
    // repetition vector of its kernels, and one per streamset whose rate cannot be predicted (a
    // pipeline input or an unknown output rate). Every other streamset's rate is its producer's
    // partition value times a known factor.
    struct StreamSetRate {
        unsigned Node = 0;
        Rational Factor{0};
        bool Fixed = false;
    };

    const auto numOfNodes = PartitionCount + (LastStreamSet - FirstStreamSet + 1U);

    auto unpredictableRateNode = [&](const size_t streamSet) -> unsigned {
        assert (FirstStreamSet <= streamSet && streamSet <= LastStreamSet);
        return PartitionCount + (streamSet - FirstStreamSet);
    };

    std::vector<StreamSetRate> Rate(LastStreamSet + 1U);

    // Each input that crosses partitions expects its consumer to read what its producer writes.
    std::vector<RatioEquation> Equations;

    // we cannot predict how much data will be passed to a pipeline
    if (out_degree(PipelineInput, mBufferGraph) > 0) {
        for (const auto input : make_iterator_range(out_edges(PipelineInput, mBufferGraph))) {
            const auto streamSet = target(input, mBufferGraph);
            Rate[streamSet] = StreamSetRate{unpredictableRateNode(streamSet), Rational{1}, false};
        }

        // if the pipeline has input, allow the pipeline to adjust the segment size
        assert (KernelPartitionId[PipelineInput] == 0);
        T[0].set(0);
    }


    assert (KernelPartitionId[PipelineOutput] == PartitionCount - 1);
    assert (PipelineOutput == FirstKernelInPartition[PartitionCount - 1]);

    for (auto kernel = FirstKernel; kernel <= PipelineOutput; ++kernel) {

        for (const auto input : make_iterator_range(in_edges(kernel, mBufferGraph))) {
            const auto streamSet = source(input, mBufferGraph);
            const StreamSetRate & R = Rate[streamSet];
            assert (R.Factor.numerator() > 0);
            const auto producer = parent(streamSet, mBufferGraph);
            const auto prodPartId = KernelPartitionId[producer];
            const auto consPartId = KernelPartitionId[kernel];
            if (prodPartId != consPartId) {
                const BufferPort & port = mBufferGraph[input];
                const Binding & bind = port.Binding;
                const ProcessingRate & iRate = bind.getRate();
                // a greedy input reads whatever is written
                if (LLVM_LIKELY(!iRate.isGreedy())) {
                    const auto expectedInput = (port.Minimum + port.Maximum) * Rational{1, 2};
                    assert (expectedInput.numerator() > 0);
                    Equations.push_back(RatioEquation{consPartId, R.Node, (expectedInput * StrideRepetitionVector[kernel]) / R.Factor,
                                                      R.Fixed && port.Minimum == port.Maximum});
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
            assert (Rate[streamSet].Factor.numerator() == 0);
            const BufferPort & port = mBufferGraph[output];
            const Binding & bind = port.Binding;
            const ProcessingRate & oRate = bind.getRate();

            if (LLVM_UNLIKELY(oRate.isUnknown())) {
                // we cannot predict how much data will be passed to a pipeline
                Rate[streamSet] = StreamSetRate{unpredictableRateNode(streamSet), Rational{1}, false};
            } else {
                const auto expectedOutput = (port.Minimum + port.Maximum) * Rational{1, 2};
                assert (expectedOutput.numerator() > 0);
                Rate[streamSet] = StreamSetRate{KernelPartitionId[kernel], expectedOutput * StrideRepetitionVector[kernel],
                                                port.Minimum == port.Maximum};
            }

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

    // Satisfy as many equations as possible; two nodes are in the same component once a kept equation
    // relates them.
    const auto keep = maximumConsistentEquations(numOfNodes, Equations);
    RatioForest Forest(numOfNodes);
    for (unsigned i = 0; i < Equations.size(); ++i) {
        if (keep[i]) {
            [[maybe_unused]] const auto consistent = Forest.add(Equations[i]);
            assert (consistent);
        }
    }
    const auto & Component = Forest.Component;
    auto & Value = Forest.Value;
    const auto & Members = Forest.Members;

    // Each component is only determined up to scale: rates are relative to one stride of the first kernel
    // without input in it (or of the pipeline input), failing that of the first kernel of its lowest
    // numbered partition. A streamset whose rate cannot be predicted and that no equation relates to
    // any partition has a rate of one.
    std::vector<unsigned> anchors;
    for (unsigned kernel = FirstKernel; kernel <= LastKernel; ++kernel) {
        if (in_degree(kernel, mBufferGraph) == 0) {
            anchors.push_back(kernel);
        }
    }
    if (anchors.empty()) {
        anchors.push_back(PipelineInput);
    }

    std::vector<bool> normalized(numOfNodes, false);
    auto normalize = [&](const unsigned component, const Rational & scale) {
        for (const auto w : Members[component]) {
            Value[w] *= scale;
        }
        normalized[component] = true;
    };
    for (const auto kernel : anchors) {
        const auto partId = KernelPartitionId[kernel];
        const auto c = Component[partId];
        if (!normalized[c]) {
            normalize(c, Rational{1} / (Value[partId] * StrideRepetitionVector[kernel]));
        }
    }
    for (unsigned c = 0; c < numOfNodes; ++c) {
        if (!Members[c].empty() && !normalized[c]) {
            const auto lowest = *std::min_element(Members[c].begin(), Members[c].end());
            if (lowest < PartitionCount) {
                normalize(c, Rational{1} / (Value[lowest] * StrideRepetitionVector[FirstKernelInPartition[lowest]]));
            } else {
                assert (Members[c].size() == 1);
                normalize(c, Rational{1} / Value[lowest]);
            }
        }
    }

    for (auto streamSet = FirstStreamSet; streamSet <= LastStreamSet; ++streamSet) {
        const StreamSetRate & R = Rate[streamSet];
        assert (R.Factor.numerator() > 0);
        BufferNode & bn = mBufferGraph[streamSet];
        bn.RelativeIORate = Value[R.Node] * R.Factor;
    }

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
