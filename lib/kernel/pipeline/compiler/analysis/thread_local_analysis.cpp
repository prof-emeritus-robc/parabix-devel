#include "pipeline_analysis.hpp"
#include "evolutionary_algorithm.hpp"
#include "lexographic_ordering.hpp"
#include <boost/icl/interval_set.hpp>
#include <boost/integer/common_factor.hpp>
#include <toolchain/toolchain.h>
#include <queue>

// #define PRINT_THREAD_LOCAL_LAYOUT

using boost::icl::interval_set;

namespace kernel {

// TODO: nested pipeline kernels could report how much internal memory they require
// and reason about that here (and in the scheduling phase)

constexpr static unsigned BUFFER_SIZE_INIT_POPULATION_SIZE = 15;

constexpr static unsigned BUFFER_SIZE_POPULATION_SIZE = 30;

constexpr static unsigned BUFFER_SIZE_GA_STALLS = 50;

constexpr static unsigned BUFFER_SIZE_GA_MAX_GENERATIONS = 10 * BUFFER_SIZE_GA_STALLS;

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
                                             BUFFER_SIZE_INIT_POPULATION_SIZE,
                                             BUFFER_SIZE_GA_MAX_GENERATIONS,
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
 * @brief ThreadLocalOffsets
 *
 * The start and end offsets of each thread-local buffer, as functions of the stride counts of the partition
 * roots. Each buffer's cost is ceil_L(Value * (r + Overflow)), where r is the stride count of the partition
 * whose traversal placed it and ceil_L rounds up to a multiple of L, the unit weight denominator LCM. An offset
 * is the maximum of a set of linear terms over two kinds of variables:
 *
 *   - the stride count of each partition root (Var < PartitionCount), bounded below by the number of strides
 *     at which that partition's buffers are allocated and unbounded above;
 *
 *   - the slack that rounds one buffer's cost up to a multiple of L (Var >= PartitionCount), which is
 *     (-Value * (r + Overflow)) mod L for that buffer: a function of r with a period that divides L.
 *
 * Every comparison asks whether one offset is at most another for every assignment of the stride counts, and
 * holds if each term of the first is at most some term of the second. A pair of terms that depends on a single
 * stride count is decided exactly by evaluating their difference over one period of its slacks: the
 * difference changes by its slope at each period, so it is greatest in the first. A pair that depends on
 * several stride counts bounds each slack in [0, L - 1] independently. Either way the answer is sound; the
 * second, and comparing offsets term by term, are incomplete.
 *
 * Every term is a multiple of L: offsets start at zero and each cost is rounded up to a multiple of it. So a
 * difference of two terms that is at most L - 1 is at most 0, which recovers much of the precision lost by
 * bounding each slack separately.
 ** ------------------------------------------------------------------------------------------------------------- */
namespace {

struct LinearTerm {
    flat_map<unsigned, int64_t> Coefficient;
    int64_t Constant = 0;
};

using SymbolicOffset = SmallVector<LinearTerm, 2>;

class ThreadLocalOffsets {
public:

    static constexpr unsigned NoAlias = std::numeric_limits<unsigned>::max();

    ThreadLocalOffsets(const unsigned partitionCount, std::vector<int64_t> && minimumStrides, const int64_t lcm)
    : PartitionCount(partitionCount)
    , MinimumStrides(std::move(minimumStrides))
    , LCM(lcm) {

    }

    static SymbolicOffset zero() {
        return SymbolicOffset(1);
    }

    // The cost of the buffer with the given id: ceil_L(value * (r + overflow)) for the stride count r of partitionId.
    LinearTerm cost(const unsigned id, const unsigned partitionId, const int64_t value, const int64_t overflow) {
        LinearTerm C;
        C.Coefficient[partitionId] = value;
        C.Constant = value * overflow;
        if (value % LCM == 0) {
            // value * r is a multiple of L, so the rounding is constant
            C.Constant += slack(C.Constant);
        } else {
            const auto var = PartitionCount + id;
            Slack.emplace(var, SlackSource{partitionId, value, overflow});
            C.Coefficient[var] = 1;
        }
        return C;
    }

