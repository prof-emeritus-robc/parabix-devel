#ifndef PASS_MEASUREMENTS_H
#define PASS_MEASUREMENTS_H

#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/PassInstrumentation.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/IR/Module.h>
#include <llvm/PassRegistry.h>
#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/IR/PassManager.h>
#include <boost/container/flat_map.hpp>


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
        const auto newCount = other.Count + Count;
        const auto delta = other.Mean - Mean;
        Mean += delta * ((double)(other.Count) / newCount);
        M2 += other.M2 * ((delta * delta * (double)Count * (double)other.Count) / (double)newCount);
        Count = newCount;
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
      return Index == Other.Index && Name.compare(Other.Name) == 0;
    }
};


struct MeasurementMapKeyInfo {
  using KeyType = MeasurementMapKey;

  static inline MeasurementMapKey getEmptyKey() {
    return MeasurementMapKey{llvm::StringRef{}, ~0ULL};
  }

  static inline MeasurementMapKey getTombstoneKey() {
    return MeasurementMapKey{llvm::StringRef(), ~1ULL};
  }

  static unsigned getHashValue(const MeasurementMapKey &PairVal) {
    return llvm::detail::combineHashValue(
        llvm::DenseMapInfo<llvm::StringRef>::getHashValue(PairVal.Name),
        llvm::DenseMapInfo<uint64_t>::getHashValue(PairVal.Index));
  }

  static bool isEqual(const MeasurementMapKey &LHS, const MeasurementMapKey &RHS) {
    return LHS == RHS;
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


    PassMeasurements(llvm::TargetMachine * TM)
    : TrackingData(1, {0, 0, MAX_INT})
    , MeasurementMap{} {


        PIC.registerBeforeNonSkippedPassCallback([this](llvm::StringRef PassId, llvm::Any IR) {
            return PassMeasurements::startTimerCallback(PassId, IR);
        });

        PIC.registerBeforeAnalysisCallback([this](llvm::StringRef PassId, llvm::Any IR) {
            return PassMeasurements::startTimerCallback(PassId, IR);
        });

        PIC.registerAfterPassCallback([this](llvm::StringRef PassId, llvm::Any IR, const llvm::PreservedAnalyses & PA){
            return endTimerCallback(PassId, IR, !PA.areAllPreserved());
        });

        PIC.registerAfterAnalysisCallback([this](llvm::StringRef PassId, llvm::Any IR) {
            return endTimerCallback(PassId, IR, false);
        });

//        registerCodeGenCallback(PIC, static_cast<LLVMTargetMachine &>(*TM));
    }


    static llvm::StringRef resolvePassName(llvm::StringRef PassId, llvm::Any IR) {
        if (PassId.empty() || PassId == "Machine Optimization Pass") {
            auto pass = llvm::any_cast<llvm::Pass *>(IR);
            if (pass) {
                auto registry = llvm::PassRegistry::getPassRegistry();
                auto info = registry->getPassInfo(pass);
                if (info) {
                    return info->getPassArgument();
                }
            }
        }
        return PassId;
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

    uint64_t findOrAddMeasurement(llvm::StringRef PassId, llvm::Any IR) {
        auto nm =  resolvePassName(PassId, IR);
        assert (TrackingData.size() > 0);
        auto & td = TrackingData.back();
        auto parent = td.MeasurementIndex;
        auto f = MeasurementMap.find(MeasurementMapKey(nm, parent));
        if (f == MeasurementMap.end()) {
            const auto idx = Measurements.size();
            Measurements.emplace_back(nm, idx, parent);
            MeasurementMap.insert(std::make_pair(MeasurementMapKey(nm, parent), idx));
            return idx;
        } else {
            return f->second;
        }
    };

    static const void * getPtr(const llvm::Any & IR) {
        if (const llvm::Function * const * F = llvm::any_cast<llvm::Function *>(&IR)) {
            return (*F);
        }
        if (const llvm::MachineFunction * const * F = llvm::any_cast<llvm::MachineFunction *>(&IR)) {
            return (*F);
        }
        if (const llvm::Module * const * M = llvm::any_cast<llvm::Module *>(&IR)) {
            return (*M);
        }
        return nullptr;
    }

    void update(PassMeasurements & other);


private:

    std::vector<PassTrackingData> TrackingData;
    llvm::DenseMap<MeasurementMapKey, uint64_t, MeasurementMapKeyInfo> MeasurementMap;

};



#endif // PASS_MEASUREMENTS_H
