#ifndef PASS_MEASUREMENTS_CPP
#define PASS_MEASUREMENTS_CPP

#include <idisa/passes/pass_measurements.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_os_ostream.h>
#include <boost/container/flat_map.hpp>
#include <queue>
#include <thread>

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
    assert (TrackingData.size() > 0);
    const auto & td = TrackingData.back();
    assert ((TrackingData.size() > 1) ^ (td.MeasurementIndex == MAX_INT));
    const auto idx = findOrAddMeasurement(PassId, td.MeasurementIndex);
    const auto loc = getLinesOfCode(IR);
    const auto cc = getCycleCount();
    TrackingData.emplace_back(cc, loc, idx);
}

void PassMeasurements::endTimerCallback(llvm::StringRef PassId, llvm::Any IR, const bool transformative) {
    const auto cc = getCycleCount();
    assert (TrackingData.size() > 1);
    const auto & td = TrackingData.back();
    uint64_t total = 0;
    if (LLVM_LIKELY(cc > td.StartTime)) {
        total = cc - td.StartTime;
    }
    assert (td.MeasurementIndex < Measurements.size());
    PassMeasurement & CM = Measurements[td.MeasurementIndex];
    assert (PassId.size() == CM.Name.size() && PassId.compare(CM.Name) == 0);
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
    TrackingData.pop_back();
    TrackingData.back().InnerCycleCount += total;
}

uint64_t PassMeasurements::findOrAddMeasurement(llvm::StringRef PassId, uint64_t parent) {
    auto f = MeasurementMap.find(MeasurementMapKey(PassId, parent));
    if (f == MeasurementMap.end()) {
        const auto k = Measurements.size();
        Measurements.emplace_back(PassId, parent);
        MeasurementMap.insert(std::make_pair(MeasurementMapKey(PassId, parent), k));
        return k;
    } else {
        return f->second;
    }
};

void PassMeasurements::update(PassMeasurements & other) {

    assert (TrackingData.size() == 1);
    assert (other.TrackingData.size() == 1);

    auto & A = Measurements;
    auto & B = other.Measurements;
    if (A.empty()) {
        A.swap(B);
        MeasurementMap.swap(other.MeasurementMap);
    } else if (B.empty()) {
        return;
    }

    const auto m = B.size();

    std::vector<uint64_t> M(m, MAX_INT);

    auto addOrUpdateMeasurement = [&](const PassMeasurement & measurement, const uint64_t otherIndex, const uint64_t currentParent) {
        const auto currentIndex = findOrAddMeasurement(measurement.Name, currentParent);
        assert (currentIndex < A.size());
        A[currentIndex].update(measurement);
        assert (M[otherIndex] == MAX_INT);
        M[otherIndex] = currentIndex;
    };

    std::queue<size_t> Q;

    for (size_t i = 0; i < m; ++i) {
        const auto & measurement = B[i];
        #ifndef NDEBUG
        auto f = other.MeasurementMap.find(MeasurementMapKey{measurement.Name, measurement.Parent});
        assert (f != other.MeasurementMap.end());
        assert (f->second == i);
        #endif
        if (measurement.Parent == MAX_INT) {
            addOrUpdateMeasurement(measurement, i, MAX_INT);
        } else {
            Q.push(i);
        }
    }

    while (!Q.empty()) {
        const auto k = Q.front();
        assert (k < m);
        Q.pop();
        const auto & measurement = B[k];
        const auto parent = M[measurement.Parent];
        if (parent == MAX_INT) {
            assert (Q.size() > 0);
            Q.push(k);
        } else {
            addOrUpdateMeasurement(measurement, k, parent);
        }
    }



}

void PassMeasurements::print(llvm::raw_ostream & out) const {

    double sumMean = 0;
    size_t maxNameLength = 0;
    for (const auto & M : Measurements) {
        sumMean += M.Mean;
        maxNameLength = std::max(maxNameLength, M.Name.size());
    }
    maxNameLength += 8;

    struct Node {
        const PassMeasurement * Metric = nullptr;
        llvm::SmallVector<Node *, 1> Children;
    };

    const auto m = Measurements.size();

    std::function<void(size_t, unsigned)> postOrderPrint = [&](const size_t measurementIndex, const unsigned indent) {

        auto & M = Measurements[measurementIndex];
        const auto normMean = (M.Mean * 100.0 / sumMean);
        const auto normStdDev = (M.getStdDev() * 100.0 / M.Mean);
        const auto modPerc = (double)M.ModifiedCount / (double)M.Count;

        const auto & nm = M.Name;

        out.indent(indent);
        out << nm;
        out.indent(maxNameLength - indent - nm.size());

        out << ", " << llvm::formatv("{0,6}", M.Count);
        out << ", " << llvm::formatv("{0,6:f2}", normMean);
        out << ", " << llvm::formatv("{0,6:f2}", normStdDev);
        out << ", " << llvm::formatv("{0,6:f2}", modPerc);
        out << '\n';

        // TODO: this is very inefficient but m is not expected to be large
        for (size_t i = 0; i < m; ++i) {
            auto & measure = Measurements[i];
            if (measure.Parent == measurementIndex) {
                postOrderPrint(i, indent + 2);
            }
        }
    };

    out << "PASS NAME,MEAN,STDEV,MODIFIED\n\n";

    for (size_t i = 0; i < m; ++i) {
        auto & measure = Measurements[i];
        if (measure.Parent == PassMeasurements::MAX_INT) {
            postOrderPrint(i, 0);
        }

    }


}

llvm::StringRef PassMeasurements::intern(const llvm::StringRef string) {
    if (LLVM_UNLIKELY(string.empty())) {
        return llvm::StringRef{};
    }
    auto f = InternedStrings.find(string);
    if (f != InternedStrings.end()) {
        return *f;
    }
    char * const buf = Allocator.Allocate<char>(string.size() + 1);
    std::memcpy(buf, string.data(), string.size());
    buf[string.size()] = '\0';
    llvm::StringRef Interned(buf, string.size());
    InternedStrings.insert(Interned);
    return Interned;
}

#endif // PASS_MEASUREMENTS_CPP
