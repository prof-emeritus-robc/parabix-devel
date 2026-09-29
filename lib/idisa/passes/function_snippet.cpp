#include <idisa/passes/function_snippet.h>
#include <llvm/IR/Function.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineRegisterInfo.h>
#include <llvm/CodeGen/MachineInstrBuilder.h>
#include <llvm/CodeGen/MachineBranchProbabilityInfo.h>
#include <llvm/CodeGen/MachineFunctionPass.h>
#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/CodeGen/TargetInstrInfo.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <llvm/CodeGen/MachineConstantPool.h>
#include <llvm/CodeGen/MachineJumpTableInfo.h>
#include <llvm/IR/ValueSymbolTable.h>
#include <llvm/CodeGen/Passes.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/CodeGen/TargetLowering.h>
#include <llvm/CodeGen/CallingConvLower.h>
#include <llvm/MC/MCRegisterInfo.h>
#include <llvm/CodeGen/Analysis.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/CodeGen/MachineSSAUpdater.h>
#include <llvm/CodeGen/LiveVariables.h>
#include <llvm/CodeGen/LiveIntervals.h>
#include <llvm/CodeGen/MachineValueType.h>

#define BEGIN_SCOPED_REGION {
#define END_SCOPED_REGION }

constexpr auto MAXIMUM_SYMBOL_LENGTH = 4096;

using namespace llvm;

constexpr static StringRef SNIPPET_PREFIX{"_snippet."};

class FunctionSnippetTokenReplacerPass : public MachineFunctionPass {
public:
    static StringRef FUNCTION_SNIPPET_METADATA_LABEL;

    static inline char ID = 0;

    FunctionSnippetTokenReplacerPass()
    : MachineFunctionPass(ID) {

    }

    bool doInitialization(Module & M) override;

    bool runOnMachineFunction(MachineFunction & MF) override;

    bool doFinalization(Module & M) override;

    void getAnalysisUsage(AnalysisUsage & AU) const override;

private:

//    struct CacheOperand {
//        MachineOperand MO;
//        unsigned Flags = 0;
//        union {
//            const TargetRegisterClass * RegClass = nullptr;
//            size_t BasicBlockId;
//        };

//        CacheOperand(const MachineOperand & src)
//        : MO(src) {
//            MO.clearParent();
//        }
//    };

//    struct CacheInst {
//        unsigned Opcode;
//        uint64_t Flags;
//        DebugLoc DL;
//        SmallVector<CacheOperand, 2> Operands;
//        SmallVector<std::pair<unsigned, unsigned>, 0> TiedOperands;
//        SmallVector<unsigned, 0> EarlyClobberOperands;

//        CacheInst() = default;

//        CacheInst(const MachineInstr & MI)
//        : Opcode(MI.getOpcode())
//        , Flags(MI.getFlags())
//        , DL(MI.getDebugLoc()) {

//        }
//    };

    using RegMappingList = std::vector<std::pair<MCRegister, Register>>;

    using RegisterMap = DenseMap<Register, Register, DenseMapInfo<Register>>;

//    struct CacheBasicBlock {
//        const BasicBlock * Source = nullptr;
//        std::vector<CacheInst> Instructions;
//        SmallVector<std::pair<size_t, BranchProbability>, 2> Successors;
//        RegMappingList LiveOuts;

//        CacheBasicBlock(const BasicBlock * bb = nullptr) : Source(bb) {}
//    };

//    struct CachedMachineFunction {
//        std::vector<CacheBasicBlock> BasicBlock;
//        RegMappingList LiveIns;
//        DenseMap<Register, const TargetRegisterClass *, DenseMapInfo<Register>> RegClassMap;
//        std::vector<MCRegister> LiveOuts;
//        size_t NumOfExitBlocks = 0;
//        std::unique_ptr<MachineFunction> MF;
//    };


    struct CacheBasicBlock {
        std::vector<const MachineInstr *> Instructions;
        std::vector<std::pair<size_t, size_t>> MBBOperandSubsitutionList;

        SmallVector<std::pair<size_t, BranchProbability>, 2> Successors;
        bool IsFunctionExit = false;
    };

    struct CachedMachineFunction {
        std::unique_ptr<MachineFunction> MF;
        std::vector<CacheBasicBlock> BasicBlock;
        std::vector<std::pair<MCRegister, Register>> LiveIns;
        std::vector<std::pair<MCRegister, Register>> LiveOuts;
        DenseMap<Register, const TargetRegisterClass *, DenseMapInfo<Register>> RegClassMap;

        size_t NumOfLiveOuts = 0;
        size_t NumOfExitBlocks = 0;
    };

    void serializeToCache(Function & F, MachineFunction & MF, MachineModuleInfo & MMI);

    const Function * getCalleeFunction(Module * M, MachineFunction & MF, MachineInstr & call) const;

    void getInputArgumentMapping(const MachineFunction & MF,
                                 MachineBasicBlock & MBB, MachineBasicBlock::instr_iterator callsite,
                                 const CachedMachineFunction & cache,
                                 RegisterMap & globalMap);

    void getOutputArgumentMapping(const MachineFunction &MF,
                                  MachineBasicBlock &MBB, MachineBasicBlock::instr_iterator callsite,
                                  const CachedMachineFunction & cachedMF,
                                  RegisterMap & callerRetMap);



private:
    DenseMap<Function *, CachedMachineFunction, DenseMapInfo<Function *>> Cache;
};

bool FunctionSnippetTokenReplacerPass::doInitialization(Module & M)  {
    return false;
}

