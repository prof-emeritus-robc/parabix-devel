#include "evolutionary_algorithm.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>

using Candidate = kernel::PermutationBasedEvolutionaryAlgorithm::Candidate;

template<typename T>
T abs_subtract(const T a, const T b) {
    if (a < b) {
        return b - a;
    } else {
        return a - b;
    }
}

namespace {

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief ParallelFor
 *
 * Runs task(worker, i) for every i < n on a fixed set of threads (the caller being worker 0) and returns once all
 * are done.  Which worker runs which item varies from run to run, so tasks must not depend on it.
 ** ------------------------------------------------------------------------------------------------------------- */
class ParallelFor {
public:

    using Task = llvm::function_ref<void(unsigned worker, size_t i)>;

    explicit ParallelFor(const unsigned threadCount) {
        for (unsigned w = 1; w < threadCount; ++w) {
            threads.emplace_back([this, w]() { work(w); });
        }
    }

    ~ParallelFor() {
        {
            std::lock_guard<std::mutex> L(lock);
            stop = true;
        }
        start.notify_all();
        for (auto & t : threads) {
            t.join();
        }
    }

    void run(const size_t n, Task task) {
        if (n == 0) return;
        {
            std::lock_guard<std::mutex> L(lock);
            current = &task;
            count = n;
            next = 0;
            busy = threads.size();
            ++round;
        }
        start.notify_all();
        drain(0, task, n);
        std::unique_lock<std::mutex> L(lock);
        done.wait(L, [this]() { return busy == 0; });
        current = nullptr;
    }

private:

    void drain(const unsigned worker, Task task, const size_t n) {
        for (;;) {
            const auto i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= n) break;
            task(worker, i);
        }
    }

    void work(const unsigned worker) {
        unsigned seen = 0;
        for (;;) {
            Task * task;
            size_t n;
            {
                std::unique_lock<std::mutex> L(lock);
                start.wait(L, [&]() { return stop || round != seen; });
                if (stop) return;
                seen = round;
                task = current;
                n = count;
            }
            drain(worker, *task, n);
            {
                std::lock_guard<std::mutex> L(lock);
                if (--busy == 0) {
                    done.notify_one();
                }
            }
        }
    }

    std::vector<std::thread> threads;
    std::mutex lock;
    std::condition_variable start;
    std::condition_variable done;
    Task * current = nullptr;
    size_t count = 0;
    std::atomic<size_t> next{0};
    size_t busy = 0;
    unsigned round = 0;
    bool stop = false;
};

}

