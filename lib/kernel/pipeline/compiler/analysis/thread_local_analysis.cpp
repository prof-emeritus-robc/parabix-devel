#include "pipeline_analysis.hpp"
#include "evolutionary_algorithm.hpp"
#include "lexographic_ordering.hpp"
#include <boost/icl/interval_set.hpp>
#include <boost/integer/common_factor.hpp>
#include <toolchain/toolchain.h>
#include <queue>
#include <z3.h>

#if Z3_VERSION_INTEGER >= LLVM_VERSION_CODE(4, 7, 0)
    typedef int64_t Z3_int64;
#else
    typedef long long int        Z3_int64;
#endif

// #define PRINT_Z3_OPTIMIZATION

using boost::icl::interval_set;

namespace kernel {

// TODO: nested pipeline kernels could report how much internal memory they require
// and reason about that here (and in the scheduling phase)

constexpr static unsigned BUFFER_SIZE_INIT_POPULATION_SIZE = 15;

constexpr static unsigned BUFFER_SIZE_GA_MAX_INIT_TIME_SECONDS = 2;

constexpr static unsigned BUFFER_SIZE_POPULATION_SIZE = 30;

constexpr static unsigned BUFFER_SIZE_GA_MAX_TIME_SECONDS = 15;

constexpr static unsigned BUFFER_SIZE_GA_STALLS = 50;

using ConflictGraph = adjacency_list<hash_setS, vecS, undirectedS>;

using IntervalSet = interval_set<unsigned>;

using Interval = IntervalSet::interval_type;

using Vertex = unsigned;

struct BufferLayoutOptimizerWorker final : public PermutationBasedEvolutionaryAlgorithmWorker {


    struct PartitionData {
        unsigned StreamSetCount = 0;
        unsigned PageCount = 0;
        Rational MinStridesPerSegment{};
        Rational SumOfStridesPerSegment;
    };

    constexpr static auto MAX_INT = std::numeric_limits<Rational::int_type>::max();

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief repair
     ** ------------------------------------------------------------------------------------------------------------- */
    void repair(Candidate & /* candidate */, pipeline_random_engine & /* rng */) final { }

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief fitness
     ** ------------------------------------------------------------------------------------------------------------- */
    size_t fitness(const Candidate & candidate, pipeline_random_engine & /* rng */) final {

        const auto partitionCount = MaxMemorySize.size();

        assert (partitionCount > 0);

        for (unsigned i = 0; i < partitionCount; ++i) {
            MaxMemorySize[i] = 0;
        }

        const auto candidateLength = candidate.size();

        assert (candidateLength >= partitionCount);

        #ifndef NDEBUG
        for (unsigned i = 0; i < candidateLength; ++i) {
            GC_Intervals[i] = Interval::right_open(0, 0);
        }
        #endif

        for (unsigned i = 0; i < candidateLength; ++i) {

            const auto a = candidate[i];
            assert (a < candidateLength);
            assert (GC_Intervals[a].lower() == 0);
            assert (GC_Intervals[a].upper() == 0);

            assert (GC_IntervalSet.empty());
            for (unsigned j = 0; j < i; ++j) {
                const auto b = candidate[j];
                assert (b < candidateLength);
                if (edge(a, b, I).second) {
                    GC_IntervalSet.add(GC_Intervals[b]);
                } else {
                    assert ("sanity check for undirected graph failed?" && !edge(b, a, I).second);
                }
            }

            const auto w = weight[a];

            size_t start = 0;
            size_t end = w;
            if (!GC_IntervalSet.empty()) {
                #ifndef NDEBUG
                auto ii = GC_IntervalSet.begin();
                assert (ii->lower() < ii->upper());
                auto lastUpper = ii->upper();
                while (++ii != GC_IntervalSet.end()) {
                    assert (lastUpper < ii->lower());
                    assert (ii->lower() < ii->upper());
                    lastUpper = ii->upper();
                }
                #endif
                for (const auto & interval : GC_IntervalSet) {
                    if (end <= interval.lower()) {
                        for (auto i = start; i < end; ++i) {
                            assert (!boost::icl::contains(GC_IntervalSet, i));
                        }
                        break;
                    } else {
                        const auto r = interval.upper();
                        start = r;
                        end = r + w;
                    }
                }
                GC_IntervalSet.clear();
            }

            assert (end > start);
            GC_Intervals[a] = Interval::right_open(start, end);

            const auto p = partitionId[a];
            assert (p < partitionId.size());
            auto & M = MaxMemorySize[p];
            M = std::max(M, end);
        }

        std::sort(MaxMemorySize.begin(), MaxMemorySize.end());

        const auto idx = (partitionCount / 2);
        auto median = MaxMemorySize[idx];
        if ((partitionCount % 2) == 0) {
            const auto idx2 = ((partitionCount - 1) / 2);
            assert (idx != idx2);
            median = (median + MaxMemorySize[idx2] + 1) / 2;
        }
        const auto max = MaxMemorySize[partitionCount - 1];
        return median + (max * max);
    }

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief translate
     ** ------------------------------------------------------------------------------------------------------------- */
    std::vector<Interval> translate(const OrderingDAWG & O, const unsigned candidateLength,
                   pipeline_random_engine & rng) {
        Candidate chosen;
        chosen.reserve(candidateLength);
        Vertex u = 0;
        while (out_degree(u, O) != 0) {
            const auto e = first_out_edge(u, O);
            const auto k = O[e];
            chosen.push_back(k);
            u = target(e, O);
        }
        assert (chosen.size() == candidateLength);
        fitness(chosen, rng);
        return GC_Intervals;
    }