bool FunctionSnippetTokenReplacerPass::doFinalization(Module & M)  {

//    if (Cache.size() > 0) {
//        auto & MMI = getAnalysis<MachineModuleInfoWrapperPass>().getMMI();
//        for (auto r : Cache) {
//            Function & irFunc = *r.first;
//            MMI.deleteMachineFunctionFor(irFunc);
//            irFunc.replaceAllUsesWith(UndefValue::get(irFunc.getType()));
//            irFunc.eraseFromParent();
//        }
//    }

//    return (!Cache.empty());

    return false;
}

MachineFunctionPass * createFunctionSnippetTokenReplacerPass() {
    return new FunctionSnippetTokenReplacerPass();
}

StringRef FunctionSnippetTokenReplacerPass::FUNCTION_SNIPPET_METADATA_LABEL{"snippetlink"};


Register extractCallerExpectedDestination(MachineFunction & MF,
                                          const MachineBasicBlock & MBB,
                                          const MachineInstr & MI,

                                          MachineInstr *& defunctSelDagCopy) {
    if (MI.getNumDefs() > 0) {
        auto op0 = MI.getOperand(0);
        if (op0.isReg() && op0.isDef()) {
            auto reg = op0.getReg();
            if (reg.isVirtual()) {
                defunctSelDagCopy = nullptr;
                return reg;
            }
        }
    }

    const MachineRegisterInfo & MRI = MF.getRegInfo();

    for (const MachineOperand & MO : MI.operands()) {
        if (MO.isReg() && MO.isDef()) {
            auto reg = MO.getReg();
            if (reg.isPhysical()) {
                auto phyReg = reg.asMCReg();
                for (MachineOperand & UO : MRI.use_operands(phyReg)) {
                    MachineInstr * UI = UO.getParent();
                    if (UI && UI->getParent() == &MBB) {
                        if (UI->isCopyLike() || UI->isMoveReg()) {
                            auto virtDest = UI->getOperand(0).getReg();
                            if (virtDest.isVirtual()) {
                                defunctSelDagCopy = UI;
                                return virtDest;
                            }
                        }
                    }
                }
            }
        }
    }
    return Register{};
}