    static SymbolicOffset add(const SymbolicOffset & X, const LinearTerm & cost) {
        SymbolicOffset R(X);
        for (auto & t : R) {
            for (const auto & c : cost.Coefficient) {
                t.Coefficient[c.first] += c.second;
            }
            t.Constant += cost.Constant;
        }
        return R;
    }

    // The maximum of X and Y, without any term that another term never falls below.
    SymbolicOffset max(const SymbolicOffset & X, const SymbolicOffset & Y) const {
        SymbolicOffset R(X);
        for (const auto & t : Y) {
            if (std::none_of(R.begin(), R.end(), [&](const LinearTerm & s) { return atMost(t, s, 0, NoAlias, NoAlias); })) {
                R.erase(std::remove_if(R.begin(), R.end(), [&](const LinearTerm & s) { return atMost(s, t, 0, NoAlias, NoAlias); }), R.end());
                R.push_back(t);
            }
        }
        return R;
    }

    // Is X + gap <= Y for every assignment? If alias != NoAlias, the stride count of partition alias is
    // taken to equal that of partition target.
    bool atMost(const SymbolicOffset & X, const SymbolicOffset & Y, const int64_t gap,
                const unsigned alias = NoAlias, const unsigned target = NoAlias) const {
        for (const auto & t : X) {
            if (std::none_of(Y.begin(), Y.end(), [&](const LinearTerm & s) { return atMost(t, s, gap, alias, target); })) {
                return false;
            }
        }
        return true;
    }

private:

    struct SlackSource {
        unsigned PartitionId;
        int64_t Value;
        int64_t Overflow;
    };

    // the amount that rounds x >= 0 up to a multiple of L
    int64_t slack(const int64_t x) const {
        return (LCM - x % LCM) % LCM;
    }

    bool atMost(const LinearTerm & A, const LinearTerm & B, const int64_t gap, const unsigned alias, const unsigned target) const {
        auto rename = [&](const unsigned var) {
            return (var == alias) ? target : var;
        };
        flat_map<unsigned, int64_t> stride;
        flat_map<unsigned, int64_t> slackCoefficient;
        auto accumulate = [&](const LinearTerm & X, const int64_t sign) {
            for (const auto & c : X.Coefficient) {
                if (c.first < PartitionCount) {
                    stride[rename(c.first)] += sign * c.second;
                } else {
                    slackCoefficient[c.first] += sign * c.second;
                }
            }
        };
        accumulate(A, 1);
        accumulate(B, -1);
        const int64_t constant = A.Constant - B.Constant;

        auto minimumStrides = [&](const unsigned partitionId) {
            auto lb = MinimumStrides[partitionId];
            if (partitionId == target) {
                lb = std::max(lb, MinimumStrides[alias]);
            }
            return lb;
        };

        // A - B is a multiple of L, so it is at most the greatest multiple of L not above any bound on it.
        auto withinBound = [&](const int64_t max) {
            const auto q = (max >= 0) ? (max / LCM) : -((LCM - 1 - max) / LCM);
            return q * LCM + gap <= 0;
        };

        unsigned var = NoAlias;
        bool singleStrideCount = true;
        auto uses = [&](const unsigned partitionId) {
            if (var == NoAlias) {
                var = partitionId;
            } else if (var != partitionId) {
                singleStrideCount = false;
            }
        };
        for (const auto & c : stride) {
            if (c.second != 0) {
                uses(c.first);
            }
        }
        for (const auto & c : slackCoefficient) {
            if (c.second != 0) {
                uses(rename(Slack.find(c.first)->second.PartitionId));
            }
        }

        if (singleStrideCount) {
            if (var == NoAlias) {
                return withinBound(constant);
            }
            const auto f = stride.find(var);
            const int64_t slope = (f == stride.end()) ? 0 : f->second;
            if (slope > 0) {
                return false;
            }
            // each slack repeats after L / gcd(Value, L) strides
            int64_t period = 1;
            for (const auto & c : slackCoefficient) {
                if (c.second != 0) {
                    const auto & s = Slack.find(c.first)->second;
                    period = boost::lcm<int64_t>(period, LCM / boost::gcd<int64_t>(s.Value, LCM));
                }
            }
            const auto lb = minimumStrides(var);
            for (int64_t r = lb; r < lb + period; ++r) {
                int64_t diff = constant + slope * r;
                for (const auto & c : slackCoefficient) {
                    if (c.second != 0) {
                        const auto & s = Slack.find(c.first)->second;
                        diff += c.second * slack(s.Value * (r + s.Overflow));
                    }
                }
                if (diff + gap > 0) {
                    return false;
                }
            }
            return true;
        }

        // With no positive coefficient on a stride count, A - B is greatest at the least stride counts and
        // the largest slack of each positive coefficient.
        int64_t max = constant;
        for (const auto & c : stride) {
            if (c.second > 0) {
                return false;
            }
            max += c.second * minimumStrides(c.first);
        }
        for (const auto & c : slackCoefficient) {
            if (c.second > 0) {
                max += c.second * (LCM - 1);
            }
        }
        return withinBound(max);
    }

