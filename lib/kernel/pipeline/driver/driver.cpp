#include <kernel/pipeline/driver/driver.h>

#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>
#include <llvm/IR/Module.h>
#include <toolchain/toolchain.h>
#include <objcache/object_cache.h>
#include <llvm/Support/raw_ostream.h>

#include <llvm/IR/Verifier.h>
#include <boost/regex.hpp>

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/BlockFrequencyInfo.h>
#include <llvm/Analysis/BranchProbabilityInfo.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/MemoryDependenceAnalysis.h>
#include <llvm/Analysis/MemorySSA.h>
#include <llvm/Analysis/OptimizationRemarkEmitter.h>
#include <llvm/Analysis/PhiValues.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/Analysis/ProfileSummaryInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>

#include <llvm/IR/Dominators.h>
#include <llvm/IR/PassManager.h>

#include <llvm/Passes/PassBuilder.h>
#include <llvm/Target/TargetMachine.h>             // for TargetMachine, Tar...
#include <llvm/Target/TargetOptions.h>             // for TargetOptions
#include <llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/GVN.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#include <llvm/Transforms/Scalar/SROA.h>
#pragma GCC diagnostic pop
#include <llvm/Transforms/Scalar/MemCpyOptimizer.h>
#include <llvm/Transforms/Scalar/NewGVN.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Scalar/Reassociate.h>
#include <llvm/Transforms/Utils.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>

#include <llvm/IRPrinter/IRPrintingPasses.h>
#if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(19, 0, 0)
#include <llvm/IR/PassInstrumentation.h>
#endif

#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/FileSystem.h>

using namespace kernel;
using namespace llvm;

using RelationshipAllocator = Relationship::Allocator;

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief CreateStreamSet
 ** ------------------------------------------------------------------------------------------------------------- */
