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
    uint64_t Parent;
    uint64_t Count;
    uint64_t ModifiedCount;
    double Mean;
    double M2;

    PassMeasurement(llvm::StringRef name, uint64_t parent,
                    uint64_t count = 0, uint64_t modifiedCount = 0, double mean = 0.0, double m2 = 0.0)
    : Name(name.data(), name.size())
    , Parent(parent)
    , Count(count)
    , ModifiedCount(modifiedCount)
    , Mean(mean)
    , M2(m2) {

    }


    void update(const double x) {
        const auto newCount = Count + 1;
        const auto delta = x - Mean;
        Mean += delta / ((double)newCount);
        M2 += delta * delta;
        Count = newCount;
    }

    void update(const PassMeasurement & other) {
        assert (other.Name.size() == Name.size() && other.Name.compare(Name) == 0);
        const auto newCount = Count + other.Count;
        assert (newCount > 0);
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



struct PassMeasurements {

    struct StringRefHash {
        std::size_t operator()(const llvm::StringRef & k) const {
            std::string_view view(k.data(), k.size());
            return std::hash<std::string_view>{}(view);
        }
    };

    struct StringRefEqual {
        bool operator()(const llvm::StringRef & A, const llvm::StringRef & B) const noexcept {
            return A.size() == B.size() && A.compare(B) == 0;
        }
    };

    struct MeasurementMapKey {
        llvm::StringRef Name;
        uint64_t Parent;

        explicit MeasurementMapKey(llvm::StringRef nm, uint64_t parent)
        : Name(nm.data(), nm.size())
        , Parent(parent) {

        }

        bool operator==(const MeasurementMapKey & Other) const = delete;
    };

    struct MeasurementMapKeyHash {
        std::size_t operator()(const MeasurementMapKey & k) const {
            uint64_t A = StringRefHash{}(k.Name);
            const uint64_t B = std::hash<uint64_t>{}(k.Parent);
            A ^= B + 0x9e3779b97f4a7c15ULL + (A << 6) + (A >> 2);
            return A;
        }
    };

    struct MeasurementMapKeyEquals {
        bool operator()(const MeasurementMapKey & A, const MeasurementMapKey & B) const {
            return (A.Parent == B.Parent) && StringRefEqual{}(A.Name, B.Name);
        }
    };

    using MeasurementMapType = std::unordered_map<MeasurementMapKey, uint64_t, MeasurementMapKeyHash, MeasurementMapKeyEquals>;

    struct PassTrackingData {
        uint64_t StartTime = 0;
        uint64_t LinesOfCode = 0;
        uint64_t InnerCycleCount = 0;
        uint64_t MeasurementIndex = 0;

        PassTrackingData(uint64_t st, uint64_t loc, uint64_t idx)
        : StartTime(st)
        , LinesOfCode (loc)
        , InnerCycleCount(0)
        , MeasurementIndex(idx) {

        }


    };

    constexpr static auto MAX_INT = std::numeric_limits<uint64_t>::max();

    std::vector<PassMeasurement> Measurements;

    llvm::PassInstrumentationCallbacks PIC;

    void startTimerCallback(llvm::StringRef PassId, llvm::Any IR);

    void endTimerCallback(llvm::StringRef PassId, llvm::Any IR, const bool transformative);

    PassMeasurements(llvm::TargetMachine * /* TM */) {

        TrackingData.emplace_back(0, 0, MAX_INT);
        assert (Measurements.empty());
        assert (MeasurementMap.empty());

        PIC.registerBeforeNonSkippedPassCallback([this](llvm::StringRef PassId, llvm::Any IR) {
            return startTimerCallback(intern(PassId), IR);
        });

        PIC.registerBeforeAnalysisCallback([this](llvm::StringRef PassId, llvm::Any IR) {
            return startTimerCallback(intern(PassId), IR);
        });

        PIC.registerAfterPassCallback([this](llvm::StringRef PassId, llvm::Any IR, const llvm::PreservedAnalyses & PA){
            return endTimerCallback(PassId, IR, !PA.areAllPreserved());
        });

        PIC.registerAfterAnalysisCallback([this](llvm::StringRef PassId, llvm::Any IR) {
            return endTimerCallback(PassId, IR, false);
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

    uint64_t findOrAddMeasurement(llvm::StringRef PassId, uint64_t parent);

    void update(PassMeasurements & other);

    void print(llvm::raw_ostream & out) const;

    llvm::StringRef intern(const llvm::StringRef string);

    const MeasurementMapType & getMeasurementMap() const {
        return MeasurementMap;
    }

private:


    std::vector<PassTrackingData> TrackingData;
    MeasurementMapType MeasurementMap;
    std::unordered_set<llvm::StringRef, StringRefHash, StringRefEqual> InternedStrings;
    llvm::BumpPtrAllocator Allocator;

};



#endif // PASS_MEASUREMENTS_H