namespace kernel {

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief runGA
 *
 * The search is deterministic: for a given problem and random seed it reaches the same result whatever the number
 * of threads and however they are scheduled, so that the code generated from it is reproducible (which -VerifyCache
 * relies on).  Candidates are made on this thread; each batch of them is repaired and evaluated in parallel, but
 * every candidate with a generator seeded for it alone, and the results are gathered in the order the candidates
 * were made.  The search ends when it stalls or after maxGenerations generations, never on a time limit.
 ** ------------------------------------------------------------------------------------------------------------- */
const PermutationBasedEvolutionaryAlgorithm & PermutationBasedEvolutionaryAlgorithm::runGA() {

    assert (candidateLength > 0);

    population.reserve(3 * maxCandidates);
    assert (population.empty());

    Population nextGeneration;
    nextGeneration.reserve(3 * maxCandidates);

    // One worker, and one generator for it, per thread.  A worker may keep a reference to the generator it was
    // made with, so each candidate's generator is that one, reseeded for the candidate.
    const unsigned workerCount = std::max(threadCount, 1U);
    std::vector<pipeline_random_engine> workerRng(workerCount);
    std::vector<WorkerPtr> workers;
    workers.reserve(workerCount);
    for (unsigned w = 0; w < workerCount; ++w) {
        workers.emplace_back(makeWorker(workerRng[w]));
    }
    ParallelFor parallel(workerCount);

    std::vector<Candidate> batch;
    std::vector<pipeline_random_engine::result_type> seeds;
    std::vector<typename Candidates::iterator> entries;
    std::vector<size_t> fresh;

    // Repairs and evaluates the candidates in the batch, appending them to the generation in batch order.
    auto evaluate = [&](Population & generation) {
        const auto n = batch.size();
        seeds.resize(n);
        for (size_t i = 0; i < n; ++i) {
            seeds[i] = rng();
        }
        parallel.run(n, [&](const unsigned w, const size_t i) {
            assert (batch[i].size() == candidateLength);
            workerRng[w].seed(seeds[i]);
            workers[w]->repair(batch[i], workerRng[w]);
        });
        entries.clear();
        fresh.clear();
        for (size_t i = 0; i < n; ++i) {
            const auto f = candidates.emplace(std::move(batch[i]), 0);
            entries.push_back(f.first);
            if (LLVM_LIKELY(f.second)) {
                fresh.push_back(i);
            }
        }
        parallel.run(fresh.size(), [&](const unsigned w, const size_t k) {
            const auto i = fresh[k];
            workerRng[w].seed(seeds[i]);
            entries[i]->second = workers[w]->fitness(entries[i]->first, workerRng[w]);
        });
        for (const auto & e : entries) {
            generation.emplace_back(e);
        }
        batch.clear();
    };

    mainWorker = makeWorker(rng);

    if (LLVM_UNLIKELY(initGA(nextGeneration))) {
        population.swap(nextGeneration);
        goto enumerated_entire_search_space;
    }

    BEGIN_SCOPED_REGION

    assert (MaxInitCandidates <= maxCandidates);

    if (nextGeneration.size() < MaxInitCandidates) {
        auto n = MaxInitCandidates - nextGeneration.size();
        while (--n) {
            Candidate C(candidateLength);
            mainWorker->newCandidate(C, rng);
            batch.push_back(std::move(C));
        }
    }
    evaluate(nextGeneration);

    assert (population.empty());
    assert (nextGeneration.size() > 0);
    assert (candidateLength > 0);

    population.swap(nextGeneration);

    if (LLVM_UNLIKELY(candidateLength < 2)) {
        goto enumerated_entire_search_space;
    }

    // The crossover/selection logic below assumes at least two candidates to
    // operate on (it asserts populationSize > 1), but nothing upstream actually
    // guarantees that many were produced -- the population-initialization phase
    // above only asserts nextGeneration.size() > 0 (initGA may supply fewer, and
    // duplicates collapse).  getResult() already handles a population of exactly
    // one correctly, so just skip the generational loop in that case too.
    if (LLVM_UNLIKELY(population.size() <= 1)) {
        goto enumerated_entire_search_space;
    }

    permutation_bitset bitString(candidateLength);

    BitVector uncopied(candidateLength);

    std::uniform_real_distribution<double> zeroToOneReal(0.0, 1.0);

    constexpr auto minFitVal = std::numeric_limits<FitnessValueType>::min();
    constexpr auto maxFitVal = std::numeric_limits<FitnessValueType>::max();
    constexpr auto worstFitnessValue = FitnessValueEvaluator::eval(minFitVal, maxFitVal) ? maxFitVal : minFitVal;

    unsigned averageStallCount = 0;
    unsigned bestStallCount = 0;

    double priorAverageFitness = worstFitnessValue;
    FitnessValueType priorBestFitness = worstFitnessValue;

    std::vector<double> weights(maxCandidates * 3);

    flat_set<unsigned> chosen;
    chosen.reserve(maxCandidates);

    for (unsigned generation = 0; generation < maxGenerations; ++generation) {

        nextGeneration.clear();

        // nextGeneration.assign(population.begin(), population.end());
        const auto populationSize = population.size();
        assert (populationSize > 1);

        // The mutation rate falls as the search stalls.
        const auto s = std::max(averageStallCount, bestStallCount);
        const Rational d{maxStallGenerations - s, maxStallGenerations};
        const double currentMutationRate = ((double)d.numerator() / (double)d.denominator()) + 0.03;
        const double currentCrossoverRate = 1.0 - currentMutationRate;

        // CROSSOVER:

        for (unsigned i = 1; i < populationSize; ++i) {
            for (unsigned j = 0; j < i; ++j) {
                if (zeroToOneReal(rng) <= currentCrossoverRate) {

                    const Candidate & A = population[i]->first;
                    const Candidate & B = population[j]->first;

                    // generate a random bit string
                    bitString.randomize(rng);

                    auto crossover = [&](const Candidate & A, const Candidate & B, const bool selector) {

                        Candidate C(candidateLength);

                        assert (candidateLength > 1);
                        assert (C.size() == candidateLength);

                        uncopied.reset();

                        #ifndef NDEBUG
                        unsigned count = 0;
                        #endif

                        for (unsigned k = 0; k < candidateLength; ++k) {
                            if (bitString.test(k) == selector) {
                                const auto v = A[k];
                                assert (v < candidateLength);
                                assert ("candidate contains duplicate values?" && !uncopied.test(v));
                                uncopied.set(v);
                                #ifndef NDEBUG
                                ++count;
                                #endif
                            } else {
                                C[k] = A[k];
                            }
                        }

                        for (unsigned k = 0U, p = -1U; k < candidateLength; ++k) {
                            const auto t = bitString.test(k);
                            if (t == selector) {
                                // V contains 1-bits for every entry we did not
                                // directly copy from A into C. We now insert them
                                // into C in the same order as they are in B.
                                #ifndef NDEBUG
                                assert (count-- > 0);
                                #endif
                                for (;;){
                                    ++p;
                                    assert (p < candidateLength);
                                    const auto v = B[p];
                                    assert (v < candidateLength);
                                    if (uncopied.test(v)) {
                                        break;
                                    }
                                }
                                C[k] = B[p];
                            }
                        }

                        assert (count == 0);

                        batch.push_back(std::move(C));

                    };

                    crossover(A, B, true);

                    crossover(B, A, false);

                }
            }
        }

        // MUTATION:

        for (unsigned i = 0; i < populationSize; ++i) {
            if (zeroToOneReal(rng) <= currentMutationRate) {

                auto & A = population[i];

                Candidate C{A->first};

                const auto a = std::uniform_int_distribution<unsigned>{0, candidateLength - 2}(rng);
                const auto b = std::uniform_int_distribution<unsigned>{a + 1, candidateLength - 1}(rng);
                std::shuffle(C.begin() + a, C.begin() + b, rng);

                batch.push_back(std::move(C));

            }
        }

        evaluate(nextGeneration);

        while (LLVM_UNLIKELY(nextGeneration.size() < 2)) {
            nextGeneration.push_back(population.back());
            population.pop_back();
        }
        population.clear();
        const auto newPopulationSize = nextGeneration.size();
        assert (newPopulationSize > 1);

        FitnessValueType sumOfGenerationalFitness = 0.0;
        auto minFitness = maxFitVal;
        auto maxFitness = minFitVal;

        for (const auto & I : nextGeneration) {
            const auto fitness = I->second;
            sumOfGenerationalFitness += fitness;
            if (minFitness > fitness) {
                minFitness = fitness;
            }
            if (maxFitness < fitness) {
                maxFitness = fitness;
            }
        }

        const double averageGenerationFitness = ((double)sumOfGenerationalFitness) / ((double)newPopulationSize);

        FitnessValueType bestGenerationalFitness = maxFitness;
        if (FitnessValueEvaluator::eval(minFitness, maxFitness)) {
            bestGenerationalFitness = minFitness;
        }

        bool hasStalled = false;

        if (abs_subtract(averageGenerationFitness, priorAverageFitness) <= static_cast<double>(averageStallThreshold)) {
            if (++averageStallCount == maxStallGenerations) {
                hasStalled = true;
            }
        } else {
            averageStallCount = 0;
        }
        assert (averageStallCount <= maxStallGenerations);

        if (abs_subtract(bestGenerationalFitness, priorBestFitness) <= maxStallThreshold) {
            if (++bestStallCount == maxStallGenerations) {
                hasStalled = true;
            }
        } else {
            bestStallCount = 0;
        }
        assert (bestStallCount <= maxStallGenerations);

        // BOLTZMANN SELECTION:
        if (newPopulationSize <= maxCandidates) {
            population.swap(nextGeneration);
        } else {

            if (LLVM_UNLIKELY(minFitness == maxFitness)) {
                std::shuffle(nextGeneration.begin(), nextGeneration.end(), rng);
                for (unsigned i = 0; i < maxCandidates; ++i) {
                    population.emplace_back(nextGeneration[i]);
                }
            } else {

                // Calculate the variance for the annealing factor

                double sumDiffOfSquares = 0.0;
                for (unsigned i = 0; i < newPopulationSize; ++i) {
                    const auto w = nextGeneration[i]->second;
                    const auto d = w - averageGenerationFitness;
                    sumDiffOfSquares += d * d;
                }

                double beta;
                if (LLVM_LIKELY(sumDiffOfSquares == 0)) {
                    beta = 4.0;
                } else {
                    beta = std::sqrt(newPopulationSize / sumDiffOfSquares);
                }

                if (weights.size() < newPopulationSize) {
                    weights.resize(newPopulationSize);
                }

                const auto weights_end = weights.begin() + newPopulationSize;

                assert (chosen.empty());

                auto sumX = 0.0;
                unsigned fittestIndividual = 0;
                const double r = beta / (double)(maxFitness - minFitness);
                for (unsigned i = 0; i < newPopulationSize; ++i) {
                    const auto itr = nextGeneration[i];
                    assert (itr->first.size() == candidateLength);
                    const auto w = itr->second;
                    assert (w >= bestGenerationalFitness);
                    if (w == bestGenerationalFitness) {
                        fittestIndividual = i;
                    }
                    const double x = std::exp((double)(w - minFitness) * r);
                    const auto y = std::max(x, std::numeric_limits<double>::min());
                    sumX += y;
                    weights[i] = sumX;
                }
                // ELITISM: always keep the fittest candidate for the next generation
                chosen.insert(fittestIndividual);
                std::uniform_real_distribution<double> selector(0, sumX);
                // A wide enough fitness spread makes some candidates' exp()-weighted
                // contributions to sumX numerically insignificant (more than double
                // precision's ~1e16x dynamic range smaller than the running total),
                // silently lost by sumX += y above -- their weights[] slot then differs
                // from its neighbour by nothing a random draw can land between, making
                // that index effectively unreachable here. Confirmed in practice as a
                // reproducible hang (csv2json_test under make check): unbounded retries
                // waiting for a random draw that may never come. Bound the attempts and
                // deterministically fill any remaining slots, so termination doesn't
                // depend on how skewed the fitness distribution happens to be.
                unsigned selectionAttempts = 0;
                const unsigned maxSelectionAttempts = 100 * maxCandidates;
                while (chosen.size() < maxCandidates && selectionAttempts < maxSelectionAttempts) {
                    ++selectionAttempts;
                    const auto d = selector(rng);
                    assert (d < sumX);
                    const auto f = std::upper_bound(weights.begin(), weights_end, d);
                    assert (f != weights_end);
                    const unsigned j = std::distance(weights.begin(), f);
                    assert (j < newPopulationSize);
                    chosen.insert(j);
                }
                for (unsigned i = 0; i < newPopulationSize && chosen.size() < maxCandidates; ++i) {
                    chosen.insert(i);
                }
                for (unsigned i : chosen) {
                    assert (i < newPopulationSize);
                    const auto itr = nextGeneration[i];
                    assert (itr->first.size() == candidateLength);
                    population.push_back(itr);
                }
                chosen.clear();
            }

        }

        assert (population.size() > 0);

        if (LLVM_UNLIKELY(hasStalled)) break;

        // errs() << "averageGenerationFitness=" << averageGenerationFitness << "\n";
        // errs() << "bestGenerationalFitness=" << bestGenerationalFitness << "\n";

        priorAverageFitness = averageGenerationFitness;
        priorBestFitness = bestGenerationalFitness;
    }

    END_SCOPED_REGION

enumerated_entire_search_space:

    std::sort(population.begin(), population.end(), FitnessComparator{});

    return *this;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief insertCandidate
 ** ------------------------------------------------------------------------------------------------------------- */
bool PermutationBasedEvolutionaryAlgorithm::insertCandidate(Candidate && candidate, Population & population) {
    assert (candidate.size() == candidateLength);
    #ifndef NDEBUG
    BitVector check(candidateLength);
    for (unsigned i = 0; i != candidateLength; ++i) {
        const auto v = candidate[i];
        assert ("invalid candidate #" && v < candidateLength);
        check.set(v);
    }
    assert ("duplicate candidate #" && (check.count() == candidateLength));
    #endif
    // NOTE: do not erase candidates or switch the std::map to something else without
    // verifying whether the population iterators are being invalidated.
    const auto f = candidates.emplace(std::move(candidate), 0);
    if (LLVM_LIKELY(f.second)) {
        f.first->second = mainWorker->fitness(f.first->first, rng);
    }
    assert (f.first != candidates.end());
    if (f.second) {
        population.emplace_back(f.first);
    }
    return f.second;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getResult
 ** ------------------------------------------------------------------------------------------------------------- */
OrderingDAWG PermutationBasedEvolutionaryAlgorithm::getResult() const {
    assert (std::is_sorted(population.begin(), population.end(), FitnessComparator{}));

    // Construct a trie of all possible best (lowest) orderings of this partition

    auto i = population.begin();
    const auto end = population.end();
    OrderingDAWG result(1);
    const auto bestWeight = (*i)->second;
    do {
        make_trie((*i)->first, result);
    } while ((++i != end) && (bestWeight == (*i)->second));

    return result;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief make_trie
 ** ------------------------------------------------------------------------------------------------------------- */
void PermutationBasedEvolutionaryAlgorithm::make_trie(const Candidate & C, OrderingDAWG & O) const {
    assert (num_vertices(O) > 0);
    assert (C.size() == candidateLength);
    auto u = 0;

    for (unsigned i = 0; i != candidateLength; ) {
        const auto j = C[i];
        assert (j < candidateLength);
        for (const auto e : make_iterator_range(out_edges(u, O))) {
            if (O[e] == j) {
                u = target(e, O);
                goto in_trie;
            }
        }
        BEGIN_SCOPED_REGION
        const auto v = add_vertex(O);
        add_edge(u, v, j, O);
        u = v;
        END_SCOPED_REGION
in_trie:    ++i;
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief newCandidate
 ** ------------------------------------------------------------------------------------------------------------- */
void PermutationBasedEvolutionaryAlgorithmWorker::newCandidate(Candidate & C, pipeline_random_engine & rng) {
    std::iota(C.begin(), C.end(), 0);
    std::shuffle(C.begin(), C.end(), rng);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief runHarmonySearch
 ** ------------------------------------------------------------------------------------------------------------- */
const BitStringBasedHarmonySearch & BitStringBasedHarmonySearch::runHarmonySearch() {

    assert (candidateLength > 1);

    population.reserve(maxCandidates);

    if (LLVM_UNLIKELY(initialize(population))) {
        goto enumerated_entire_search_space;
    }

    BEGIN_SCOPED_REGION

    std::uniform_real_distribution<double> zeroToOneReal(0.0, 1.0);

    std::uniform_int_distribution<unsigned> zeroOrOneInt(0, 1);

    FitnessValueType sumOfGenerationalFitness = 0.0;

    for (const auto & I : population) {
        const auto fitness = I->second;
        sumOfGenerationalFitness += fitness;
    }


    constexpr auto minFitVal = std::numeric_limits<FitnessValueType>::min();
    constexpr auto maxFitVal = std::numeric_limits<FitnessValueType>::max();
    constexpr auto worstFitnessValue = FitnessValueEvaluator::eval(minFitVal, maxFitVal) ? maxFitVal : minFitVal;

    unsigned averageStallCount = 0;

    double priorAverageFitness = worstFitnessValue;


    Population nextGeneration;
    nextGeneration.reserve(maxCandidates);

    Candidate newCandidate(candidateLength);

    for (unsigned round = 0; round < maxRounds; ++round) {

        const auto populationSize = population.size();

        assert (populationSize <= maxCandidates);

        auto considerationRate = CosAmplitude * std::cos(CosAngularFrequency * (double)round) + CosShift;

        std::uniform_int_distribution<unsigned> upToN(0, populationSize - 1);

        for (unsigned j = 0; j < candidateLength; ++j) {
            if (zeroToOneReal(rng) < considerationRate) {
                const auto k = upToN(rng);
                const bool v = population[k]->first.test(j);
                newCandidate.set(j, v);
            } else {
                newCandidate.set(j, zeroOrOneInt(rng));
            }
        }

        repairCandidate(newCandidate);

        const auto f = candidates.insert(std::make_pair(newCandidate, 0));
        if (LLVM_LIKELY(f.second)) {
            const auto val = fitness(f.first->first);
            if (val >= population.front()->second) {
                sumOfGenerationalFitness += val;
                f.first->second = val;
                bestResult = std::max(bestResult, val);
                std::pop_heap(population.begin(), population.end(), FitnessComparator{});
                const auto & worst = population.back();
                sumOfGenerationalFitness -= worst->second;
                population.pop_back();
                population.emplace_back(f.first);
                std::push_heap(population.begin(), population.end(), FitnessComparator{});
            }
        }

        const auto n = population.size();

        const double averageGenerationFitness = ((double)sumOfGenerationalFitness) / ((double)n);

        if (abs_subtract(averageGenerationFitness, priorAverageFitness) <= static_cast<double>(averageStallThreshold)) {
            if (++averageStallCount == maxStallGenerations) {
                break;
            }
        } else {
            averageStallCount = 0;
        }
        assert (averageStallCount < maxStallGenerations);

    }

    END_SCOPED_REGION

enumerated_entire_search_space:

    std::sort_heap(population.begin(), population.end(), FitnessComparator{});

    return *this;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief removeLeastFitCandidates
 ** ------------------------------------------------------------------------------------------------------------- */
void BitStringBasedHarmonySearch::updatePopulation(Population & nextGeneration) {
    for (const auto & I : nextGeneration) {
        assert (population.size() <= maxCandidates);
        if (population.size() == maxCandidates) {
            if (I->second >= population.front()->second) {
                std::pop_heap(population.begin(), population.end(), FitnessComparator{});
                population.pop_back();
            } else {
                // New item exceeds the weight of the heaviest candiate
                // in the population.
                continue;
            }
        }
        population.emplace_back(I);
        std::push_heap(population.begin(), population.end(), FitnessComparator{});
    }

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief insertCandidate
 ** ------------------------------------------------------------------------------------------------------------- */
bool BitStringBasedHarmonySearch::insertCandidate(const Candidate & candidate, Population & population) {
    const auto f = candidates.insert(std::make_pair(candidate, 0));
    if (LLVM_LIKELY(f.second)) {
        const auto val = fitness(f.first->first);
        f.first->second = val;
        bestResult = std::max(bestResult, val);
        population.emplace_back(f.first);
        std::push_heap(population.begin(), population.end(), FitnessComparator{});
        return true;
    }
    return false;
}

}
