#ifndef PASS_MEASUREMENTS_H
#define PASS_MEASUREMENTS_H

#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/PassInstrumentation.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/IR/Module.h>
#include <llvm/PassRegistry.h>
#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/IR/PassManager.h>
#include <unordered_map>
#include <llvm/Support/Allocator.h>
#include <unordered_set>
#include <llvm/Support/raw_ostream.h>

struct PassMeasurement {
    llvm::StringRef Name;
    uint64_t Index;
    uint64_t Parent;
    uint64_t Count;
    uint64_t ModifiedCount;
    double Mean;
    double M2;

    PassMeasurement(llvm::StringRef name, uint64_t idx, uint64_t parent)
    : Name(name)
    , Index(idx)
    , Parent(parent)
    , Count(0)
    , ModifiedCount(0)
    , Mean(0.0)
    , M2(0.0) {

    }

    PassMeasurement(const PassMeasurement & other, uint64_t idx, uint64_t parent)
    : Name(other.Name)
    , Index(idx)
    , Parent(parent)
    , Count(other.Count)
    , ModifiedCount(other.ModifiedCount)
    , Mean(other.Mean)
    , M2(other.M2) {

    }


    void update(const double x) {
        const auto newCount = Count + 1;
        const auto delta = x - Mean;
        Mean += delta / ((double)newCount);
        M2 += delta * delta;
        Count = newCount;
    }

    void update(const PassMeasurement & other) {
        assert (other.Count > 0 && Count > 0);
        assert (other.Name == Name);
        const auto newCount = Count + other.Count;
        const double dblCount = (double)newCount;
        const auto newMean = (Mean * Count + other.Mean * other.Count) / dblCount;
        const auto delta = other.Mean - Mean;
        const auto newM2 = (M2 + other.M2) + (delta * Count * delta * other.Count) / dblCount;
        Count = newCount;
        Mean = newMean;
        M2 = newM2;
        ModifiedCount += other.ModifiedCount;
    }

    double getStdDev() const {
        if (LLVM_UNLIKELY(Count == 1)) {
            return 0.0;
        }
        return std::sqrt(M2 / Count);
    }

};

struct MeasurementMapKey {
    llvm::StringRef Name;
    uint64_t Index;

    explicit MeasurementMapKey(llvm::StringRef nm, uint64_t idx)
    : Name(nm.data(), nm.size())
    , Index(idx) {

    }

    bool operator==(const MeasurementMapKey &Other) const {
        return (Other.Index == Index) && Other.Name.equals(Name);
    }

};

struct PassMeasurements {

    struct PassTrackingData {
        uint64_t StartTime = 0;
        uint64_t LinesOfCode = 0;
        uint64_t InnerCycleCount = 0;
        uint64_t MeasurementIndex = 0;

        PassTrackingData(uint64_t st, uint64_t loc, uint64_t idx)
        : StartTime(st)
        , LinesOfCode (loc)
        , MeasurementIndex(idx) {

        }


    };

    constexpr static auto MAX_INT = std::numeric_limits<uint64_t>::max();

    std::vector<PassMeasurement> Measurements;

    llvm::PassInstrumentationCallbacks PIC;

    void startTimerCallback(llvm::StringRef PassId, llvm::Any IR);

    void endTimerCallback(llvm::StringRef PassId, llvm::Any IR, const bool transformative);

    void endPassTimerCallback(llvm::StringRef PassId, llvm::Any IR, const llvm::PreservedAnalyses & PA) {
        return endTimerCallback(PassId, IR, !PA.areAllPreserved());
    }

    void endAnalysisTimerCallback(llvm::StringRef PassId, llvm::Any IR) {
        return endTimerCallback(PassId, IR, false);
    }

    PassMeasurements(llvm::TargetMachine * TM)
    : TrackingData(1, {0, 0, MAX_INT})
    , MeasurementMap{} {

        PIC.registerBeforeNonSkippedPassCallback([this](llvm::StringRef PassId, llvm::Any IR) {
            return startTimerCallback(intern(PassId), IR);
        });

        PIC.registerBeforeAnalysisCallback([this](llvm::StringRef PassId, llvm::Any IR) {
            return startTimerCallback(intern(PassId), IR);
        });

        PIC.registerAfterPassCallback([this](llvm::StringRef PassId, llvm::Any IR, const llvm::PreservedAnalyses & PA){
            return endTimerCallback(intern(PassId), IR, !PA.areAllPreserved());
        });

        PIC.registerAfterAnalysisCallback([this](llvm::StringRef PassId, llvm::Any IR) {
            return endTimerCallback(intern(PassId), IR, false);
        });

//        registerCodeGenCallback(PIC, static_cast<LLVMTargetMachine &>(*TM));
    }

    static size_t getLinesOfCode(const llvm::Any & IR) {
        if (const llvm::Function * const * F = llvm::any_cast<llvm::Function *>(&IR)) {
            return (*F)->getInstructionCount();
        }
        if (const llvm::MachineFunction * const * F = llvm::any_cast<llvm::MachineFunction *>(&IR)) {
            return (*F)->getInstructionCount();
        }
        if (const llvm::Module * const * M = llvm::any_cast<llvm::Module *>(&IR)) {
            size_t count = 0;
            for (const llvm::Function & F : (*M)->functions()) {
                count += F.getInstructionCount();
            }
            return count;
        }
        return 0;
    }

    uint64_t findOrAddMeasurement(llvm::StringRef PassId, uint64_t parent) {
        auto f = MeasurementMap.find(MeasurementMapKey(PassId, parent));
        if (f == MeasurementMap.end()) {
            const auto idx = Measurements.size();
            Measurements.emplace_back(PassId, idx, parent);
            MeasurementMap.insert(std::make_pair(MeasurementMapKey(PassId, parent), idx));
            return idx;
        } else {
            return f->second;
        }
    };

    void update(PassMeasurements & other);

    void print(llvm::raw_ostream & out) const;

    llvm::StringRef intern(const llvm::StringRef string);

private:

    struct MeasurementMapKeyHash {
        std::size_t operator()(const MeasurementMapKey & k) const {
            const auto a = llvm::DenseMapInfo<uint64_t>::getHashValue(k.Index);
            const auto b = llvm::DenseMapInfo<llvm::StringRef>::getHashValue(k.Name);
            return llvm::detail::combineHashValue(a, b);
        }
    };

    struct MeasurementMapKeyEquals {
        bool operator()(const MeasurementMapKey & A, const MeasurementMapKey & B) const {
            return (A.Index == B.Index) && A.Name.equals(B.Name);
        }
    };

    struct StringRefHash {
        std::size_t operator()(const llvm::StringRef & k) const {
            return llvm::DenseMapInfo<llvm::StringRef>::getHashValue(k);
        }
    };

    struct StringRefEqual {
        bool operator()(const llvm::StringRef & A, const llvm::StringRef & B) const noexcept {
            return A.equals(B);
        }
    };

    std::vector<PassTrackingData> TrackingData;
    std::unordered_map<MeasurementMapKey, uint64_t, MeasurementMapKeyHash, MeasurementMapKeyEquals> MeasurementMap;
    std::unordered_set<llvm::StringRef, StringRefHash, StringRefEqual> InternedStrings;
    llvm::BumpPtrAllocator Allocator;

};



#endif // PASS_MEASUREMENTS_H