bool FunctionSnippetTokenReplacerPass::runOnMachineFunction(MachineFunction & MF) {

    using Prop = MachineFunctionProperties::Property;



    const auto & props = MF.getProperties();

    const auto usedGlobalISel = props.hasProperty(Prop::Legalized) || props.hasProperty(Prop::RegBankSelected);

    auto & MMI = getAnalysis<MachineModuleInfoWrapperPass>().getMMI();
    auto & MBPI = getAnalysis<MachineBranchProbabilityInfo>();
    auto & MRI = MF.getRegInfo();


    const TargetInstrInfo * const TII = MF.getSubtarget().getInstrInfo();

    RegisterMap GlobalRegMap;

    RegisterMap CallerRetMap;

    DenseMap<Register, MachineInstr *, DenseMapInfo<Register>> OutgoingPhi;

    SmallVector<MachineBasicBlock *, 16> InlinedBasicBlockList;

    using InstrIterator = MachineBasicBlock::instr_iterator;

    const TargetRegisterInfo * TRI = MF.getSubtarget().getRegisterInfo();

    const auto * const CallMask = TRI->getCallPreservedMask(MF, MF.getFunction().getCallingConv());

    auto doSplice = [&](const Function * const callee,
            MachineBasicBlock & MBB, const InstrIterator callsite,
            const CachedMachineFunction & cachedMF, InstrIterator & next) -> bool {

        assert (callsite->isCall());

        assert (GlobalRegMap.empty());

        auto getRegister = [&](Register calleeReg) -> Register {
            if (calleeReg.isPhysical() || !calleeReg.isValid()) {
                return calleeReg;
            }
            auto f = GlobalRegMap.find(calleeReg);
            assert (f != GlobalRegMap.end());
            return f->second;
        };

        callsite->print(errs());

        // Add the caller register -> callee input mappings
        getInputArgumentMapping(MF, MBB, callsite, cachedMF, GlobalRegMap);

        // Add the callee register -> caller output mappings
        getOutputArgumentMapping(MF, MBB, callsite, cachedMF, CallerRetMap);

//        MachineInstr * defunctCopy = nullptr;
//        auto destReg = extractCallerExpectedDestination(MF, MBB, *callsite, defunctCopy);
//        assert (destReg.isValid() && destReg.isVirtual());

//        const TargetRegisterClass * RC = MRI.getRegClass(destReg);
//        Register scratchOutput = MRI.createVirtualRegister(RC);
//        GlobalRegMap.insert(std::make_pair(destReg, scratchOutput));



        const auto & CBBs = cachedMF.BasicBlock;

        auto & HRI = MF.getRegInfo();
        const auto numOfCachedMachineBasicBlocks = CBBs.size();
        assert (numOfCachedMachineBasicBlocks > 0);

        if (LLVM_LIKELY(numOfCachedMachineBasicBlocks == 1)) {

            const auto & CBB = CBBs[0];

//            assert (cachedMF.NumOfExitBlocks == 1);
            assert (CBB.IsFunctionExit);

            errs() << "INSERTED CODE:\n";

            for (const MachineInstr * I : CBB.Instructions) {

                assert (!I->isReturn());
                MachineInstr * const MI = MF.CloneMachineInstr(I);

                const auto m = MI->getNumOperands();
                for (size_t i = 0; i < m; ++i) {
                    auto & MO = MI->getOperand(i);
                    if (MO.isReg()) {
                        const auto calleeReg = MO.getReg();
                        if (calleeReg.isVirtual()) {
                            if (MO.isUse()) {
                                auto f = GlobalRegMap.find(calleeReg);
                                assert (f != GlobalRegMap.end());
                                MO.setReg(f->second);
                            } else {
                                auto f = CallerRetMap.find(calleeReg);
                                if (f == CallerRetMap.end()) {
                                    auto r = cachedMF.RegClassMap.find(calleeReg);
                                    assert (r != cachedMF.RegClassMap.end());
                                    Register newReg = MRI.createVirtualRegister(r->second);
                                    GlobalRegMap[calleeReg] = newReg;
                                    MO.setReg(newReg);
                                } else {
                                    MO.setReg(f->second);
                                }
                            }
                        }
                    }
                }

                MBB.insert(callsite, MI);

             //   MI->cloneMemRefs(*cachedMF.MF, *I);

                MI->print(errs());

            }

            errs() << "\n\n";

        } else { // snippet function has multiple basicblocks.

            InlinedBasicBlockList.resize(numOfCachedMachineBasicBlocks);
            InlinedBasicBlockList[0] = &MBB;

            auto insertPoint = std::next(MBB.getIterator());
            for (unsigned i = 1; i < numOfCachedMachineBasicBlocks; ++i) {
                auto newBB = MF.CreateMachineBasicBlock();
                InlinedBasicBlockList[i] = newBB;
                MF.insert(insertPoint, newBB);
            }

            MachineBasicBlock * exitBlock = nullptr;

//            assert (cachedMF.NumOfExitBlocks == 1);

//            if (LLVM_LIKELY(cachedMF.NumOfExitBlocks != 1)) {
//                exitBlock = MF.CreateMachineBasicBlock();

//                auto PhiType = TII->get(TargetOpcode::PHI);

//                assert (OutgoingPhi.empty());
//                for (auto & entry : CallerRetMap) {
//                    const auto c = MRI.getRegClass(entry.first);
//                    auto phiReg = MRI.createVirtualRegister(c);
//                    auto phiIB = BuildMI(*exitBlock, exitBlock->end(), DebugLoc{}, PhiType, phiReg);
//                    auto phiNode = phiIB.getInstr();
//                    MRI.replaceRegWith(entry.second, phiReg);
//                    entry.second = phiReg;
//                    OutgoingPhi.insert(std::make_pair(entry.first, phiNode));
//                }

//                MBB.splice(exitBlock->end(), exitBlock, callsite);
//                MF.insert(insertPoint, exitBlock);
//            }

            for (unsigned i = 0; i < numOfCachedMachineBasicBlocks; ++i) {

                const auto & CBB = CBBs[i];

                MachineBasicBlock * const targetBB = InlinedBasicBlockList[i];
                for (const auto & s : CBB.Successors) {
                    MachineBasicBlock * newSucc = InlinedBasicBlockList[s.first];
                    targetBB->addSuccessor(newSucc, s.second);
                }

                const auto m = CBB.Instructions.size();

                const auto & subList = CBB.MBBOperandSubsitutionList;

                auto nextImmSubsitution = subList.begin();

                for (size_t j = 0; j < m; ++j) {

                    const MachineInstr * I = CBB.Instructions[j];

                    MachineInstr * const MI = MF.CloneMachineInstr(I);

                    targetBB->insert(targetBB->end(), MI);

                    const auto n = MI->getNumOperands();

                    for (size_t k = 0; k < n; ++k) {
                        MachineOperand & MO = MI->getOperand(k);
                        if (MO.isReg()) {
                            auto reg = MO.getReg();
                            if (reg.isVirtual()) {
                                const auto newReg = getRegister(reg);
                                MO.ChangeToRegister(newReg, MO.isDef(), MO.isImplicit(), MO.isKill(), MO.isDead(), MO.isUndef(), MO.isDebug());
                            }
                        } else if (MO.isImm()) {
                            if (nextImmSubsitution != subList.end()) {
                                if (nextImmSubsitution->first == j && nextImmSubsitution->second == k) {
                                    MO.setMBB(InlinedBasicBlockList[MO.getImm()]);
                                    ++nextImmSubsitution;
                                }
                            }
                        }
                    }

                }

                assert (nextImmSubsitution == subList.end());

//                assert (cachedMF.NumOfExitBlocks == 1);

                if (LLVM_UNLIKELY(CBB.IsFunctionExit)) {
                    MBB.splice(targetBB->end(), targetBB, callsite);
                }
            }

        };

        GlobalRegMap.clear();

//        if (defunctCopy) {
//            defunctCopy->eraseFromParent();
//        }

//        MachineBasicBlock & exit = *callsite->getParent();
//        const auto & dl = callsite->getDebugLoc();
//        BuildMI(exit, callsite, dl, TII->get(TargetOpcode::COPY), destReg).addReg(scratchOutput);
//        for (const auto & mo : callsite->operands()) {
//            if (mo.isRegMask()) {
//                BuildMI(exit, callsite, dl, TII->get(TargetOpcode::KILL)).addRegMask(mo.getRegMask());
//                break;
//            }
//        }

        next = std::next(callsite);
        callsite->eraseFromParent();
        return (numOfCachedMachineBasicBlocks > 0);
    };


    bool Changed = false;
    bool ModifiedCFG = false;

    Function & F = MF.getFunction();


    Module * const M = F.getParent();

    if (Cache.size() > 0) {
        auto mbb_itr = MF.begin();
        auto mbb_end = MF.end();
        while (mbb_itr != mbb_end) {
            auto itr = mbb_itr->instr_begin();
restart_after_callsite:
            auto end = mbb_itr->instr_end();
            while (itr != end) {
                MachineInstr & I = *itr;
                if (I.isCall()) {
                    const Function * const callee = getCalleeFunction(M, MF, I);
                    if (callee && callee->hasMetadata(FunctionSnippetTokenReplacerPass::FUNCTION_SNIPPET_METADATA_LABEL)) {
                        const auto f = Cache.find(callee);
                        assert (f != Cache.end());
                        const auto & cachedMF = f->second;

                        InstrIterator next;
                        const auto modCFG = doSplice(callee, *mbb_itr, itr, cachedMF, next);
                        Changed = true;

//                        if (Changed) {
//                            MRI.invalidateLiveness();
//                            auto & prop = MF.getProperties();
//                            prop.reset(MachineFunctionProperties::Property::TracksLiveness);
//                            if (auto * LV = getAnalysisIfAvailable<LiveVariables>()) {
//                                LV->releaseMemory();
//                            }
//                            if (auto * LIS = getAnalysisIfAvailable<LiveIntervals>()) {
//                                LIS->releaseMemory();
//                            }
//                        }

                        #ifndef NDEBUG
                        MF.verify(nullptr, "Function Snippet Replacement error", true);
                        #endif

                        if (modCFG) {
                            itr = next;
                            mbb_itr = next->getParent()->getIterator();
                            mbb_end = MF.end();
                            ModifiedCFG = true;
                            goto restart_after_callsite;
                        } else {
                            itr = next;
                        }
                        continue;
                    }
                }
                ++itr;
            }
            ++mbb_itr;
        }

    }

    if (F.getMetadata(FUNCTION_SNIPPET_METADATA_LABEL)) {

        errs() << " -- adding snippet from " << MF.getName() << "\n";

        assert (F.getCallingConv() == CallingConv::Fast);

        serializeToCache(F, MF, MMI);

        errs() << " -- added snippet from " << MF.getName() << "\n";

    }

    if (Changed) {
        auto & props = MF.getProperties();
        props.set(MachineFunctionProperties::Property::IsSSA);
        props.reset(MachineFunctionProperties::Property::TracksLiveness);
        MRI.invalidateLiveness();
    }

    #ifndef NDEBUG
    MF.verify(nullptr, "Function Snippet Replacement error", true);
    #endif
    return Changed;
}