    const unsigned PartitionCount;
    const std::vector<int64_t> MinimumStrides;
    const int64_t LCM;
    flat_map<unsigned, SlackSource> Slack;
};

}

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
        SymbolicOffset Start;
        SymbolicOffset End;
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


    #ifdef PRINT_THREAD_LOCAL_LAYOUT
    errs() << " -- starting thread local layout\n";
    #endif

    size_t unscaledUnitWeightDenomLCM = 1U;

    const auto pageSize = CBuilder::PAGE_SIZE;

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
                    Type * const type = bn.Buffer->getType(b);
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

                        Type * const type = bn.Buffer->getType(b);
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

        #ifdef PRINT_THREAD_LOCAL_LAYOUT
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

        #ifdef PRINT_THREAD_LOCAL_LAYOUT
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

        #ifdef PRINT_THREAD_LOCAL_LAYOUT
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

        // the initial stride count is always set to the max during allocation
        std::vector<int64_t> minimumStrides(PartitionCount, 0);
        for (unsigned partId = 0; partId < PartitionCount; ++partId) {
            minimumStrides[partId] = MaximumNumOfStrides[FirstKernelInPartition[partId]];
        }
        ThreadLocalOffsets Offsets(PartitionCount, std::move(minimumStrides), (int64_t)unscaledUnitWeightDenomLCM);

        std::queue<Vertex> S;

        SmallVector<SymbolicOffset, 16> startOffset;
        SmallVector<SymbolicOffset, 16> endOffset;
        SmallVector<bool, 16> keepEnd;

        // Because each buffer is paged aligned, different num of stride counts can change which thread-local buffer
        // starts after different prior buffers. Reason out for all possible num of stride counts which buffers we
        // need to calculate the end offset of first in order to determine what the start position is.

        size_t minSegmentSize = 0;