StreamSet * BaseDriver::CreateStreamSet(const unsigned NumElements, const unsigned FieldWidth) noexcept {
    RelationshipAllocator A(mAllocator);
    return new (A) StreamSet(mBuilder->getContext(), NumElements, FieldWidth);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief CreateRepeatingStreamSet
 ** ------------------------------------------------------------------------------------------------------------- */
RepeatingStreamSet * BaseDriver::CreateRepeatingStreamSet(const unsigned FieldWidth, std::vector<std::vector<uint64_t>> && stringSet, const bool isDynamic) noexcept {
    RelationshipAllocator A(mAllocator);
    // TODO: the stringSet will probably cause a memleak
    return new (A) RepeatingStreamSet(mBuilder->getContext(), FieldWidth, std::move(stringSet), isDynamic, false);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief CreateUnalignedRepeatingStreamSet
 ** ------------------------------------------------------------------------------------------------------------- */
RepeatingStreamSet * BaseDriver::CreateUnalignedRepeatingStreamSet(const unsigned FieldWidth, std::vector<std::vector<uint64_t>> && stringSet, const bool isDynamic) noexcept {
    RelationshipAllocator A(mAllocator);
    // TODO: the stringSet will probably cause a memleak
    return new (A) RepeatingStreamSet(mBuilder->getContext(), FieldWidth, std::move(stringSet), isDynamic, true);
}


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief CreateTruncatedStreamSet
 ** ------------------------------------------------------------------------------------------------------------- */
TruncatedStreamSet * BaseDriver::CreateTruncatedStreamSet(const StreamSet * data) noexcept {
    RelationshipAllocator A(mAllocator);
    return new (A) TruncatedStreamSet(mBuilder->getContext(), data);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief CreateConstant
 ** ------------------------------------------------------------------------------------------------------------- */
Scalar * BaseDriver::CreateScalar(not_null<Type *> scalarType) noexcept {
    RelationshipAllocator A(mAllocator);
    return new (A) Scalar(scalarType);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief CreateConstant
 ** ------------------------------------------------------------------------------------------------------------- */
Scalar * BaseDriver::CreateConstant(not_null<Constant *> value) noexcept {
    RelationshipAllocator A(mAllocator);
    return new (A) ScalarConstant(value);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief CreateCommandLineScalar
 ** ------------------------------------------------------------------------------------------------------------- */
Scalar * BaseDriver::CreateCommandLineScalar(CommandLineScalarType type) noexcept {
    RelationshipAllocator A(mAllocator);
    Type * scalarTy = nullptr;
    switch (type) {

        #ifdef ENABLE_PAPI
        case CommandLineScalarType::PAPIEventList:
            scalarTy = PointerType::getUnqual(mBuilder->getContext()); break;
        #endif
        case CommandLineScalarType::ParabixIllustratorObject:
            scalarTy = mBuilder->getVoidPtrTy(); break;
        case CommandLineScalarType::DynamicMultithreadingAddSynchronizationThreshold:
        case CommandLineScalarType::DynamicMultithreadingRemoveSynchronizationThreshold:
            scalarTy = mBuilder->getFloatTy(); break;
        default:
            scalarTy = mBuilder->getSizeTy(); break;
    }


    return new (A) CommandLineScalar(type, scalarTy);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addKernel
 ** ------------------------------------------------------------------------------------------------------------- */
void BaseDriver::addKernel(not_null<Kernel *> kernel) {

    // Verify the I/O relationships were properly set / defaulted in.

    for (Binding & input : kernel->getInputScalarBindings()) {
        if (LLVM_UNLIKELY(input.getRelationship() == nullptr)) {
            report_fatal_error(StringRef(kernel->getName()) + "." + input.getName() + " must be set upon construction");
        }
    }

    if (LLVM_UNLIKELY(kernel->getKernelFlags() & Kernel::KernelFlags::RequiresIllustratorObject)) {
        // TODO: temporary design choice; need to rethink how we should handle implicit scalars
        auto illustratorObject = CreateCommandLineScalar(CommandLineScalarType::ParabixIllustratorObject);
        kernel->getInputScalarBindings().emplace_back(KERNEL_ILLUSTRATOR_CALLBACK_OBJECT, illustratorObject);
    }

    for (Binding & input : kernel->getInputStreamSetBindings()) {
        if (LLVM_UNLIKELY(input.getRelationship() == nullptr)) {
            report_fatal_error(StringRef(kernel->getName()) + "." + input.getName() + " must be set upon construction");
        }
    }
    for (Binding & output : kernel->getOutputStreamSetBindings()) {
        if (LLVM_UNLIKELY(output.getRelationship() == nullptr)) {
            report_fatal_error(StringRef(kernel->getName()) + "." + output.getName() + " must be set upon construction");
        }
    }
    for (Binding & output : kernel->getOutputScalarBindings()) {
        if (output.getRelationship() == nullptr) {
            output.setRelationship(CreateScalar(output.getType()));
        }
    }

    mUncachedKernel.emplace_back(kernel);

}

class RemoveRedundantAllocaAndGEPInstructions : public PassInfoMixin<RemoveRedundantAllocaAndGEPInstructions> {
public:
    /// Run the pass over the function.
    PreservedAnalyses run(Function &F, AnalysisManager<Function> &AM);
};

class PHICanonicalizerPass : public PassInfoMixin<PHICanonicalizerPass> {
public:
    /// Run the pass over the function.
    PreservedAnalyses run(Function &F, AnalysisManager<Function> &AM);
};


class TracePass : public PassInfoMixin<TracePass> {
public:
    TracePass(KernelBuilder & b) : b(b), TraceFilter(codegen::TraceOption) {}
    PreservedAnalyses run(Function &F, AnalysisManager<Function> &AM);
private:
    KernelBuilder & b;
    const boost::regex TraceFilter;
};


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief FilteredPrintFunctionPass
 ** ------------------------------------------------------------------------------------------------------------- */
class FilteredPrintFunctionPass : public PrintFunctionPass {
public:
    FilteredPrintFunctionPass(raw_ostream & OS,
                  const std::string & Banner = "")
    : PrintFunctionPass(OS, Banner)
    , RegexFilter(codegen::ShowIRFilter) {

    }

    PreservedAnalyses run(Function &F, AnalysisManager<Function> & A) {
        if (LLVM_UNLIKELY(boost::regex_search(F.getName().data(), RegexFilter))) {
            return PrintFunctionPass::run(F, A);
        } else {
            return PreservedAnalyses::all();
        }
    }

    static bool isRequired() { return true; }
private:
    const boost::regex RegexFilter;
};


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief runAllOptimizationPasses
 ** ------------------------------------------------------------------------------------------------------------- */
void BaseDriver::runAllOptimizationPasses(KernelBuilder & b, Kernel::SelectedOptimizationPasses & passes, TargetMachine * TM) {


    #ifndef NDEBUG
    #define ADD_VERIFY_IR_PASS true
    #else
    #define ADD_VERIFY_IR_PASS  LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::VerifyIR))
    #endif

    PassInstrumentationCallbacks PIC;

    CGSCCAnalysisManager CGAM;
    FunctionAnalysisManager FAM;
    ModuleAnalysisManager MAM;

    CGAM.registerPass([&] { return PassInstrumentationAnalysis(&PIC); });

    if (ADD_VERIFY_IR_PASS) {
        MAM.registerPass([&] { return VerifierAnalysis(); });
    }
    MAM.registerPass([&] { return ProfileSummaryAnalysis(); });
    MAM.registerPass([&] { return PassInstrumentationAnalysis(&PIC); });
    MAM.registerPass([&] { return InlineAdvisorAnalysis(); });
    MAM.registerPass([&] { return LazyCallGraphAnalysis(); });


    FAM.registerPass([&] { return PassInstrumentationAnalysis(&PIC); });
    FAM.registerPass([&] { return TargetIRAnalysis(); });

    FAM.registerPass([&] { return AssumptionAnalysis(); });
    FAM.registerPass([&] { return DominatorTreeAnalysis(); });
    FAM.registerPass([&] { return PostDominatorTreeAnalysis(); });
    FAM.registerPass([&] { return TargetLibraryAnalysis(); });
    FAM.registerPass([&] { return AAManager(); });

    FAM.registerPass([&] { return LoopAnalysis(); });
    FAM.registerPass([&] { return PhiValuesAnalysis(); });
    FAM.registerPass([&] { return MemoryDependenceAnalysis(); });
    FAM.registerPass([&] { return MemorySSAAnalysis(); });
    FAM.registerPass([&] { return OptimizationRemarkEmitterAnalysis(); });

    FAM.registerPass([&] { return BranchProbabilityAnalysis(); });
    FAM.registerPass([&] { return BlockFrequencyAnalysis(); });


    CGAM.registerPass([&] { return FunctionAnalysisManagerCGSCCProxy(); });
    CGAM.registerPass([&] { return ModuleAnalysisManagerCGSCCProxy(MAM); });

    MAM.registerPass([&] { return FunctionAnalysisManagerModuleProxy(FAM); });
    MAM.registerPass([&] { return CGSCCAnalysisManagerModuleProxy(CGAM); });

    FAM.registerPass([&] { return ModuleAnalysisManagerFunctionProxy(MAM); });
    FAM.registerPass([&] { return CGSCCAnalysisManagerFunctionProxy(CGAM); });

    ModulePassManager MPM;
    FunctionPassManager FPM;

    #define FLAG(x) (1ULL << (x))

    uint64_t requiredPasses = 0;

    std::unique_ptr<raw_fd_ostream> unoptimizedOut;

    if (LLVM_UNLIKELY(codegen::ShowUnoptimizedIROption != codegen::OmittedOption)) {
        const auto & options = codegen::ShowUnoptimizedIROption;
        if (options.empty()) {
            unoptimizedOut = std::make_unique<raw_fd_ostream>(STDERR_FILENO, false, true);
        } else {
            std::error_code unoptimizedErr;
            unoptimizedOut = std::make_unique<raw_fd_ostream>(options, unoptimizedErr, sys::fs::OpenFlags::OF_None);
        }
        if (codegen::ShowIRFilter.empty()) {
            FPM.addPass(PrintFunctionPass(*unoptimizedOut));
        } else {
            FPM.addPass(FilteredPrintFunctionPass(*unoptimizedOut));
        }
    }
    if (LLVM_UNLIKELY(!codegen::TraceOption.empty())) {
        FPM.addPass(TracePass(b));
    }
    if (ADD_VERIFY_IR_PASS) {
        MPM.addPass(VerifierPass());
    }
    MPM.addPass(ModuleInlinerPass());

    FPM.addPass(RemoveRedundantAllocaAndGEPInstructions());
    FPM.addPass(SimplifyCFGPass());
    FPM.addPass(SROAPass(SROAOptions::ModifyCFG));
#if LLVM_VERSION_INTEGER < LLVM_VERSION_CODE(20, 0, 0)
    FPM.addPass(llvm::InstCombinePass());
#else
    llvm::InstCombineOptions Opts;
    //Opts.VerifyFixpoint = false;
    FPM.addPass(llvm::InstCombinePass(Opts));
#endif
    FPM.addPass(DCEPass());
    FPM.addPass(ReassociatePass());
    FPM.addPass(GVNPass());

    using P = Kernel::OptimizationPass;

    for (const P pass : passes) {
        switch (pass) {
             case P::AggressiveInstCombinePass:
                FPM.addPass(AggressiveInstCombinePass());
                break;
            case P::DCEPass:
                FPM.addPass(DCEPass());
                break;
            case P::EarlyCSEPass:
                FPM.addPass(EarlyCSEPass());
                break;
            case P::MemCpyOptPass:
                FPM.addPass(MemCpyOptPass());
                break;
            case P::NewGVNPass:
                FPM.addPass(NewGVNPass());
                break;
            case P::SimplifyCFGPass:
                FPM.addPass(SimplifyCFGPass());
                break;
            case P::PHICanonicalizerPass:
                FPM.addPass(PHICanonicalizerPass());
                break;
        }
    }

    std::unique_ptr<raw_fd_ostream> optimizedOut;

    // ShowIRFilter

    if (LLVM_UNLIKELY(codegen::ShowIROption != codegen::OmittedOption)) {
        const auto & options = codegen::ShowIROption;
        if (options.empty()) {
            optimizedOut = std::make_unique<raw_fd_ostream>(STDERR_FILENO, false, true);
        } else {
            std::error_code optimizedErr;
            optimizedOut = std::make_unique<raw_fd_ostream>(options, optimizedErr, sys::fs::OpenFlags::OF_None);
        }
        if (codegen::ShowIRFilter.empty()) {
            FPM.addPass(PrintFunctionPass(*optimizedOut));
        } else {
            FPM.addPass(FilteredPrintFunctionPass(*optimizedOut));
        }
    }

    MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));

    if (ADD_VERIFY_IR_PASS) {
        MPM.addPass(VerifierPass());
    }

    #undef ADD_VERIFY_IR_PASS

    MPM.run(*b.getModule(), MAM);

}


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief constructor
 ** ------------------------------------------------------------------------------------------------------------- */
BaseDriver::BaseDriver(std::string && moduleName)
: mContext(new LLVMContext())
, mMainModule(new Module(moduleName, *mContext))
, mBuilder(nullptr)
, mObjectCache(nullptr) {
    if (LLVM_UNLIKELY(codegen::EnableObjectCache)) {
        mObjectCache.reset(new ParabixObjectCache());
    }
}

BaseDriver::~BaseDriver() {

}


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief RemoveRedundantAllocaAndGEPInstructions::run
 ** ------------------------------------------------------------------------------------------------------------- */
PreservedAnalyses RemoveRedundantAllocaAndGEPInstructions::run(Function &F,
                                                 FunctionAnalysisManager &AM) {

    assert (!F.empty());


    SmallVector<AllocaInst *, 32> allocas;

    BasicBlock & bb = F.getEntryBlock();

#if LLVM_VERSION_INTEGER < LLVM_VERSION_CODE(20, 0, 0)
    Instruction * inst = bb.getFirstNonPHIOrDbgOrLifetime();
    while (inst) {
        #ifndef NDEBUG
        for (unsigned i = 0; i < inst->getNumOperands(); ++i) {
            Value * const op = inst->getOperand(i);
            if (op == nullptr) {
                report_fatal_error("null operand");
            }
        }
        #endif
        Instruction * const nextNode = inst->getNextNode();
        if (isa<AllocaInst>(inst) || isa<GetElementPtrInst>(inst)) {
            if (LLVM_UNLIKELY(inst->getNumUses() == 0)) {
                inst->eraseFromParent();
                inst = nextNode;
                continue;
            }
        }
        if (isa<AllocaInst>(inst)) {
            if (isAllocaPromotable(cast<AllocaInst>(inst))) {
                allocas.push_back(cast<AllocaInst>(inst));
            }
        }
        inst = nextNode;
    }
#else
    BasicBlock::iterator it = bb.getFirstNonPHIOrDbgOrLifetime();
    while (it != bb.end()) {
        Instruction &inst = *it++;

        if (isa<AllocaInst>(inst) || isa<GetElementPtrInst>(inst)) {
            if (LLVM_UNLIKELY(inst.use_empty())) {
                inst.eraseFromParent();
                continue;
            }
        }

        if (auto *allocaInst = dyn_cast<AllocaInst>(&inst)) {
            if (isAllocaPromotable(allocaInst)) {
                allocas.push_back(allocaInst);
            }
        }
    }
#endif

    if (!allocas.empty()) {
        auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
        PromoteMemToReg(allocas, DT);
    }

    return PreservedAnalyses::all();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief PHICanonicalizerPass::run
 ** ------------------------------------------------------------------------------------------------------------- */
PreservedAnalyses PHICanonicalizerPass::run(Function &F, FunctionAnalysisManager &AM) {

    assert (!F.empty());

    // LLVM is not aggressive enough with how it deals with phi nodes. To ensure that
    // we collapse every phi node in which all incoming values are identical into the
    // incoming value, we execute the following mini optimization pass.

    // TODO: check the newer versions of LLVM to see if any can do this now.

    SmallVector<BasicBlock *, 16> preds;
    SmallVector<Value *, 16> value;

    bool anyPhis = false;

    for (BasicBlock & bb : F) {

        preds.assign(pred_begin(&bb), pred_end(&bb));
        const auto n = preds.size();
        value.resize(n);

        Instruction * inst = &bb.front();
        while (isa<PHINode>(inst)) {
            PHINode * const phi = cast<PHINode>(inst);
            #ifndef NDEBUG
            if (LLVM_UNLIKELY(phi->getNumIncomingValues() != n || n == 0)) {
                bb.print(errs(), nullptr, true, false);
                errs() << "\n\nIllegal PHINode: ";
                phi->print(errs(), true);
            }
            #endif
            inst = inst->getNextNode();
            if (LLVM_LIKELY(phi->hasNUsesOrMore(1))) {
                Value * const value = phi->getIncomingValue(0);
                assert (value);
                const auto n = phi->getNumIncomingValues();
                for (unsigned i = 1; i != n; ++i) {
                    Value * const op = phi->getIncomingValue(i);
                    assert (op);
                    if (LLVM_LIKELY(op != value)) {
                        goto keep_phi_node;
                    }
                }
                phi->replaceAllUsesWith(value);
            }

            RecursivelyDeleteDeadPHINode(phi);
            continue;
            // ----------------------------------------------------------------------------------
            //  canonicalize the phi node ordering for the eliminate duplicate phi node function
            // ----------------------------------------------------------------------------------
keep_phi_node:
            bool canonicalize = false;
            for (unsigned i = 0; i != n; ++i) {
                const auto f = std::find(preds.begin(), preds.end(), phi->getIncomingBlock(i));
                assert ("phi-node has invalid incoming block?" && f != preds.end());
                const auto j = std::distance(preds.begin(), f);
                canonicalize |= (j != i);
                value[j] = phi->getIncomingValue(i);
            }
            if (canonicalize) {
                for (unsigned i = 0; i != n; ++i) {
                    phi->setIncomingBlock(i, preds[i]);
                    phi->setIncomingValue(i, value[i]);
                }
            }
            anyPhis = true;
        }
        if (LLVM_LIKELY(anyPhis)) {
            EliminateDuplicatePHINodes(&bb);
        }
    }

    // No changes, all analyses are preserved.
    return PreservedAnalyses::all();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief TracePass::run
 ** ------------------------------------------------------------------------------------------------------------- */
PreservedAnalyses TracePass::run(Function &F, FunctionAnalysisManager & AM) {

    SmallVector<Instruction *, 16> toTrace;
    for (auto & B : F) {

        assert (toTrace.empty());

        for (Instruction & inst : B) {
            if (LLVM_UNLIKELY(boost::regex_search(inst.getName().data(), TraceFilter))) {
                toTrace.push_back(&inst);
            }
        }

        if (LLVM_LIKELY(toTrace.empty())) {
            continue;
        }

        for (Instruction * I : toTrace) {
#if LLVM_VERSION_INTEGER < LLVM_VERSION_CODE(20, 0, 0)
            Instruction * N = I;
            if (LLVM_UNLIKELY(isa<PHINode>(I))) {
                N = B.getFirstNonPHIOrDbgOrLifetime();
            } else if (LLVM_LIKELY(I != B.getTerminator())) {
                assert (I->getNextNode());
                N = I->getNextNode();
            }
#else
            BasicBlock::iterator N = I->getIterator();
            if (LLVM_UNLIKELY(isa<PHINode>(I))) {
                N = B.getFirstNonPHIOrDbgOrLifetime();
            } else if (LLVM_LIKELY(I != B.getTerminator())) {
                assert(N != B.end() && "Iterator out of bounds unexpectedly");
                ++N;
            }
#endif
            b.SetInsertPoint(N);
            const Type *ty = I->getType();
            if (ty->isIntOrPtrTy()) {
                b.CallPrintInt(I->getName(), I);
            } else if (ty->isVectorTy()) {
                b.CallPrintRegister(I->getName(), I);
            }
        }
        toTrace.clear();
    }


    // No changes, all analyses are preserved.
    return PreservedAnalyses::all();

}