const Function * FunctionSnippetTokenReplacerPass::getCalleeFunction(Module * M, MachineFunction & MF, MachineInstr & call) const {
    if (call.getNumOperands() == 0) {
        return nullptr;
    }
    const auto & targetOp = call.getOperand(0);
    if (targetOp.isReg()) {
        auto reg = targetOp.getReg();
        if (reg.isVirtual()) {
            SmallVector<MachineInstr *, 4> potential;
            SmallPtrSet<MachineInstr *, 4> visited;

            auto & MRI = MF.getRegInfo();
            auto defMI = MRI.getVRegDef(reg);

            if (defMI) {

                visited.insert(defMI);

                for (;;) {

                    const auto n = defMI->getNumOperands();
                    if (LLVM_UNLIKELY(defMI->isPHI())) {
                        for (size_t i = 1; i < n; i += 2) {
                            auto reg = defMI->getOperand(i).getReg();
                            if (reg.isVirtual()) {
                                auto nextDefMI = MRI.getVRegDef(reg);
                                if (nextDefMI && visited.insert(nextDefMI).second) {
                                    potential.push_back(nextDefMI);
                                }
                            }
                        }
                    } else {

                        for (size_t i = 0; i < n; ++i) {
                            const auto & callee = defMI->getOperand(i);
                            if (callee.isGlobal()) {
                                const Function * const f = dyn_cast<Function>(callee.getGlobal());
                                if (f) {
                                    return f;
                                }
                            }
                            if (callee.isSymbol()) {
                                auto symbolName = callee.getSymbolName();
                                assert (symbolName);
                                const auto prefix = MF.getDataLayout().getGlobalPrefix();
                                if (prefix && *symbolName == prefix)  {
                                    ++symbolName;
                                }
                                // Any SymbolName over 4K is almost-certainly malicious
                                StringRef nm{symbolName, strnlen(symbolName, MAXIMUM_SYMBOL_LENGTH)};
                                const Function * const f = M->getFunction(nm);
                                if (f) {
                                    return f;
                                }
                            }
                        }

                    }

                    if (potential.empty()) {
                        break;
                    }

                    defMI = potential.pop_back_val();
                }
            }
        }
    }


    return nullptr;
}

using ExpectedRegSet = SmallPtrSet<MCRegister, 16>;

bool inline isRegAssignment(const MachineInstr & MI) {
    if (MI.isCopyLike() || MI.isMoveReg()) {
        return true;
    }
    const auto opc = MI.getOpcode();
    if (llvm::isPreISelGenericOpcode(opc) || llvm::isPreISelGenericOptimizationHint(opc)) {
        if (MI.getNumOperands() == 2) {
            const MachineOperand & dst = MI.getOperand(0);
            const MachineOperand & src = MI.getOperand(1);
            return (dst.isReg() && src.isReg() && dst.isDef() && src.isUse());
        }
    }
    return false;
}

void FunctionSnippetTokenReplacerPass::getInputArgumentMapping(const MachineFunction & MF,
                                                               MachineBasicBlock & MBB, MachineBasicBlock::instr_iterator callsite,
                                                               const CachedMachineFunction & cache,
                                                               RegisterMap & globalMap) {
    assert (callsite->getParent() == &MBB);
    if (LLVM_UNLIKELY(cache.LiveIns.empty())) {
        return;
    }

    const TargetRegisterInfo * const CallerTRI = MF.getSubtarget().getRegisterInfo();

    const auto expectedCount = globalMap.size() + cache.LiveIns.size();

    auto itr = std::next(callsite->getReverseIterator());
find_next:
    for (;;) {
        assert (itr != MBB.rend());
        MachineInstr & MI = *itr++;
        if (isRegAssignment(MI)) {
            const MachineOperand & dst = MI.getOperand(0);
            auto dstReg = dst.getReg();
            if (dstReg.isPhysical()) {
                MCRegAliasIterator alias(dstReg.asMCReg(), CallerTRI, true);
                for (; alias.isValid(); ++alias) {
                    for (const auto & liveIn : cache.LiveIns) {
                        MCRegister phyReg = liveIn.first;
                        if (*alias == phyReg) {
                            const MachineOperand & src = MI.getOperand(1);
                            const auto callerVirtReg = src.getReg();
                            if (callerVirtReg.isVirtual()) {
                                auto calleeVirtReg = liveIn.second; assert (calleeVirtReg.isVirtual());
                                errs() << "IN: "; MI.print(errs());
                                assert (globalMap.count(calleeVirtReg) == 0);
                                globalMap.insert(std::make_pair(calleeVirtReg, callerVirtReg));
                                MI.eraseFromParent();
                                if (globalMap.size() == expectedCount) {
                                    return;
                                }
                                goto find_next;
                            }
                        }
                    }
                }
            }
        }
    }

}