    BufferLayoutOptimizerWorker(const unsigned candidateLength
                               , const size_t partitionCount
                               , const ConflictGraph & I
                               , const std::vector<size_t> & weight
                               , const std::vector<unsigned> & partitionId
                               , pipeline_random_engine & rng)
    : I(I)
    , weight(weight)
    , partitionId(partitionId)
    , GC_IntervalSet()
    , GC_Intervals(candidateLength)
    , MaxMemorySize(partitionCount) {

    }

private:

    const ConflictGraph & I;
    const std::vector<size_t> & weight;
    const std::vector<unsigned> & partitionId;

    IntervalSet GC_IntervalSet;
    std::vector<Interval> GC_Intervals;
    std::vector<size_t> MaxMemorySize;

};

struct BufferLayoutOptimizer final : public PermutationBasedEvolutionaryAlgorithm {

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief getIntervals
     ** ------------------------------------------------------------------------------------------------------------- */
    std::vector<Interval> translate(const OrderingDAWG & O, pipeline_random_engine & rng) {
        auto w = (BufferLayoutOptimizerWorker *)mainWorker.get();
        return w->translate(O, candidateLength, rng);
    }

    WorkerPtr makeWorker(pipeline_random_engine & rng) final {
        return std::make_unique<BufferLayoutOptimizerWorker>(candidateLength, partitionCount, I, weight, partitionId, rng);
    }

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief constructor
     ** ------------------------------------------------------------------------------------------------------------- */
    BufferLayoutOptimizer(const unsigned numOfLocalStreamSets
                         , const size_t partitionCount
                         , const ConflictGraph & I
                         , const std::vector<size_t> & weight
                         , const std::vector<unsigned> & partitionId
                         , pipeline_random_engine & srcRng)
    : PermutationBasedEvolutionaryAlgorithm (numOfLocalStreamSets,
                                             BUFFER_SIZE_GA_MAX_INIT_TIME_SECONDS,
                                             BUFFER_SIZE_INIT_POPULATION_SIZE,
                                             BUFFER_SIZE_GA_MAX_TIME_SECONDS,
                                             BUFFER_SIZE_POPULATION_SIZE,
                                             BUFFER_SIZE_GA_STALLS,
                                             std::max(codegen::SegmentThreads, codegen::TaskThreads),
                                             srcRng)
    , I(I)
    , weight(weight)
    , partitionId(partitionId)
    , partitionCount(partitionCount) {
        assert (num_vertices(I) == numOfLocalStreamSets);
        assert (weight.size() >= numOfLocalStreamSets);
        assert (numOfLocalStreamSets >= partitionCount);
    }


private:

