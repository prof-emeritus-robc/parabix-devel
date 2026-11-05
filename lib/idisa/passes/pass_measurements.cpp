#ifndef PASS_MEASUREMENTS_CPP
#define PASS_MEASUREMENTS_CPP

#include <idisa/passes/pass_measurements.h>

#if (defined(__has_builtin) && __has_builtin(__builtin_readcyclecounter)) || defined(__GNUC__)
#elif defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <x86intrin.h>
#else
#include <chrono>
#endif

inline uint64_t getCycleCount() {
    #if (defined(__has_builtin) && __has_builtin(__builtin_readcyclecounter)) || defined(__GNUC__)
    return __builtin_readcyclecounter();
    #elif defined(_MSC_VER) || defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    return __rdtsc();
    #else
    return static_cast<uint64_t>(std::chrono::steady_clock::now().count());
    #endif
}


void PassMeasurements::startTimerCallback(llvm::StringRef PassId, llvm::Any IR) {
    const auto idx = findOrAddMeasurement(PassId, IR);
    const auto loc = getLinesOfCode(IR);
    const auto cc = getCycleCount();
    TrackingData.emplace_back(cc, loc, idx);
}

void PassMeasurements::endTimerCallback(llvm::StringRef PassId, llvm::Any IR, const bool transformative) {
    const auto cc = getCycleCount();
    assert (!TrackingData.empty());

    auto & td = TrackingData.back();
    uint64_t total = 0;
    if (LLVM_LIKELY(cc > td.StartTime)) {
        total = cc - td.StartTime;
    }
    PassMeasurement & CM = Measurements[td.MeasurementIndex];
    assert (resolvePassName(PassId, IR) == CM.Name);
    uint64_t mean = 0;
    if (LLVM_LIKELY(total > td.InnerCycleCount)) {
        mean = total - td.InnerCycleCount;
    }
    CM.update(mean);
    if (transformative) {
        const auto loc = getLinesOfCode(IR);
        if (loc != td.LinesOfCode) {
            CM.ModifiedCount++;
        }
    }
    assert (TrackingData.size() > 1);
    TrackingData.pop_back();
    TrackingData.back().InnerCycleCount += total;
}


void PassMeasurements::update(PassMeasurements & other) {
    auto & A = Measurements;
    auto & B = other.Measurements;
    if (A.empty()) {
        A.swap(B);
    } else if (B.empty()) {
        return;
    }

    const auto m = B.size();

    boost::container::flat_map<uint64_t, uint64_t> M;
    M.reserve(m);
    M.emplace(MAX_INT, MAX_INT);

    boost::container::flat_map<uint64_t, uint64_t> N;
    N.reserve(m);

    for (;;) {

        for (size_t i = 0; i < m; ++i) {
            auto & Bi = B[i];
            const auto f = M.find(Bi.Parent);
            if (f != M.end()) {
                const auto parent = f->second;
                auto key = MeasurementMapKey(Bi.Name, parent);
                auto g = MeasurementMap.find(key);
                if (g != MeasurementMap.end()) {
                    const uint64_t passIdx = g->second;
                    PassMeasurement & PM = Measurements[passIdx];
                    PM.update(Bi);
                    N.emplace(Bi.Index, passIdx);
                } else {
                    const uint64_t idx = Measurements.size();
                    Measurements.emplace_back(Bi, idx, parent);
                    MeasurementMap.insert(std::make_pair(key, idx));
                    N.emplace(Bi.Index, idx);
                }
            }
            M.clear();
            if (N.empty()) {
                return;
            }
            M.swap(N);
        }

    }

}

#endif // PASS_MEASUREMENTS_CPP