void FunctionSnippetTokenReplacerPass::getOutputArgumentMapping(const MachineFunction & MF,
                                                               MachineBasicBlock & MBB, MachineBasicBlock::instr_iterator callsite,
                                                               const CachedMachineFunction & cache,
                                                               RegisterMap & callerRetMap) {
    assert (callerRetMap.empty());
    assert (callsite->getParent() == &MBB);
    if (LLVM_UNLIKELY(cache.LiveOuts.empty())) {
        return;
    }

    const TargetRegisterInfo * const CallerTRI = MF.getSubtarget().getRegisterInfo();

    MachineBasicBlock::instr_iterator itr = std::next(callsite);
find_next:
    while (itr != MBB.end()) {
        auto & MI = *itr++;
        if (isRegAssignment(MI)) {
            auto MO = MI.getOperand(1);
            if (LLVM_LIKELY(MO.isReg())) {
                const auto reg = MO.getReg();
                if (reg.isPhysical()) {
                    MCRegAliasIterator alias(reg.asMCReg(), CallerTRI, true);
                    for (; alias.isValid(); ++alias) {
                        for (const auto & liveOut : cache.LiveOuts) {
                            MCRegister phyReg = liveOut.first;
                            if (phyReg == *alias) {
                                auto callerVirtReg = MI.getOperand(0).getReg();
                                const auto calleeVirtReg = liveOut.second; assert (calleeVirtReg.isVirtual());
                                errs() << "OUT: "; MI.print(errs());
                                assert (callerRetMap.count(calleeVirtReg) == 0);
                                callerRetMap.insert(std::make_pair(calleeVirtReg, callerVirtReg));
                                MI.eraseFromParent();
                                if (callerRetMap.size() == cache.LiveOuts.size()) {
                                    return;
                                }
                                goto find_next;
                            }
                        }
                    }
                }
            }
        }
    }

}

inline size_t getABIReturnRegisterCount(const MachineFunction & MF, const TargetLowering * TLI, Type * const RetTy) {

    if (RetTy == nullptr || RetTy->isVoidTy()) {
        return 0;
    }

    const DataLayout & DL = MF.getDataLayout();

    SmallVector<EVT, 4> RetTys;
    ComputeValueVTs(*TLI, DL, RetTy, RetTys);

    auto & C = RetTy->getContext();

    size_t numRegisters = 0;
    for (auto & EVT : RetTys) {
        numRegisters += TLI->getNumRegistersForCallingConv(C, CallingConv::Fast, EVT);
    }
    return numRegisters;
}