    const ConflictGraph & I;
    const std::vector<size_t> & weight;
    const std::vector<unsigned> & partitionId;
    const size_t partitionCount;

};

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief determineInitialThreadLocalBufferLayout
 *
 * Given our buffer graph, we want to identify the best placement to maximize sequential prefetching behavior with
 * the minimal total memory required. Although this assumes that the memory-aware scheduling algorithm was first
 * called, it does not actually use any data from it. The reason for this disconnection is to enable us to explore
 * the impact of static memory allocation independent of the chosen scheduling algorithm.
 *
 * Because the Intel L2 streamer prefetcher has one forward and one reverse monitor per page, a streamset will
 * only be placed in a page in which no other streamset accesses it during the same kernel invocation.
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineAnalysis::determineInitialThreadLocalBufferLayout(KernelBuilder & b, pipeline_random_engine & rng) {

    if (LLVM_UNLIKELY(FirstStreamSet == PipelineOutput)) {
        assert (LastStreamSet == PipelineOutput);
        return;
    }

    struct TLVertData {
        size_t Value = 0;
        size_t Overflow = 0;
        size_t PartitionId = 0;
        Z3_ast UnitCost = nullptr;
        Z3_ast Start = nullptr;
        Z3_ast End = nullptr;
    };

    using ThreadLocalDataGraph = adjacency_list<vecS, vecS, bidirectionalS, TLVertData>;

    // This process serves two purposes: (1) generate the initial memory layout for our thread-local
    // streamsets. (2) determine how many the number of pages to assign each streamset based on the
    // number of strides executed by the parition root.

    const auto n = PartitionCount + LastStreamSet - FirstStreamSet + 1U;

    std::vector<unsigned> mapStreamSetToThreadLocal(n, 0);
    std::vector<Rational> unscaledUnitWeight(n, Rational{0});
    std::vector<size_t> overflowStrideAdjustment(n, 0);
    std::vector<unsigned> streamSetPartitionId(n);
    std::vector<unsigned> mappedPartitionId;
    mappedPartitionId.reserve(PartitionCount);

    auto & dl = b.getModule()->getDataLayout();

    const auto bw = b.getBitBlockWidth();

    size_t numOfThreadLocalStreamSets = 0U;


    #ifdef PRINT_Z3_OPTIMIZATION
    errs() << " -- starting thread local layout\n";
    #endif

    size_t unscaledUnitWeightDenomLCM = 1U;

    const auto pageSize = getPageSize();

    for (unsigned partitionId = 0; partitionId < PartitionCount; ++partitionId) {
        const auto firstKernel = FirstKernelInPartition[partitionId];
        const auto firstKernelOfNextPartition = FirstKernelInPartition[partitionId + 1];

        const auto startThreadLocalStreamSetCount = numOfThreadLocalStreamSets;

        Rational zeroExtensionSpace{0,1};
        Rational zeroExtensionOverflow{0,1};

        const auto packedPartitionId = mappedPartitionId.size();

        for (auto kernel = firstKernel; kernel < firstKernelOfNextPartition; ++kernel) {

            for (const auto input : make_iterator_range(in_edges(kernel, mBufferGraph))) {
                const BufferPort & bp = mBufferGraph[input];
                if (LLVM_UNLIKELY(bp.isZeroExtended())) {
                    const auto streamSet = source(input, mBufferGraph);
                    const BufferNode & bn = mBufferGraph[streamSet];
                    Type * const type = bn.Buffer->getType();
                    const size_t typeSize = b.getTypeSize(dl, type);
                    const auto W = bp.Maximum * Rational{typeSize * StrideRepetitionVector[kernel],
                                   bw * pageSize * StrideRepetitionVector[firstKernel]};
                    zeroExtensionSpace = std::max(zeroExtensionSpace, W);

                    const auto strideSize = getKernel(kernel)->getStride();
                    const auto O = (bp.Maximum * StrideRepetitionVector[kernel] + Rational{bp.RequiredOverflowSpace}) / strideSize;
                    zeroExtensionOverflow = std::max(zeroExtensionOverflow, O);
                }
            }

            for (const auto output : make_iterator_range(out_edges(kernel, mBufferGraph))) {
                const auto streamSet = target(output, mBufferGraph);
                const BufferNode & bn = mBufferGraph[streamSet];

                if (bn.isThreadLocal()) {
                    const auto src = mConsumerGraph[streamSet];

                    const BufferPort & bp = mBufferGraph[output];
                    const auto strideSize = getKernel(kernel)->getStride();
                    const auto O = (bp.Maximum * StrideRepetitionVector[kernel] + Rational{bp.RequiredOverflowSpace}) / strideSize;
                    const size_t overflow = ceiling(O);

                    if (src == 0) {
                        const auto k = streamSet - FirstStreamSet;
                        assert (mapStreamSetToThreadLocal[k] == 0);
                        mapStreamSetToThreadLocal[k] = numOfThreadLocalStreamSets;
                        streamSetPartitionId[numOfThreadLocalStreamSets] = packedPartitionId;

                        Type * const type = bn.Buffer->getType();
                        const size_t typeSize = b.getTypeSize(dl, type);
                        const auto W = bp.Maximum * Rational{typeSize * StrideRepetitionVector[kernel],
                                       bw * pageSize * StrideRepetitionVector[firstKernel]};
                        unscaledUnitWeightDenomLCM = boost::lcm(unscaledUnitWeightDenomLCM, W.denominator());
                        unscaledUnitWeight[numOfThreadLocalStreamSets] = W;

                        overflowStrideAdjustment[numOfThreadLocalStreamSets] = overflow;

                        ++numOfThreadLocalStreamSets;
                    } else {
                        assert (mConsumerGraph[src] == 0);
                        const auto j = mapStreamSetToThreadLocal[src - FirstStreamSet];
                        assert (j < numOfThreadLocalStreamSets);
                        overflowStrideAdjustment[j] = std::max(overflowStrideAdjustment[j], overflow);
                    }
                }
            }
        }

        if (zeroExtensionSpace.numerator() > 0) {
            const auto k = LastStreamSet - FirstStreamSet + partitionId + 1U;
            assert (mapStreamSetToThreadLocal[k] == 0);
            mapStreamSetToThreadLocal[k] = numOfThreadLocalStreamSets;
            streamSetPartitionId[numOfThreadLocalStreamSets] = packedPartitionId;
            unscaledUnitWeightDenomLCM = boost::lcm(unscaledUnitWeightDenomLCM, (size_t)zeroExtensionSpace.denominator());
            unscaledUnitWeight[numOfThreadLocalStreamSets] = zeroExtensionSpace;
            overflowStrideAdjustment[numOfThreadLocalStreamSets] = ceiling(zeroExtensionOverflow);
            ++numOfThreadLocalStreamSets;
        }

        if (startThreadLocalStreamSetCount != numOfThreadLocalStreamSets) {
            mappedPartitionId.push_back(partitionId);
        }

    }

    const auto m = PartitionCount + n;

    ThreadLocalPlacementGraph T(m + 1U);

    for (unsigned i = 0; i <= m; ++i) {
        auto & Ti = T[i];
        Ti.OverflowStrideAdjustment = 0;
        Ti.Terminal = false;
    }

    if (numOfThreadLocalStreamSets) {

        ConflictGraph I(numOfThreadLocalStreamSets);

        std::vector<unsigned> remaining(numOfThreadLocalStreamSets, 0);
        std::vector<unsigned> mapThreadLocalToStreamSet(numOfThreadLocalStreamSets, 0);

        for (unsigned partitionId = 0; partitionId < PartitionCount; ++partitionId) {
            const auto firstKernel = FirstKernelInPartition[partitionId];
            const auto firstKernelOfNextPartition = FirstKernelInPartition[partitionId + 1];

            #ifdef PREVENT_THREAD_LOCAL_BUFFERS_FROM_SHARING_MEMORY
            for (auto kernel = firstKernel; kernel < firstKernelOfNextPartition; ++kernel) {
                for (const auto output : make_iterator_range(out_edges(kernel, mBufferGraph))) {
                    const auto streamSet = target(output, mBufferGraph);
                    assert (FirstStreamSet <= streamSet && streamSet <= LastStreamSet);
                    const BufferNode & bn = mBufferGraph[streamSet];
                    if (bn.isThreadLocal()) {
                        const auto j = mapping[streamSet - FirstStreamSet];
                        assert (j < numOfThreadLocalStreamSets);
                        if (LLVM_LIKELY(!bn.isInOutRedirect())) {
                            const auto k = PartitionCount + streamSet - FirstStreamSet;
                            reverse_mapping[j] = k;  assert (k > 0);
                        }
                        remaining[j] = firstKernel + 1;
                    }
                }
            }
            for (unsigned i = 1; i < numOfThreadLocalStreamSets; ++i) {
                const auto id = remaining[i];
                if (id == (firstKernel + 1)) {
                    for (unsigned j = 0; j < i; ++j) {
                        if (remaining[j] == id) {
                            add_edge(j, i, I);
                        }
                    }
                }
            }
            #else

            // Determine which streamsets are no longer alive
            for (auto kernel = firstKernel; kernel < firstKernelOfNextPartition; ++kernel) {

                bool hasZeroExtendedInput = false;

                // TODO: if only one thread-local input is zero extended, we technically do not
                // need to mark the zero extension buffer as conflicting with that input. We do
                // conflict with all other thread-local inputs.

                for (const auto input : make_iterator_range(in_edges(kernel, mBufferGraph))) {
                    const BufferPort & bp = mBufferGraph[input];
                    if (LLVM_UNLIKELY(bp.isZeroExtended())) {
                        assert (mBufferGraph[source(input, mBufferGraph)].isNonThreadLocal());
                        const auto k = LastStreamSet + partitionId + 1U;
                        const auto j = mapStreamSetToThreadLocal[k - FirstStreamSet];
                        assert (mapThreadLocalToStreamSet[j] == 0 || mapThreadLocalToStreamSet[j] == k);
                        mapThreadLocalToStreamSet[j] = k;
                        remaining[j] = 1U;
                        hasZeroExtendedInput = true;
                        break;
                    }
                }

                for (const auto output : make_iterator_range(out_edges(kernel, mBufferGraph))) {
                    const auto streamSet = target(output, mBufferGraph);
                    assert (FirstStreamSet <= streamSet && streamSet <= LastStreamSet);
                    const BufferNode & bn = mBufferGraph[streamSet];
                    if (bn.isThreadLocal()) {
                        const auto src = mConsumerGraph[streamSet];
                        auto v = src ? src : streamSet;
                        assert (FirstStreamSet <= v && v <= streamSet);
                        assert (mBufferGraph[v].isThreadLocal());
                        const auto j = mapStreamSetToThreadLocal[v - FirstStreamSet];
                        assert (j < numOfThreadLocalStreamSets);
                        if (src == 0) {
                            assert (mapThreadLocalToStreamSet[j] == 0);
                            mapThreadLocalToStreamSet[j] = streamSet;
                        }
                        assert (mapThreadLocalToStreamSet[j] != 0);
                        remaining[j] += 1U;
                    }
                }

                // Mark any overlapping allocations in our interval graph.
                for (unsigned i = 1; i != numOfThreadLocalStreamSets; ++i) {
                    if (remaining[i]) {
                        for (unsigned j = 0; j != i; ++j) {
                            if (remaining[j]) {
                                add_edge(j, i, I);
                            }
                        }
                    }
                }

                for (const auto input : make_iterator_range(in_edges(kernel, mBufferGraph))) {
                    const auto streamSet = source(input, mBufferGraph);
                    assert (FirstStreamSet <= streamSet && streamSet <= LastStreamSet);
                    const BufferNode & bn = mBufferGraph[streamSet];
                    if (bn.isThreadLocal()) {
                        const auto src = mConsumerGraph[streamSet];
                        auto v = src ? src : streamSet;
                        assert (FirstStreamSet <= v && v <= streamSet);
                        assert (mBufferGraph[v].isThreadLocal());
                        const auto j = mapStreamSetToThreadLocal[v - FirstStreamSet];
                        assert (j < numOfThreadLocalStreamSets);
                        assert (remaining[j] > 0);
                        remaining[j]--;
                    }
                }

                if (LLVM_UNLIKELY(hasZeroExtendedInput)) {
                    const auto k = LastStreamSet + partitionId + 1U;
                    const auto j = mapStreamSetToThreadLocal[k - FirstStreamSet];
                    assert (mapThreadLocalToStreamSet[j] == k);
                    assert (remaining[j] == 1);
                    remaining[j] = 0U;
                }

                for (const auto output : make_iterator_range(out_edges(kernel, mBufferGraph))) {
                    const auto streamSet = target(output, mBufferGraph);
                    assert (FirstStreamSet <= streamSet && streamSet <= LastStreamSet);
                    const BufferNode & bn = mBufferGraph[streamSet];
                    if (bn.isThreadLocal()) {
                        const auto src = mConsumerGraph[streamSet];
                        auto v = src ? src : streamSet;
                        assert (FirstStreamSet <= v && v <= streamSet);
                        const auto j = mapStreamSetToThreadLocal[v - FirstStreamSet];
                        assert (j < numOfThreadLocalStreamSets);
                        remaining[j] += out_degree(streamSet, mBufferGraph) - 1U;
                    }
                }
            }
            #endif
        }

        #if !defined(NDEBUG) && !defined(PREVENT_THREAD_LOCAL_BUFFERS_FROM_SHARING_MEMORY)
        for (size_t i = 0; i < numOfThreadLocalStreamSets; ++i) {
            assert (remaining[i] == 0);
        }
        #endif

        const auto l = LastStreamSet - FirstStreamSet + 1U;

        ThreadLocalConflictGraph = ThreadLocalConflictGraphType(l);

        for (auto e : make_iterator_range(edges(I))) {
            auto getVertex = [&](const size_t u) {
                assert (u < mapThreadLocalToStreamSet.size());
                const auto v = mapThreadLocalToStreamSet[u];
                assert (v >= FirstStreamSet);
                return v - FirstStreamSet;
            };
            const auto u = getVertex(source(e, I));
            const auto v = getVertex(target(e, I));
            if (u < l && v < l) {
                add_edge(u, v, ThreadLocalConflictGraph);
            }
        }

        #ifdef PRINT_Z3_OPTIMIZATION
        BEGIN_SCOPED_REGION
            auto & out = errs();
            out << "digraph \"" << "I" << "\" {\n";
            for (unsigned i = 0; i < n; ++i) {
                out << "v" << i << " [label=\"";
                out << "S_" << (FirstStreamSet + i);
                out << "\"];\n";
            }
            for (const auto e : make_iterator_range(edges(ThreadLocalConflictGraph))) {
                const auto s = source(e, ThreadLocalConflictGraph);
                const auto t = target(e, ThreadLocalConflictGraph);
                out << "v" << s << " -> v" << t << ";\n";
            }
            out << "}\n\n";
        END_SCOPED_REGION
        #endif

        std::vector<size_t> unitWeight(n + 1);
        for (unsigned i = 0; i < numOfThreadLocalStreamSets; ++i) {
            const auto W = unscaledUnitWeight[i] * unscaledUnitWeightDenomLCM;
            assert (W.denominator() == 1);
            unitWeight[i] = W.numerator();
        }
        unitWeight[n] = 0;

        BufferLayoutOptimizer BA(numOfThreadLocalStreamSets,
                                 mappedPartitionId.size(),
                                 I, unitWeight, streamSetPartitionId,
                                 rng);

        BA.runGA();

        auto O = BA.getResult();

        #ifdef PRINT_Z3_OPTIMIZATION
        errs() << " -- finished thread local layout genetic algorithm phase\n";
        #endif

        const auto intervals = BA.translate(O, rng);
        assert (intervals.size() == numOfThreadLocalStreamSets);

        const auto w = m + PartitionCount + 1U;

        ThreadLocalDataGraph D(w);

        for (unsigned i = 0; i < numOfThreadLocalStreamSets; ++i) {
            const auto streamSet = mapThreadLocalToStreamSet[i];
            assert (FirstStreamSet <= streamSet && streamSet < (LastStreamSet + PartitionCount));
            assert (mapStreamSetToThreadLocal[streamSet - FirstStreamSet] == i);
            const auto j = PartitionCount + streamSet - FirstStreamSet; assert (j < w);
            TLVertData & N = D[j];
            N.Value = unitWeight[i];
            N.Overflow = overflowStrideAdjustment[i];
            const auto k = streamSetPartitionId[i];
            const auto partId = mappedPartitionId[k];
            assert (partId < PartitionCount);
            N.PartitionId = partId;
            assert (T[j].OverflowStrideAdjustment == 0);
            T[j].OverflowStrideAdjustment = overflowStrideAdjustment[i];
            const auto & C = intervals[i];
            #ifndef NDEBUG
            assert (C.upper() > C.lower());
            #endif
            if (C.lower() == 0) {
                add_edge(partId, j, D);
            }
        }

        for (auto e : make_iterator_range(edges(I))) {
            const auto a = source(e, I);
            assert (a < numOfThreadLocalStreamSets);
            const auto & A = intervals[a];
            const auto b = target(e, I);
            assert (b < numOfThreadLocalStreamSets);
            const auto & B = intervals[b];

            assert (disjoint(A, B));

            auto make_edge = [&](const size_t i, const size_t j) {
                const auto u = mapThreadLocalToStreamSet[i];
                assert (u >= FirstStreamSet);
                const auto v = mapThreadLocalToStreamSet[j];
                assert (v >= FirstStreamSet);
                add_edge(PartitionCount + u - FirstStreamSet, PartitionCount + v - FirstStreamSet, D);
            };

            if (A.lower() < B.lower()) {
                assert (A.upper() <= B.lower());
                assert (B.lower() > 0);
                make_edge(a, b);
            } else {
                assert (B.upper() <= A.lower());
                assert (A.lower() > 0);
                make_edge(b, a);
            }
        }

        transitive_reduction_dag(D);

        std::vector<size_t> unvisitedAncestors(m);
        for (auto i = PartitionCount; i < m; ++i) {
            const auto a = in_degree(i, D);
            unvisitedAncestors[i] = a;
            if (a != 0 && out_degree(i, D) == 0) {
                const auto streamSet = FirstStreamSet + i - PartitionCount;
                const TLVertData & N = D[PartitionCount + streamSet - FirstStreamSet];
                add_edge(i, N.PartitionId, D);
            }
        }

        for (size_t i = 0; i < PartitionCount; ++i) {
            unvisitedAncestors[i] = in_degree(i, D);
        }

        #ifdef PRINT_Z3_OPTIMIZATION
        BEGIN_SCOPED_REGION
        auto & out = errs();
        out << "digraph \"" << "D" << "\" {\n";
        for (unsigned i = 0; i < w; ++i) {
            if (degree(i, D) > 0) {
                out << "v" << i << " [label=\"";
                if (i < PartitionCount) {
                    out << "P_" << i;
                } else if (i < n) {
                    out << "S_" << (FirstStreamSet + i - PartitionCount);
                } else if (i < m) {
                    out << "Z_" << (i - n);
                } else {
                    out << 'X';
                }
                out << "\"];\n";
            }
        }

        for (const auto e : make_iterator_range(edges(D))) {
            const auto s = source(e, D);
            const auto t = target(e, D);
            const auto V = Rational{D[t].Value, unscaledUnitWeightDenomLCM};
            out << "v" << s << " -> v" << t <<
                   " [label=\"" << V.numerator() << "/" << V.denominator() << "\"";
            out << "];\n";
        }

        out << "}\n\n";
        END_SCOPED_REGION
        #endif

        const auto cfg = Z3_mk_config();
        Z3_set_param_value(cfg, "model", "true");
        Z3_set_param_value(cfg, "proof", "false");

        const auto ctx = Z3_mk_context(cfg);
        Z3_del_config(cfg);
        const auto solver = Z3_mk_solver(ctx);
        Z3_solver_inc_ref(ctx, solver);

        auto hard_assert = [&](Z3_ast c) {
            Z3_solver_assert(ctx, solver, c);
        };

        const auto intType = Z3_mk_int_sort(ctx);

        auto constant_int = [&](const size_t value) {
            return Z3_mk_int(ctx, value, intType);
        };

        auto add = [&](Z3_ast X, Z3_ast Y) {
            assert (X && Y);
            Z3_ast args[2] = { X, Y };
            return Z3_mk_add(ctx, 2, args);
        };

        auto multiply =[&](Z3_ast X, Z3_ast Y) {
            assert (X && Y);
            Z3_ast args[2] = { X, Y };
            return Z3_mk_mul(ctx, 2, args);
        };

        auto z3_max = [&](Z3_ast X, Z3_ast Y) {
            assert (X && Y);
            return Z3_mk_ite(ctx, Z3_mk_gt(ctx, X, Y), X, Y);
        };

        auto check = [&]() -> Z3_lbool {
            return Z3_solver_check(ctx, solver);
        };

        auto neverGreaterThan = [&](Z3_ast a, Z3_ast b) -> bool {
            Z3_solver_push(ctx, solver);
            hard_assert(Z3_mk_gt(ctx, a, b));
            const Z3_lbool r = check();
            Z3_solver_pop(ctx, solver, 1);
            return r == Z3_L_FALSE;
        };

        const auto DENOM_LCM = Z3_mk_int(ctx, unscaledUnitWeightDenomLCM, intType);

        const auto z3_ZERO = Z3_mk_int(ctx, 0, intType);

        auto round_up_to_nearest_lcm_of_denom_multiple = [&](Z3_ast value) {
            auto a = Z3_mk_mod(ctx, value, DENOM_LCM);
            Z3_ast args1[2] = { DENOM_LCM, a };
            auto b = Z3_mk_sub(ctx, 2, args1);
            auto c = Z3_mk_mod(ctx, b, DENOM_LCM);
            Z3_ast args2[2] = { value, c };
            return Z3_mk_add(ctx, 2, args2);
        };

        std::queue<Vertex> S;

        std::vector<Z3_ast> controlVar(PartitionCount, nullptr);

        SmallVector<Z3_ast, 16> startOffset;
        SmallVector<Z3_ast, 16> endOffset;

        // Because each buffer is paged aligned, different num of stride counts can change which thread-local buffer
        // starts after different prior buffers. Reason out for all possible num of stride counts which buffers we
        // need to calculate the end offset of first in order to determine what the start position is.

        for (unsigned partId = 0; partId < PartitionCount; ++partId) {
            if (out_degree(partId, D) > 0) {

                const Z3_ast rv = Z3_mk_fresh_const(ctx, nullptr, intType);
                // the initial stride count is always set to the max during allocation
                const auto max = MaximumNumOfStrides[FirstKernelInPartition[partId]];

                hard_assert(Z3_mk_ge(ctx, rv, constant_int(max)));
                controlVar[partId] = rv;

                auto & Dp = D[partId];
                Dp.UnitCost = z3_ZERO;
                Dp.Start = z3_ZERO;
                Dp.End = z3_ZERO;
                Dp.Overflow = max;

                assert (S.empty());
                for (auto u = partId;;) {

                    assert (D[u].End);

                    for (auto e : make_iterator_range(out_edges(u, D))) {
                        const auto v = target(e, D);
                        assert (PartitionCount <= v || v == partId);
                        assert (v != u);
                        auto & U = unvisitedAncestors[v];
                        assert (U > 0);
                        if (--U == 0) {

                             auto & Dv = D[v];

                             const auto d = in_degree(v, D); assert (d > 0);

                             ThreadLocalDataGraph::in_edge_iterator ei_begin, ei_end;
                             std::tie(ei_begin, ei_end) = in_edges(v, D);

                             Z3_ast maxPriorEnd = nullptr;


                             endOffset.resize(d);

                             if (d == 1) {

                                 const auto s = source(*ei_begin, D);
                                 assert (s < num_vertices(D));
                                 maxPriorEnd = D[s].End;
                                 endOffset[0] = maxPriorEnd;

                             } else {

                                 auto canRemoveFirst = [&](const size_t i, const size_t j) -> bool {

                                     assert (startOffset[i] && endOffset[i]);
                                     assert (startOffset[j] && endOffset[j]);

                                     // If I cannot start within the range of J and I cannot end
                                     // after J ends, return true.

                                     Z3_solver_push(ctx, solver);
                                     Z3_ast args[2];
                                     args[0] = Z3_mk_ge(ctx, startOffset[i], startOffset[j]);
                                     args[1] = Z3_mk_lt(ctx, startOffset[i], endOffset[j]);
                                     args[0] = Z3_mk_and(ctx, 2, args);
                                     args[1] = Z3_mk_gt(ctx, endOffset[i], endOffset[j]);
                                     hard_assert(Z3_mk_or(ctx, 2, args));
                                     const Z3_lbool r = check();
                                     Z3_solver_pop(ctx, solver, 1);
                                     return r == Z3_L_FALSE;
                                 };

                                 size_t c = 0;
                                 startOffset.resize(d);
                                 for (auto ei = ei_begin; ei != ei_end; ++ei, ++c) {
                                     const auto s = source(*ei, D);
                                     const auto & Ds = D[s];
                                     startOffset[c] = Ds.Start; assert (Ds.Start);
                                     endOffset[c] = Ds.End; assert (Ds.End);
                                 }
                                 assert (c == d);

                                 assert (check() == Z3_L_TRUE);

                                 for (size_t i = 1; i < d; ++i ) {
                                     assert (endOffset[i]);
                                     for (size_t j = 0; j < i; ++j) {
                                         if (endOffset[j]) {
                                             if (canRemoveFirst(i, j)) {
                                                 endOffset[i] = nullptr;
                                                 break;
                                             }

                                             if (canRemoveFirst(j, i)) {
                                                 endOffset[j] = nullptr;
                                             }
                                         }
                                     }
                                 }

                                for (size_t i = 0; i < d; ++i ) {
                                    if (endOffset[i]) {
                                        if (maxPriorEnd) {
                                            maxPriorEnd = z3_max(maxPriorEnd, endOffset[i]);
                                        } else {
                                            maxPriorEnd = endOffset[i];
                                        }
                                    }
                                }

                            }

                            assert (maxPriorEnd);

                            if (LLVM_UNLIKELY(v == partId)) {

                                for (size_t i = 0; i < d; ++i ) {
                                    if (endOffset[i]) {
                                        const auto s = source(*(ei_begin + i), D);
                                        assert (s < m);
                                        add_edge(s, m, D);
                                    }
                                }

                                Dv.End = maxPriorEnd;

                            } else {

                                Rational V{Dv.Value, unscaledUnitWeightDenomLCM};
                                assert (V.numerator() > 0);
                                for (size_t i = 0; i < d; ++i ) {
                                    if (endOffset[i]) {
                                        const auto s = source(*(ei_begin + i), D);
                                        assert (s < m);
                                        assert (v <= m);
                                        add_edge(s, v, V, T);
                                    }
                                }

                                Z3_ast cost = add(rv, constant_int(Dv.Overflow));
                                cost = multiply(cost, constant_int(Dv.Value));
                                cost = round_up_to_nearest_lcm_of_denom_multiple(cost);
                                Dv.UnitCost = cost;
                                Dv.Start = maxPriorEnd;
                                Dv.End = add(maxPriorEnd, cost);

                                S.push(v);

                            }

                        }

                    }
                    if (S.empty()) {
                        break;
                    }
                    u = S.front();
                    S.pop();

                }
            }
        }

        assert (check() == Z3_L_TRUE);

        const auto d = in_degree(m, D); assert (d > 0);

        ThreadLocalDataGraph::in_edge_iterator ei_begin, ei_end;
        std::tie(ei_begin, ei_end) = in_edges(m, D);

        endOffset.resize(d);

        SmallVector<Z3_ast, 16> cVar(d);

        size_t c = 0;
        for (auto ei = ei_begin; ei != ei_end; ++ei, ++c) {
            const auto s = source(*ei, D);
            T[s].Terminal = true;
            const auto & Ds = D[s];
            endOffset[c] = Ds.End;
            assert (endOffset[c]);
            assert (endOffset[c] != z3_ZERO);
            cVar[c] = controlVar[Ds.PartitionId];
        }
        assert (c == d);

        for (size_t i = 1; i < d; ++i ) {
            assert (cVar[i]);
            for (size_t j = 0; j < i; ++j) {
                if (cVar[j]) {
                    Z3_solver_push(ctx, solver);
                    hard_assert(Z3_mk_eq(ctx, cVar[i], cVar[j]));
                    if (neverGreaterThan(endOffset[i], endOffset[j])) {
                        cVar[i] = nullptr;
                        Z3_solver_pop(ctx, solver, 1);
                        break;
                    }
                    if (neverGreaterThan(endOffset[j], endOffset[i])) {
                        cVar[j] = nullptr;
                    }
                    Z3_solver_pop(ctx, solver, 1);
                }
            }
        }

        const auto r = check();
        assert (r == Z3_L_TRUE);

        const auto model = Z3_solver_get_model(ctx, solver);
        Z3_model_inc_ref(ctx, model);

        Z3_int64 minSegmentSize = 0;

        for (size_t i = 0; i < d; ++i ) {
            if (cVar[i]) {

                Z3_ast value;
                if (LLVM_UNLIKELY(Z3_model_eval(ctx, model, cVar[i], Z3_L_TRUE, &value) != Z3_L_TRUE)) {
                    report_fatal_error("Unexpected Z3 error when attempting to obtain value from model!");
                }

                Z3_int64 num;
                if (LLVM_UNLIKELY(Z3_get_numeral_int64(ctx, value, &num) != Z3_L_TRUE)) {
                    report_fatal_error("Unexpected Z3 error when attempting to convert model value to number!");
                }

                minSegmentSize = std::max(minSegmentSize, num);

                const auto s = source(*(ei_begin + i), D);
                assert (s < m);
                assert (m < num_vertices(T));
                add_edge(s, m, Rational{0,1}, T);
            }
        }



        Z3_model_dec_ref(ctx, model);
        Z3_solver_dec_ref(ctx, solver);
        Z3_del_context(ctx);

        assert (in_degree(m, T) > 0);
        assert (minSegmentSize > 0);

        MinimumThreadLocalSegmentSize = minSegmentSize;

        #ifdef PRINT_Z3_OPTIMIZATION
        BEGIN_SCOPED_REGION
        for (unsigned i = 0; i < PartitionCount; ++i) {
            clear_in_edges(i, D);
        }
        auto & out = errs();
        out << "digraph \"" << "T" << "\" {\n";
        for (unsigned i = 0; i < m; ++i) {
            if (degree(i, D) > 0) {
                out << "v" << i << " [label=\"";
                if (i < PartitionCount) {
                    out << "P_" << i;
                } else if (i < n) {
                    out << "S_" << (FirstStreamSet + i - PartitionCount);
                    const auto & Ti = T[i];
                    if (Ti.OverflowStrideAdjustment) {
                        out << '+' << Ti.OverflowStrideAdjustment;
                    }
                    if (Ti.Terminal) {
                        out << '*';
                    }
                } else if (i < m) {
                    out << "Z_" << (i - n);
                    const auto & Ti = T[i];
                    if (Ti.OverflowStrideAdjustment) {
                        out << '+' << Ti.OverflowStrideAdjustment;
                    }
                } else {
                    out << "X";
                }
                out << "\"];\n";
            }
        }

        for (const auto e : make_iterator_range(edges(D))) {
            const auto s = source(e, D);
            const auto t = target(e, D);
            const auto V = Rational{D[t].Value, unscaledUnitWeightDenomLCM};
            out << "v" << s << " -> v" << t <<
                   " [label=\"" << V.numerator() << "/" << V.denominator() << "\"";
            if (!edge(s, t, T).second) {
                out << ", color=\"red\"";
            }
            out << "];\n";
        }

        for (const auto e : make_iterator_range(edges(T))) {
            const auto s = source(e, T);
            const auto t = target(e, T);
            if (!edge(s, t, D).second) {
                const auto & V = T[e];
                out << "v" << s << " -> v" << t <<
                       " [label=\"" << V.numerator() << "/" << V.denominator() << "\"";
                out << ", color=\"green\"";
                out << "];\n";

            }
        }

        out << "}\n\n";
        END_SCOPED_REGION
        #endif
    }

    ThreadLocalPlacement = T;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief updateInterPartitionThreadLocalBuffers
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineAnalysis::updateInterPartitionThreadLocalBuffers() {

    // update threadlocal status of sources for truncated buffers

    if (LLVM_UNLIKELY(FirstStreamSet == PipelineOutput)) {
        assert (LastStreamSet == PipelineOutput);
        return;
    }

    if (LLVM_UNLIKELY(DebugOptionIsSet(codegen::DisableThreadLocalStreamSets))) {
        for (auto streamSet = FirstStreamSet; streamSet <= LastStreamSet; ++streamSet) {
            BufferNode & bn = mBufferGraph[streamSet];
            if (bn.isThreadLocal() && bn.isInternal() && bn.isOwned()) {
                bn.Locality = GloballyShared;
            }
        }
        return;
    }

    if (LLVM_UNLIKELY(!codegen::ThreadLocalPermittedOptions.empty())) {

        for (auto streamSet = FirstStreamSet; streamSet <= LastStreamSet; ++streamSet) {
            BufferNode & bn = mBufferGraph[streamSet];
            if (bn.isThreadLocal() && bn.isInternal() && bn.isOwned()) {
                bn.Locality = GloballyShared;
            }
        }

        const auto permitted = parseCommaDelimitedList(codegen::ThreadLocalPermittedOptions);

        for (auto streamSet = FirstStreamSet; streamSet <= LastStreamSet; ++streamSet) {
            const auto f = permitted.find(streamSet);
            if (f == permitted.end()) {
                for (auto id = streamSet;;) {
                    BufferNode & bn = mBufferGraph[id];
                    if (LLVM_LIKELY(!bn.isConstant())) {
                        bn.Locality = BufferLocality::ThreadLocal;
                    }
                    if (LLVM_LIKELY(in_degree(id, InOutStreamSetReplacement) == 0)) {
                        break;
                    }
                    id = parent(id, InOutStreamSetReplacement);
                }
            }
        }
    }

    for (auto streamSet = FirstStreamSet; streamSet <= LastStreamSet; ++streamSet) {
        BufferNode & bn = mBufferGraph[streamSet];

        if (bn.isExternal() || bn.isUnowned() || bn.isConstant()) {
            continue;
        }

        const auto producer = parent(streamSet, mBufferGraph);
        const auto partId = KernelPartitionId[producer];
        for (const auto input : make_iterator_range(out_edges(streamSet, mBufferGraph))) {
            const auto consumer = target(input, mBufferGraph);
            if (partId != KernelPartitionId[consumer]) {
                bn.Locality = BufferLocality::GloballyShared;
                break;
            }
        }
    }

    if (num_edges(InOutStreamSetReplacement) > 0) {
        for (auto streamSet = FirstStreamSet; streamSet <= LastStreamSet; ++streamSet) {
            if (LLVM_UNLIKELY(out_degree(streamSet, InOutStreamSetReplacement) > 0 && in_degree(streamSet, InOutStreamSetReplacement) == 0)) {
                auto toCheck = streamSet;
                bool isNonThreadLocal = false;
                for (;;) {
                    assert (FirstStreamSet <= toCheck && toCheck <= LastStreamSet);
                    const BufferNode & bn = mBufferGraph[toCheck];
                    if (bn.isNonThreadLocal()) {
                        isNonThreadLocal = !bn.isConstant() && !bn.isExternal();
                        break;
                    }
                    if (out_degree(toCheck, InOutStreamSetReplacement) == 0) {
                        break;
                    }
                    toCheck = child(toCheck, InOutStreamSetReplacement);
                }

                if (isNonThreadLocal) {
                    auto toUpdate = streamSet;
                    for (;;) {
                        assert (FirstStreamSet <= toUpdate && toUpdate <= LastStreamSet);
                        BufferNode & bn = mBufferGraph[toUpdate];
                        assert (!bn.isConstant() && !bn.isExternal());
                        bn.Locality = BufferLocality::GloballyShared;
                        if (out_degree(toUpdate, InOutStreamSetReplacement) == 0) {
                            break;
                        }
                        toUpdate = child(toUpdate, InOutStreamSetReplacement);
                    }
                }
            }
        }
    }

recheck_truncations:

    bool anyChanges = false;

    for (auto streamSet = FirstStreamSet; streamSet <= LastStreamSet; ++streamSet) {
        BufferNode & bn = mBufferGraph[streamSet];

        if (LLVM_UNLIKELY(bn.isTruncated())) {
            for (auto ref : make_iterator_range(in_edges(streamSet, mStreamGraph))) {
                const auto & v = mStreamGraph[ref];
                if (v.Reason == ReasonType::Reference) {
                    const auto srcStreamSet = source(ref, mBufferGraph);
                    assert (srcStreamSet >= FirstStreamSet && srcStreamSet <= LastStreamSet);
                    BufferNode & sn = mBufferGraph[srcStreamSet];
                    if (sn.isNonThreadLocal()) {
                        anyChanges |= (bn.Locality != sn.Locality);
                        bn.Locality = sn.Locality;
                    } else if (bn.isNonThreadLocal()) {
                        anyChanges |= (bn.Locality != sn.Locality);
                        sn.Locality = bn.Locality;
                    }
                    break;
                }
            }
        }
    }

    if (anyChanges) goto recheck_truncations;

}

} // end of kernel namespace