        for (unsigned partId = 0; partId < PartitionCount; ++partId) {
            if (out_degree(partId, D) > 0) {

                const auto max = MaximumNumOfStrides[FirstKernelInPartition[partId]];

                // Every partition's offsets are compared assuming its stride count is at least the number at
                // which its buffers were allocated; at run time one segment size drives all of them.
                minSegmentSize = std::max<size_t>(minSegmentSize, max);

                auto & Dp = D[partId];
                Dp.Start = ThreadLocalOffsets::zero();
                Dp.End = ThreadLocalOffsets::zero();
                Dp.Overflow = max;

                assert (S.empty());
                for (auto u = partId;;) {

                    assert (!D[u].End.empty());

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

                             startOffset.resize(d);
                             endOffset.resize(d);
                             keepEnd.assign(d, true);

                             size_t c = 0;
                             for (auto ei = ei_begin; ei != ei_end; ++ei, ++c) {
                                 const auto s = source(*ei, D);
                                 assert (s < num_vertices(D));
                                 const auto & Ds = D[s];
                                 assert (!Ds.Start.empty() && !Ds.End.empty());
                                 startOffset[c] = Ds.Start;
                                 endOffset[c] = Ds.End;
                             }
                             assert (c == d);

                             // If I cannot start within the range of J and I cannot end after J ends, the
                             // start of this buffer never depends on the end of I.
                             auto canRemoveFirst = [&](const size_t i, const size_t j) -> bool {
                                 return Offsets.atMost(endOffset[i], endOffset[j], 0) &&
                                     (Offsets.atMost(startOffset[i], startOffset[j], 1) ||
                                      Offsets.atMost(endOffset[j], startOffset[i], 0));
                             };

                             for (size_t i = 1; i < d; ++i ) {
                                 for (size_t j = 0; j < i; ++j) {
                                     if (keepEnd[j]) {
                                         if (canRemoveFirst(i, j)) {
                                             keepEnd[i] = false;
                                             break;
                                         }
                                         if (canRemoveFirst(j, i)) {
                                             keepEnd[j] = false;
                                         }
                                     }
                                 }
                             }

                             SymbolicOffset maxPriorEnd;
                             for (size_t i = 0; i < d; ++i ) {
                                 if (keepEnd[i]) {
                                     if (maxPriorEnd.empty()) {
                                         maxPriorEnd = endOffset[i];
                                     } else {
                                         maxPriorEnd = Offsets.max(maxPriorEnd, endOffset[i]);
                                     }
                                 }
                             }

                            assert (!maxPriorEnd.empty());

                            if (LLVM_UNLIKELY(v == partId)) {

                                for (size_t i = 0; i < d; ++i ) {
                                    if (keepEnd[i]) {
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
                                    if (keepEnd[i]) {
                                        const auto s = source(*(ei_begin + i), D);
                                        assert (s < m);
                                        assert (v <= m);
                                        add_edge(s, v, V, T);
                                    }
                                }

                                Dv.Start = maxPriorEnd;
                                Dv.End = ThreadLocalOffsets::add(maxPriorEnd, Offsets.cost(v, partId, Dv.Value, Dv.Overflow));

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

        const auto d = in_degree(m, D); assert (d > 0);

        ThreadLocalDataGraph::in_edge_iterator ei_begin, ei_end;
        std::tie(ei_begin, ei_end) = in_edges(m, D);

        endOffset.resize(d);

        SmallVector<unsigned, 16> cPartition(d);
        keepEnd.assign(d, true);

        size_t c = 0;
        for (auto ei = ei_begin; ei != ei_end; ++ei, ++c) {
            const auto s = source(*ei, D);
            T[s].Terminal = true;
            const auto & Ds = D[s];
            endOffset[c] = Ds.End;
            assert (!endOffset[c].empty());
            cPartition[c] = Ds.PartitionId;
        }
        assert (c == d);

        // Among the terminal buffers, keep only those whose end may be the greatest when their
        // partitions' stride counts are equal.
        for (size_t i = 1; i < d; ++i ) {
            for (size_t j = 0; j < i; ++j) {
                if (keepEnd[j]) {
                    const auto same = (cPartition[i] == cPartition[j]);
                    const auto alias = same ? ThreadLocalOffsets::NoAlias : cPartition[j];
                    const auto target = same ? ThreadLocalOffsets::NoAlias : cPartition[i];
                    if (Offsets.atMost(endOffset[i], endOffset[j], 0, alias, target)) {
                        keepEnd[i] = false;
                        break;
                    }
                    if (Offsets.atMost(endOffset[j], endOffset[i], 0, alias, target)) {
                        keepEnd[j] = false;
                    }
                }
            }
        }

        for (size_t i = 0; i < d; ++i ) {
            if (keepEnd[i]) {
                const auto s = source(*(ei_begin + i), D);
                assert (s < m);
                assert (m < num_vertices(T));
                add_edge(s, m, Rational{0,1}, T);
            }
        }

        assert (in_degree(m, T) > 0);
        assert (minSegmentSize > 0);

        MinimumThreadLocalSegmentSize = minSegmentSize;

        #ifdef PRINT_THREAD_LOCAL_LAYOUT
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
            // A consumer with a LookBehind reads items of the prior segment, which another
            // thread may have produced; a thread-local buffer would not hold them.
            if (partId != KernelPartitionId[consumer] || mBufferGraph[input].LookBehind != 0) {
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