inline void FunctionSnippetTokenReplacerPass::serializeToCache(Function &F, MachineFunction &MF, MachineModuleInfo & MMI) {

    assert (F.getCallingConv() == CallingConv::Fast);


    const MachineRegisterInfo & MRI = MF.getRegInfo();

    const TargetSubtargetInfo & TSI = MF.getSubtarget();

    const TargetInstrInfo * const TII = TSI.getInstrInfo();

    const TargetLowering * TLI = MF.getSubtarget().getTargetLowering();

    const TargetRegisterInfo * TRI = TSI.getRegisterInfo();

    errs() << "--------------------------------------------------------\n";
    errs() << "CALLEE FUNCTION: " << MF.getName() << "\n";
    errs() << "--------------------------------------------------------\n";
    MF.print(errs());
    errs() << "--------------------------------------------------------\n";


    auto & MBPI = getAnalysis<MachineBranchProbabilityInfo>();

    CachedMachineFunction cache;

    cache.MF = std::make_unique<MachineFunction>(F, MF.getTarget(), TSI, 0, MMI);

    MachineRegisterInfo & newMRI = cache.MF->getRegInfo();

    const auto numOfVirtRegs = MRI.getNumVirtRegs();
    DenseMap<Register, Register, DenseMapInfo<Register>> RegisterMapping;
    std::vector<Register> CreatedVirtualRegisters;

//    cache.VRegClass.resize(numOfVirtRegs);
//    for (size_t i = 0; i < numOfVirtRegs; ++i) {
//        auto vreg = Register::index2VirtReg(i);
//        auto c = MRI.getRegClass(vreg); assert (c);
//        auto v = newMRI.createVirtualRegister(c);
//        virtRegMap.insert(vreg, v);
//    }

    DenseSet<const MachineInstr *, DenseMapInfo<MachineInstr *>> skipped;

    for (const auto & LI : MRI.liveins()) {
        Register reg = LI.second; assert (reg.isVirtual());

        for (;;) {
            const MachineInstr * const def = MRI.getVRegDef(reg);
            skipped.insert(def);

            auto useItr = MRI.use_instr_begin(reg);
            if (std::next(useItr) != MRI.use_instr_end()) {
                break;
            }

            const MachineInstr & next = *useItr;
            if (!next.isCopyLike() && !next.isMoveReg()) {
               break;
            }

            reg = next



        }




        const TargetRegisterClass * c = MRI.getRegClass(reg);
        auto v = newMRI.createVirtualRegister(c);
        cache.LiveIns.emplace_back(LI.first, v);
        RegisterMapping.insert(std::make_pair(LI.first, v));
        RegisterMapping.insert(std::make_pair(LI.second, v));
        CreatedVirtualRegisters.push_back(v);

    }

    if (const MachineConstantPool * src = MF.getConstantPool()) {
        MachineConstantPool * dst = cache.MF->getConstantPool();
        for (const auto & entry : src->getConstants()) {
            dst->getConstantPoolIndex(entry.Val.ConstVal, entry.getAlign());
        }
    }

    DenseMap<const MachineBasicBlock *, size_t, DenseMapInfo<const MachineBasicBlock *>> ClonedMBB;



    const auto n = MF.size();

    cache.BasicBlock.resize(n);

    BEGIN_SCOPED_REGION
    auto MBBItr = MF.begin();
    for (size_t i = 0; i < n; ++i, ++MBBItr) {
        const MachineBasicBlock * bb = &*MBBItr;
        ClonedMBB.insert(std::make_pair(bb, i));
    }
    END_SCOPED_REGION

//    if (const MachineJumpTableInfo * src = MF.getJumpTableInfo()) {
//        MachineJumpTableInfo * dst = cache.MF->getOrCreateJumpTableInfo(src->getEntryKind());
//        std::vector<MachineBasicBlock *> targets;
//        for (const auto & entry : src->getJumpTables()) {
//            assert (targets.empty());
//            for (const MachineBasicBlock * srcMBB : entry.MBBs) {
//                const auto f = ClonedMBB.find(srcMBB);
//                assert (f != ClonedMBB.end());
//                targets.emplace_back(f->second);
//            }
//            dst->createJumpTableIndex(targets);
//            targets.clear();
//        }
//    }

//    cache.NumOfLiveOuts = getABIReturnRegisterCount(MF, TLI, F.getReturnType());

    const auto frameSetup = TII->getCallFrameSetupOpcode();
    const auto frameDestroy= TII->getCallFrameDestroyOpcode();

    size_t numOfReturnInsts = 0;

    auto MBBItr = MF.begin();
    for (size_t i = 0; i < n; ++i) {
        assert (MBBItr != MF.end());
        const MachineBasicBlock & MBB = *MBBItr++;
        auto & CBB = cache.BasicBlock[i];

        CBB.Successors.reserve(MBB.succ_size());
        for (auto * succ : MBB.successors()) {
            auto prob = MBPI.getEdgeProbability(&MBB, succ);
            const auto f = ClonedMBB.find(succ);
            assert (f != ClonedMBB.end());
            CBB.Successors.emplace_back(f->second, prob);
        }

        if (LLVM_LIKELY(MBB.isReturnBlock())) {

            for (const MachineInstr & term : MBB.terminators()) {
                if (term.isReturn()) {
                    for (auto & mo : term.operands()) {
                        if (mo.isReg() && mo.isUse()) {
                            const auto reg = mo.getReg().asMCReg();
                            auto itr = std::next(term.getReverseIterator());
                            for (;;++itr) {
                                assert (itr != MBB.rend());
                                if (itr->isCopyLike() || itr->isMoveReg()) {
                                    if (itr->getOperand(0).getReg() == reg) {
                                        auto defVirtReg = itr->getOperand(1).getReg();
                                        const auto * inst = &(*itr);
                                        for (;;) {
                                            skipped.insert(inst);
                                            const auto * const next = MRI.getUniqueVRegDef(defVirtReg);
                                            if (LLVM_LIKELY(!next || (!next->isCopyLike() && !next->isMoveReg()))) {
                                                break;
                                            }
                                            defVirtReg = next->getOperand(1).getReg();
                                            inst = next;
                                        }
                                        RegisterMapping.insert(std::make_pair(reg, defVirtReg));
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }


        const auto m = MBB.size();
        auto MIItr = MBB.begin();
        for (size_t j = 0; j < m; ++j) {
            const auto & MI = *MIItr++;
            if (MI.getOpcode() == frameSetup || MI.getOpcode() == frameDestroy || skipped.count(&MI)) {
                continue;
            }

            if (MI.isReturn()) {
                assert (cache.LiveOuts.empty());
                for (auto & mo : MI.operands()) {
                    if (mo.isReg() && mo.isUse() && !mo.isImplicit()) {
                        auto reg = mo.getReg();
                        assert (reg.isPhysical());
                        auto f = RegisterMapping.find(reg);
                        assert (f != RegisterMapping.end());
                        const auto v = f->second;
                        cache.LiveOuts.emplace_back(reg.asMCReg(), v);
                    }
                }

                assert (getABIReturnRegisterCount(MF, TLI, F.getReturnType()) == cache.LiveOuts.size());
                CBB.IsFunctionExit = true;
                numOfReturnInsts++;
            } else {

                MachineInstr * const cloned = cache.MF->CloneMachineInstr(&MI);
                if (!MI.memoperands_empty()) {
                    cloned->setMemRefs(*cache.MF, MI.memoperands());
                }
                cloned->setDebugLoc(DebugLoc());

                const auto l = cloned->getNumOperands();
                for (size_t k = 0; k < l; ++k) {
                    const auto & MO = MI.getOperand(k);
                    auto & MC = cloned->getOperand(k);

                    if (MO.isReg() && MO.isUse()) {
                        const auto reg = MO.getReg();
                        if (LLVM_LIKELY(reg.isValid())) {
                            auto f = RegisterMapping.find(reg);
                            assert (f != RegisterMapping.end());
                            MC.setReg(f->second);
                        }
                    } else if (MO.isMBB()) {
                        const auto f = ClonedMBB.find(MO.getMBB());
                        assert (f != ClonedMBB.end());
                        MC.ChangeToImmediate(f->second);
                        CBB.MBBOperandSubsitutionList.emplace_back(j, k);
                    }
                }

                for (size_t k = 0; k < l; ++k) {
                    const auto & MO = MI.getOperand(k);
                    auto & MC = cloned->getOperand(k);
                    if (MO.isReg() && MO.isDef()) {
                        auto reg = MO.getReg();
                        if (LLVM_LIKELY(reg.isValid())) {
                            if (reg.isVirtual()) {
                                auto f = RegisterMapping.find(reg);
                                if (LLVM_LIKELY(f == RegisterMapping.end())) {
                                    const TargetRegisterClass * c = MRI.getRegClass(reg);
                                    auto v = newMRI.createVirtualRegister(c);
                                    RegisterMapping.insert(std::make_pair(reg, v));
                                    CreatedVirtualRegisters.push_back(v);
                                    MC.setReg(v);
                                } else {
                                    MC.setReg(f->second);
                                }
                            } else if (reg.isPhysical()) {
                                MCRegister phyReg = reg.asMCReg();
                                const TargetRegisterClass * c = MI.getRegClassConstraint(k, TII, TRI);
                                if (LLVM_LIKELY(c)) goto found_phy_class;
                                // If the above check fails, MI may be a polymorphic operation. Check the users to see
                                // if we can infer what the class is.
                                for (const MachineOperand & UO : MRI.use_operands(phyReg)) {
                                    const MachineInstr * UI = UO.getParent();
                                    if (LLVM_LIKELY(UI && UI->getParent() == MI.getParent())) {
                                        for (unsigned opIdx = 0, m = UI->getNumOperands(); opIdx != m; ++ opIdx) {
                                            const MachineOperand & Op = UI->getOperand(opIdx);
                                            if (&Op == &UO) {
                                                c = UI->getRegClassConstraint(opIdx, TII, TRI);
                                                if (LLVM_LIKELY(c)) goto found_phy_class;
                                            }
                                        }
                                    }
                                }
                                // If that fails but this is a register assignment, try to infer this operand's class
                                // from the explicit use operand.
                                if (MI.isCopyLike() || MI.isMoveReg()) {
                                    for (size_t defIdx = 0; defIdx < l; ++defIdx) {
                                        const MachineOperand & DO = MI.getOperand(defIdx);
                                        if (DO.isReg() && DO.isUse() && !DO.isImplicit()) {
                                            auto & DC = cloned->getOperand(defIdx);
                                            c = newMRI.getRegClass(DC.getReg());
                                            if (LLVM_LIKELY(c)) goto found_phy_class;
                                        }
                                    }
                                }
                                // If all the checks fail, this must be implicit status flag and anything large enough
                                // will work. The actual value won't be in the compiled code.
                                assert (MO.isImplicit());
                                c = TRI->getMinimalPhysRegClass(phyReg, MVT::Other); assert (c);
                            found_phy_class:
                                if (LLVM_LIKELY(c->isAllocatable())) {
                                    auto v = newMRI.createVirtualRegister(c);
                                    RegisterMapping[reg] = v;
                                    CreatedVirtualRegisters.push_back(v);
                                    MC.setReg(v);
                                }
                            }
                        }
                    }
                }

                CBB.Instructions.push_back(cloned);
            }
        }
    }

    assert (MBBItr == MF.end());

    assert (numOfReturnInsts == 1);

    for (const auto & v : CreatedVirtualRegisters) {
        const TargetRegisterClass * c = newMRI.getRegClass(v); assert (c);
        cache.RegClassMap.insert(std::make_pair(v, c));
    }

    Cache.insert(std::make_pair(&F, std::move(cache)));

}

void FunctionSnippetTokenReplacerPass::getAnalysisUsage(AnalysisUsage & AU) const {
    AU.addRequired<MachineModuleInfoWrapperPass>();
    AU.addRequired<MachineBranchProbabilityInfo>();
    MachineFunctionPass::getAnalysisUsage(AU);
}

Value * CallFunctionByToken(IDISA::IDISA_Builder & b, Type * retTy, StringRef name, ArrayRef<Value *> params,
                            std::function<Value *(ArrayRef<Value *>)> functionGenerator) {

    Module * const m = b.getModule();

    SmallVector<char, 128> tmp;
    raw_svector_ostream nm(tmp);
    nm << SNIPPET_PREFIX << name;

    const auto n = params.size();
    std::vector<Type *> paramTys(n);
    for (size_t i = 0; i < n; ++i) {
        paramTys[i] = params[i]->getType();
    }



    FunctionType * const funcTy = FunctionType::get(retTy, paramTys, false);

    Function * snippetFunction = m->getFunction(nm.str());
    if (snippetFunction == nullptr) {

        Function * const callerFunction = b.GetInsertPoint()->getParent()->getParent();

        auto ip = b.saveIP();

        snippetFunction = Function::Create(funcTy, Function::InternalLinkage, nm.str(), m);


//        snippetFunction->removeFromParent();

//        auto & funcList = m->getFunctionList();
//        funcList.insert(callerFunction->getIterator(), snippetFunction);

//        m->getValueSymbolTable().
//        snippetFunction->setName(nm.str());

//        callerFunction->getIterator().

//        funcList.splice(callerFunction->getIterator(), funcList, snippetFunction->getIterator());

        //        snippetFunction->setVisibility(Function::HiddenVisibility);
                snippetFunction->setCallingConv(CallingConv::Fast);

        //        appendToCompilerUsed(*m, {snippetFunction});

        std::array<Metadata *, 1> C;
        C[0] = ValueAsMetadata::get(snippetFunction);
        MDNode * const md = MDNode::get(b.getContext(), C);
//        std::array<Value *, 1> V;
//        V[0] = snippetFunction;
//        OperandBundleDef op(FunctionSnippetTokenReplacerPass::FUNCTION_SNIPPET_METADATA_LABEL.str(), V);

        snippetFunction->setMetadata(FunctionSnippetTokenReplacerPass::FUNCTION_SNIPPET_METADATA_LABEL, md);

        BasicBlock * const entry = BasicBlock::Create(b.getContext(), "entry", snippetFunction);
        b.SetInsertPoint(entry);

        auto arg = snippetFunction->arg_begin();
        auto nextArg = [&]() {
            assert (arg != snippetFunction->arg_end());
            Value * const v = &*arg;
            std::advance(arg, 1);
            return v;
        };

        SmallVector<Value *, 16> funcParam(n);
        for (size_t i = 0; i < n; ++i) {
            funcParam[i] = nextArg();
        }

        Value * const retVal = functionGenerator(funcParam);
        if (LLVM_UNLIKELY(retVal->getType() != retTy)) {
            SmallVector<char, 256> tmp;
            raw_svector_ostream msg(tmp);
            msg << "Error generating function token: expected return type ";
            retTy->print(msg);
            msg << " but function generator returned ";
            retVal->getType()->print(msg);
            report_fatal_error(msg.str());
        }

        b.CreateRet(retVal);

        b.restoreIP(ip);

//        opaqueToken = GlobalAlias::create(funcTy, 0, GlobalValue::WeakAnyLinkage, nm.str(), snippetFunction, m);

//        opaqueToken = Function::Create(funcTy, Function::ExternalLinkage, nm.str(), m);
////        opaqueToken->addFnAttr(Attribute::ReadNone);
//        opaqueToken->addFnAttr(Attribute::NoUnwind);
//        opaqueToken->setCallingConv(CallingConv::Fast);
//        std::array<Metadata *, 1> C;
//        C[0] = ValueAsMetadata::get(snippetFunction);
//        MDNode * const md = MDNode::get(b.getContext(), C);
//        opaqueToken->setMetadata(FunctionSnippetTokenReplacerPass::FUNCTION_SNIPPET_METADATA_LABEL, md);



    }

    auto & C = b.getContext();

//    std::array<Metadata *, 1> M;
//    M[0] = ValueAsMetadata::get(snippetFunction);
//    MDNode * node = MDNode::get(C, M);
//    MetadataAsValue * mark = MetadataAsValue::get(C, node);
    std::array<Value *, 1> V;
    V[0] = ConstantInt::get(b.getInt64Ty(), (uintptr_t)snippetFunction);
    OperandBundleDef op("cfguardtarget", V);
    SmallVector<OperandBundleDef, 1> bundle;
    bundle.emplace_back(std::move(op));

    CallInst * const retStruct = b.CreateCall(funcTy, snippetFunction, params, bundle);
    retStruct->setCallingConv(CallingConv::Fast);

//    const auto isAgg = retTy->isAggregateType();
//    const auto n = isAgg ? retTy->getStructNumElements() : 1U;
//    SmallVector<Value *, 16> retVal(n);
//    if (retTy->isAggregateType()) {
//        for (unsigned i = 0; i < n; ++i) {

//        }
//    } else {
//        retVal[0] = retStruct;
//    }
    return retStruct;
}

FunctionSnippetPassManagerProxy::FunctionSnippetPassManagerProxy(Module & M, llvm::PassManagerBase & pm, const bool addPostOptimizations)
: BasePM(pm)
, AddPostOptimizations(addPostOptimizations)
, AlreadyInsertedFunctionSnippetPass(false) {
    // TODO: we need to scan through the function declarations in M to see if any snippets are single use ones.
    // We can add a LLVM AlwaysInline attribute to those to let LLVM deal with them completely. Only if we have a
    // multi-use snippet do we need to run the snippet inliner pass on M. This has the added benefit of the pass
    // knowing it will have some work to do on at least one function in M.
//    AlreadyInsertedFunctionSnippetPass = true;
//    for (Function & F : M) {
//        if (F.hasMetadata(FunctionSnippetTokenReplacerPass::FUNCTION_SNIPPET_METADATA_LABEL)) {
//            if (F.getNumUses() > 1) {
//                F.addFnAttr(Attribute::NoInline);
//                AlreadyInsertedFunctionSnippetPass = false;
//            } else {
//                F.addFnAttr(Attribute::AlwaysInline);
//            }
//        }
//    }

}


void FunctionSnippetPassManagerProxy::add(Pass * P) {
//    if (AlreadyInsertedFunctionSnippetPass) {
//        BasePM.add(P);
//    } else {
        if (P->getPassID() == &TargetPassConfig::ID) {
            auto * TPC = static_cast<TargetPassConfig *>(P);
            TPC->setRequiresCodeGenSCCOrder();
        }
        BasePM.add(P);
        if (P->getPassID() == &FinalizeISelID) {
            BasePM.add(createFunctionSnippetTokenReplacerPass());
            if (AddPostOptimizations) {
                const PassRegistry & PR = *PassRegistry::getPassRegistry();
                auto addPassById = [&](AnalysisID PassId) {
                    auto * PI = PR.getPassInfo(&PassId);
                    if (PI) {
                        BasePM.add(PI->createPass());
                    }
                };
                addPassById(&MachineCSEID);
                addPassById(&PeepholeOptimizerID);
                addPassById(&DeadMachineInstructionElimID);
            }
        }
        AlreadyInsertedFunctionSnippetPass = true;
//    }
}
