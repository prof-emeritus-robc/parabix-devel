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
#include <llvm/CodeGen/MachineLoopInfo.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/CodeGen/TargetRegisterInfo.h>
#include <llvm/CodeGen/MachineFrameInfo.h>
#include <llvm/Support/CommandLine.h>
#include <toolchain/toolchain.h>
#include <queue>

#define BEGIN_SCOPED_REGION {
#define END_SCOPED_REGION }

constexpr auto MAXIMUM_SYMBOL_LENGTH = 4096;


using namespace llvm;

static cl::opt<FunctionSnippetMode> FunctionSnippetModeOption("function-snippets",
    cl::desc("How pablo function snippets are compiled."),
    cl::values(clEnumValN(FunctionSnippetMode::Direct, "direct", "emit snippet code in place (no snippet functions)"),
               clEnumValN(FunctionSnippetMode::LateIR, "late-ir", "keep snippets opaque during IR optimization; inline them before codegen"),
               clEnumValN(FunctionSnippetMode::MIR, "mir", "splice snippets after instruction selection (experimental)")),
    cl::init(FunctionSnippetMode::LateIR), cl::cat(codegen::CodeGenOptions));

FunctionSnippetMode getFunctionSnippetMode() {
    return FunctionSnippetModeOption;
}

constexpr static StringRef SNIPPET_PREFIX{"__cfs."};
constexpr static StringRef SNIPPET_INPUT_SUFFIX{"_in"};
constexpr static StringRef SNIPPET_OUTPUT_SUFFIX{"_out"};
constexpr static StringRef SNIPPET_METADATA_LABEL{"__cfsmd"};

enum CFAnchorCode {
    ANCHOR_START = 0,
    ANCHOR_CALL = 1,
    ANCHOR_END = 2
};

using BasicBlockIterator = std::vector<MachineBasicBlock *>::iterator;

using InstrIterator = MachineBasicBlock::instr_iterator;

class FunctionSnippetTokenReplacerPass : public MachineFunctionPass {
public:

    static inline char ID = 0;

    FunctionSnippetTokenReplacerPass()
    : MachineFunctionPass(ID) {

    }

    bool doInitialization(Module & M) override;

    bool runOnMachineFunction(MachineFunction & MF) override;

    bool doFinalization(Module & M) override;

    void getAnalysisUsage(AnalysisUsage & AU) const override;

private:

    struct CodeFragment {
        std::vector<Register> LiveIn;
        std::vector<Register> LiveOut;
        std::vector<Register> VRegDefs;
        std::vector<MachineBasicBlock *> BasicBlocks;
        MachineBasicBlock::instr_iterator Begin;
        MachineBasicBlock::instr_iterator End;
        bool FullyResolved = false;
    };

    using CodeFragmentMap = DenseMap<const Function *, CodeFragment, DenseMapInfo<Function *>>;

    using OrderingMap = DenseMap<MachineBasicBlock *, size_t, DenseMapInfo <MachineBasicBlock *>>;

    using GlobalSet = DenseSet<const GlobalValue *, DenseMapInfo<GlobalValue *>>;

    bool checkForFencedCodeFragment(Module * M,
                                    MachineFunction & MF,
                                    BasicBlockIterator & MBBIterator,
                                    InstrIterator & instrIterator,
                                    CodeFragmentMap & codeFragments,
                                    GlobalSet & discoveredGlobals) const;

    bool subsituteCodeFragment(const Function * F,
                               MachineFunction & MF,
                               BasicBlockIterator & MBBIterator,
                               InstrIterator & instrIterator,
                               const CodeFragmentMap & codeFragments,
                               GlobalSet & discoveredGlobals) const;


    using RegisterVector = SmallVector<Register, 16>;
    using ToDeleteVector = SmallVector<MachineInstr *, 32>;

    static void getCallsiteVirtualRegisters(MachineFunction & MF, InstrIterator &callsite,
                                     RegisterVector &uses, RegisterVector & defs, GlobalSet & discoveredGlobals);

    static void eraseCallsite(MachineRegisterInfo & MRI, InstrIterator & callsite);

    template <typename ArrayA, typename ArrayB>
    static void rerouteLiveSets(const TargetRegisterInfo * const TRI, MachineRegisterInfo & MRI, const TargetInstrInfo * const TII,
                                InstrIterator ip,
                                const ArrayA & srcArray, ArrayB & dstArray);

    const Function * getCalleeFunction(Module * M, MachineFunction & MF, MachineInstr & call) const;

};

bool FunctionSnippetTokenReplacerPass::doInitialization(Module & M)  {
    return false;
}

bool FunctionSnippetTokenReplacerPass::doFinalization(Module & M)  {
    return false;
}

MachineFunctionPass * createFunctionSnippetTokenReplacerPass() {
    return new FunctionSnippetTokenReplacerPass();
}

bool inline isMemFence(MachineInstr & I) {
    if (I.getOpcode() == TargetOpcode::MEMBARRIER) return true;
    return (I.hasUnmodeledSideEffects() && I.mayLoadOrStore() && !I.isCall() && !I.isReturn());
}

inline void updateRegCopySource(MachineInstr & MI, Register newVal) {
    if (LLVM_LIKELY(MI.isCopyLike())) {
        MI.getOperand(1).setReg(newVal);
    } else {
        const auto & desc = MI.getDesc();
        const auto m = MI.getNumOperands();
        const auto n = std::min(m, desc.getNumOperands());
        for (unsigned i = 0; i < n; ++i) {
            auto & MO = MI.getOperand(i);
            if (!MO.isReg() || MO.isDef() || MO.isImplicit()) {
                continue;
            }
            const auto & info = desc.operands()[i];
            if (info.OperandType == MCOI::OPERAND_REGISTER) {
                MI.getOperand(i).setReg(newVal);
                return;
            }
        }
        if (LLVM_LIKELY(n != m)) {
            MI.getOperand(n).setReg(newVal);
        }

    }
};

inline uint64_t getAnchorCode(const MDNode * const md) {
    assert (md->getNumOperands() == 1);
    return cast<ConstantInt>(cast<ConstantAsMetadata>(md->getOperand(0).get())->getValue())->getZExtValue();
}

inline bool hasNoWork(const Module * const M, const Function * const func) {
    for (const auto & F : M->functions()) {
        if (F.hasMetadata(SNIPPET_METADATA_LABEL)) {
            for (const User * U : F.users()) {
                if (isa<Instruction>(U)) {
                    if (cast<Instruction>(U)->getFunction() == func) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool FunctionSnippetTokenReplacerPass::runOnMachineFunction(MachineFunction & MF) {

    Function & caller = MF.getFunction();
    Module * const M = caller.getParent();

    if (hasNoWork(M, &caller)) {
        return false;
    }

    ReversePostOrderTraversal<MachineFunction *> RPT(&MF);
    std::vector<MachineBasicBlock *> ordering(RPT.begin(), RPT.end());

    CodeFragmentMap CFMap;

    GlobalSet discoveredGlobals;

    for (BasicBlockIterator bbItr = ordering.begin(); bbItr != ordering.end(); ++bbItr) {

        InstrIterator begin = (*bbItr)->instr_begin();
        InstrIterator end = (*bbItr)->instr_end();

        for (auto instItr = begin; instItr != end; ) {

            assert (instItr->getParent() == *bbItr);

            MachineInstr & I = *instItr;
            if (I.isCall()) {
                const Function * const callee = getCalleeFunction(M, MF, I);
                if (callee) {
                    MDNode * md = callee->getMetadata(SNIPPET_METADATA_LABEL);
                    if (md) {
                        const auto gv = getAnchorCode(md);
                        if (gv == CFAnchorCode::ANCHOR_CALL) {
                            if (subsituteCodeFragment(callee, MF, bbItr, instItr, CFMap, discoveredGlobals)) {
//                                MF.verify(this, "After subsituteCodeFragment");
                                assert (instItr->getParent() == *bbItr);
                                end = (*bbItr)->instr_end();
                                assert (instItr != end);
                                continue;
                            }
                        }
                    }
                }
            }

            if (isMemFence(I)) {
                if (checkForFencedCodeFragment(M, MF, bbItr, instItr, CFMap, discoveredGlobals)) {
//                    MF.verify(this, "After checkForFencedCodeFragment");
                    assert (instItr->getParent() == *bbItr);
                    end = (*bbItr)->instr_end();
                    assert (instItr != end);
                    continue;
                }
            }
            ++instItr;
        }
    }

    // TODO: we can do this within the loop.
    auto & MFI = MF.getFrameInfo();
    MFI.computeMaxCallFrameSize(MF);
//    MFI.setHasOpaqueSPAdjustment(false);

    if (!discoveredGlobals.empty()) {
        auto & MRI = MF.getRegInfo();
        for (auto & MBB : MF) {
            for (auto & MI : make_early_inc_range(MBB)) {
                for (auto & MO : MI.operands()) {
                    if (MO.isGlobal() && discoveredGlobals.count(MO.getGlobal())) {
                        auto & def = MI.getOperand(0);
                        if (def.isReg()) {
                            auto reg = def.getReg();
                            if (reg.isVirtual()) {
                                if (MRI.use_empty(reg)) {
                                    MI.eraseFromParent();
                                }
                            }
                        }
                        break;
                    }
                }
            }
        }
    }
    return true;
}

void simplifyRegCopy(MachineRegisterInfo & MRI, const TargetRegisterInfo * const TRI, Register src, Register dst) {

}

void simplifyRegCopy(MachineRegisterInfo & MRI, const TargetRegisterInfo * const TRI, Register reg) {
    for (const auto & MI : MRI.use_instructions(reg)) {
        if (MI.isCopy()) {
            assert (MI.getOperand(1).getReg() == reg);



        }
    }
}


inline bool isVectorConstant(const MachineInstr & I, const TargetInstrInfo * const TII) {
    if (TII->isTriviallyReMaterializable(I) || TII->isAsCheapAsAMove(I)) {
       for (const auto & O : I.explicit_uses()) {
           if (O.isReg() && O.getReg().isValid()) {
               return false;
           }
       }
       return true;
    }
    return false;
}

inline bool isNonComputeRegisterAssign(const MachineInstr & I, const TargetInstrInfo * const TII) {
    switch (I.getOpcode()) {
        case TargetOpcode::COPY:
        case TargetOpcode::IMPLICIT_DEF:
        case TargetOpcode::SUBREG_TO_REG:
        case TargetOpcode::EXTRACT_SUBREG:
        case TargetOpcode::INSERT_SUBREG:
        case TargetOpcode::REG_SEQUENCE:
        case TargetOpcode::PHI:
        case TargetOpcode::G_CONSTANT:
        case TargetOpcode::G_FCONSTANT:
        case TargetOpcode::G_IMPLICIT_DEF:
        case TargetOpcode::G_ANYEXT:
        case TargetOpcode::G_TRUNC:
            return true;
    default:
         return I.isMoveReg() || I.isMoveImmediate() || I.isMetaInstruction() || isVectorConstant(I, TII);
    }
}

inline bool isImplicitDefinition(const MachineInstr & I, const TargetInstrInfo * const TII) {
    switch (I.getOpcode()) {
        case TargetOpcode::IMPLICIT_DEF:
        case TargetOpcode::G_CONSTANT:
        case TargetOpcode::G_FCONSTANT:
        case TargetOpcode::G_IMPLICIT_DEF:
            return true;
    default:
        return I.isMoveImmediate() || isVectorConstant(I, TII);
    }
}

inline const TargetRegisterClass * findSmallestCommonRegSuperclass(const TargetRegisterInfo * const TRI,
                                                                   const TargetRegisterClass * A, const TargetRegisterClass * B,
                                                                   unsigned & indexA, unsigned & indexB) {
    const auto n = TRI->getNumSubRegIndices();
    const TargetRegisterClass * smallestRC = nullptr;
    unsigned rw = std::numeric_limits<unsigned>::max();
    for (unsigned i = 1; i != n; ++i) {
        for (unsigned j = 1; j != n; ++j) {
            unsigned preA = 0, preB = 0;
            auto rc = TRI->getCommonSuperRegClass(A, i, B, j, preA, preB);
            if (rc) {
                const auto w = TRI->getRegSizeInBits(*rc);
                if (w < rw) {
                    smallestRC = rc;
                    rw = w;
                    indexA = i;
                    indexB = j;
                }

            }
        }
    }
    return smallestRC;
}

inline bool isBefore(const InstrIterator A, const InstrIterator B) {
    if (A == B) {
        return false;
    }
    auto * const MBBA = A->getParent();
    auto * const MBBB = B->getParent();
    if (LLVM_LIKELY(MBBA == MBBB)) {
        const auto end = MBBA->instr_end();
        for (auto a = A, b = B;;) {
            if (a == B || b == end) return true;
            if (b == A || a == end) return false;
            ++a; ++b;
        }
    } else {
        assert (false);
        return false;
    }
}

bool FunctionSnippetTokenReplacerPass::checkForFencedCodeFragment(Module * M,
                                                                  MachineFunction & MF,
                                                                  BasicBlockIterator & MBBIterator,
                                                                  InstrIterator & startIterator,
                                                                  CodeFragmentMap & codeFragments,
                                                                  GlobalSet & discoveredGlobals) const {


    auto end = (*MBBIterator)->instr_end();

    MachineRegisterInfo & MRI = MF.getRegInfo();
    auto & ST = MF.getSubtarget();
    const TargetRegisterInfo * const TRI = ST.getRegisterInfo();
    const TargetInstrInfo * const TII = ST.getInstrInfo();

    assert (isMemFence(*startIterator));

    auto nextItr = std::next(startIterator);

    const Function * F = nullptr;

    for (; nextItr != end; ++nextItr) {
        assert (nextItr->getParent() == *MBBIterator);
        MachineInstr & I = *nextItr;
        if (LLVM_UNLIKELY(I.isCall())) {
            F = getCalleeFunction(M, MF, I);
            if (F) {
                MDNode * md = F->getMetadata(SNIPPET_METADATA_LABEL);
                if (md) {
                    const auto gv = getAnchorCode(md);
                    if (LLVM_LIKELY(gv == CFAnchorCode::ANCHOR_START)) {
                        goto found_start_anchor;
                    }
                }
            }
            return false;
        } /*else if (isNonComputeRegisterAssign(I, TII)) {
            continue;
        }
        return false;*/
    }
    return false;

found_start_anchor:

    CodeFragment CF;
    CF.FullyResolved = true;
    CF.BasicBlocks.emplace_back(*MBBIterator);

    BasicBlockIterator mbbItr = MBBIterator;

    RegisterVector LiveIn;
    RegisterVector LiveOut;

    // Parse the input callsite
    getCallsiteVirtualRegisters(MF, nextItr, LiveIn, LiveOut, discoveredGlobals);
    assert (nextItr->getParent() == *mbbItr);
    rerouteLiveSets(TRI, MRI, TII, nextItr, LiveIn, LiveOut);
    assert (startIterator->getParent());
    assert (isMemFence(*startIterator));
    startIterator->eraseFromParent();
    assert (nextItr->getParent() == *mbbItr);
    assert (LiveIn.size() == LiveOut.size());

    CF.Begin = nextItr;
    CF.LiveIn.assign(LiveOut.begin(), LiveOut.end());
    LiveIn.clear();
    LiveOut.clear();

    for (; nextItr != end; ) {
        assert (nextItr->getParent() == *mbbItr);
        MachineInstr & I = *nextItr;
        if (LLVM_UNLIKELY(I.isCall())) {
            const Function * const F = getCalleeFunction(M, MF, I);
            if (F) {
                MDNode * md = F->getMetadata(SNIPPET_METADATA_LABEL);
                if (md) {
                    const auto gv = getAnchorCode(md);
                    if (LLVM_LIKELY(gv == CFAnchorCode::ANCHOR_END)) {
                        goto found_end_anchor;
                    } else { assert (gv == CFAnchorCode::ANCHOR_CALL);
                        if (subsituteCodeFragment(F, MF, mbbItr, nextItr, codeFragments, discoveredGlobals)) {
                            continue;
                        } else {
                            CF.FullyResolved = false;
                        }
                    }
                }
            }
        } else if (LLVM_UNLIKELY(isMemFence(I))) {
            if (checkForFencedCodeFragment(M, MF, mbbItr, nextItr, codeFragments, discoveredGlobals)) {
                end = (*mbbItr)->instr_end();
                continue;
            }
        }

        if (I.isBranch()) {
            ++mbbItr;
            // If we have nested fragments, they won't exist in the MBBIterator traversal.
            CF.BasicBlocks.emplace_back(*mbbItr);
            nextItr = (*mbbItr)->instr_begin();
            end = (*mbbItr)->instr_end();
        } else {
            ++nextItr;
        }
    }
    llvm::report_fatal_error("Error attempting to parse code fragment in " + MF.getName() + ": could not find end anchor.");

found_end_anchor:

    assert (nextItr->getParent() == *mbbItr);
    getCallsiteVirtualRegisters(MF, nextItr, LiveIn, LiveOut, discoveredGlobals);
    assert (nextItr->getParent() == *mbbItr);
    rerouteLiveSets(TRI, MRI, TII, nextItr, LiveIn, LiveOut);
    CF.End = std::prev(nextItr);

    BEGIN_SCOPED_REGION

    DenseSet<Register, DenseMapInfo<Register>> observedDefs;
    DenseMap<Register, Register, DenseMapInfo<Register>> internallyRemapped;

    for (auto & liveIn : CF.LiveIn) {
        internallyRemapped.insert(std::make_pair(liveIn, liveIn));
    }

    const auto & MBBs = CF.BasicBlocks;
    const auto n = MBBs.size();
    assert (n > 0);

    for (size_t i = 0; i < n; ++i) {
        InstrIterator start;
        if (LLVM_LIKELY(i == 0)) {
            start = CF.Begin;
        } else {
            start = MBBs[i]->instr_begin();
        }
        InstrIterator end;
        if (LLVM_LIKELY(i == (n - 1))) {
            end = nextItr;
        } else {
            end = MBBs[i]->instr_end();
        }

        for (auto itr = start; itr != end; ++itr) {
            MachineInstr & I = *itr;
            for (auto & op : I.operands()) {
                if (op.isReg()) {
                    const auto reg = op.getReg();
                    if (reg.isVirtual()) {
                        if (op.isDef()) {
                            observedDefs.insert(reg);
                        } else if (LLVM_UNLIKELY(observedDefs.count(reg) == 0))  {
                            auto f = internallyRemapped.find(reg);
                            if (f != internallyRemapped.end()) {
                                op.setReg(f->getSecond());
                            } else {
                                MachineInstr * const regDef = MRI.getVRegDef(reg);
                                if (LLVM_LIKELY(isVectorConstant(*regDef, TII))) {
                                    Register newConstant = MRI.createVirtualRegister(MRI.getRegClass(reg));
                                    MachineInstr * const  newConstantDef = MF.CloneMachineInstr(regDef);
                                    newConstantDef->getOperand(0).setReg(newConstant);
                                    CF.Begin = MBBs[0]->insert(CF.Begin, newConstantDef);
                                    op.setReg(newConstant);
                                    internallyRemapped.insert(std::make_pair(reg, newConstant));
                                    observedDefs.insert(newConstant);
                                } else {
                                   llvm_unreachable("Non-trivial CSE value injection?");
                                }
                            }
                        }

                    }
                }
            }
        }
    }

    CF.VRegDefs.assign(observedDefs.begin(), observedDefs.end());

    END_SCOPED_REGION

    for (; nextItr != end; ) {
        MachineInstr & I = *nextItr;
        if (isMemFence(I)) {

            I.eraseFromParent();
            assert (LiveIn.size() == LiveOut.size());
            CF.LiveOut.assign(LiveIn.begin(), LiveIn.end());
            LiveIn.clear();
            LiveOut.clear();

            // NOTE: mbbItr wasn't changed in the prior loop. it still points to the last MBB traversed of
            // the original CFG.
            startIterator = CF.End;
            MBBIterator = mbbItr;

            auto nm = F->getName();
            constexpr size_t SNIPPET_INPUT_SUFFIX_LENGTH = SNIPPET_INPUT_SUFFIX.size();
            const Function * calleeFunc = M->getFunction(nm.substr(0, nm.size() - SNIPPET_INPUT_SUFFIX_LENGTH));
            assert (calleeFunc);

        //    errs() << "Parsed code fragment " << calleeFunc->getName() << " in " << MF.getFunction().getName() << "\n";

            codeFragments.insert(std::make_pair(calleeFunc, std::move(CF)));
            return true;
        }
        ++nextItr;
    }

    llvm::report_fatal_error("Error attempting to parse code fragment in " + MF.getName() + ": could not find closing memfence.");
}

bool FunctionSnippetTokenReplacerPass::subsituteCodeFragment(const Function * F,
                                                             MachineFunction & MF,
                                                             BasicBlockIterator & MBBIterator,
                                                             InstrIterator & instrIterator,
                                                             const CodeFragmentMap & codeFragments,
                                                             GlobalSet & discoveredGlobals) const {
    const auto f = codeFragments.find(F);
    if (LLVM_UNLIKELY(f == codeFragments.end())) {
        // If this happens, we've visited a callsite before parsing the actual code fragment snippet.
        // The only thing we can do is defer resolution but because it's possible to have nested code
        // fragment subsitutions, this can impact the outer subsitution too.
        return false;
    }
    const CodeFragment & CF = f->second;
    if (LLVM_UNLIKELY(!CF.FullyResolved)) {
        // We have visited the code fragment but within the fragment is a callsite that we have either
        // not visited the fragment for or it too had an unresolved callsite. Until we've resolved all
        // the internal callsites, there is no point in subsituting this fragment for the callsite.
        return false;
    }

    MachineRegisterInfo & MRI = MF.getRegInfo();
    auto & ST = MF.getSubtarget();
    const TargetRegisterInfo * const TRI = ST.getRegisterInfo();
    const TargetInstrInfo * const TII = ST.getInstrInfo();

    DenseMap<Register, Register, DenseMapInfo<Register>> RegMap;

    DenseMap<MachineBasicBlock *, MachineBasicBlock *, DenseMapInfo<MachineBasicBlock *>> MBBMap;

    auto getRegister = [&](Register calleeReg) -> Register {
        assert (calleeReg.isVirtual());
        assert (calleeReg.isValid());
        auto f = RegMap.find(calleeReg);
        assert (f != RegMap.end());
        return f->second;
    };

    auto getBasicBlock = [&](const MachineBasicBlock * bb) -> MachineBasicBlock * {
        auto f = MBBMap.find(bb);
        assert (f != MBBMap.end());
        return f->second;
    };

    RegisterVector LiveIn;
    RegisterVector LiveOut;

    getCallsiteVirtualRegisters(MF, instrIterator, LiveIn, LiveOut, discoveredGlobals);

    const auto n = CF.LiveIn.size();

    // TODO: this isn't quite what I want. I need a different reroute function that only
    // creates a new virtual register if a copy is needed to convert classes.
    RegisterVector mappedLiveIns(n);

    for (size_t i = 0; i < n; ++i) {
        const auto c = MRI.getRegClass(CF.LiveIn[i]);
        Register newReg = MRI.createVirtualRegister(c);
        mappedLiveIns[i] = newReg;
    }

    rerouteLiveSets(TRI, MRI, TII, instrIterator, LiveIn, mappedLiveIns);

    for (size_t i = 0; i < n; ++i) {
        RegMap.insert(std::make_pair(CF.LiveIn[i], mappedLiveIns[i]));
    }

    for (auto vreg : CF.VRegDefs) {
        const auto c = MRI.getRegClass(vreg);
        Register newReg = MRI.createVirtualRegister(c);
        assert (RegMap.count(vreg) == 0);
        RegMap.insert(std::make_pair(vreg, newReg));
    }

    const auto & BB = CF.BasicBlocks;
    assert (BB.size() > 0);
    MBBMap.insert(std::make_pair(BB[0], *MBBIterator));

    auto currentInsertPoint = instrIterator;

    if (LLVM_UNLIKELY(BB.size() != 1)) {

        const auto n = BB.size();
        auto ip = std::next((*MBBIterator)->getIterator());

        for (unsigned i = 1; i < n; ++i) {
            MachineBasicBlock * newBB = MF.CreateMachineBasicBlock();
            MBBMap.insert(std::make_pair(BB[i], newBB));
            MF.insert(ip, newBB);
        }

        auto & MBPI = getAnalysis<MachineBranchProbabilityInfo>();

        for (unsigned i = 0; i < (n - 1); ++i) {
            MachineBasicBlock * cur = getBasicBlock(BB[i]);
            for (const auto & s : BB[i]->successors()) {
                MachineBasicBlock * const newSucc = getBasicBlock(s);
                auto prob = MBPI.getEdgeProbability(BB[i], s);
                cur->addSuccessor(newSucc, prob);
            }
        }
    }

    assert (CF.Begin->getParent());
    auto nextItr = CF.Begin->getIterator();
    auto currentMBB = MBBIterator;

    for (;;) {
        const MachineInstr & I = *nextItr;
        MachineInstr * cloned = MF.CloneMachineInstr(&I);
        const auto m = I.getNumOperands();
        for (size_t i = 0; i < m; ++i) {
            const auto & MO = I.getOperand(i);
            auto & MC = cloned->getOperand(i);
            if (MO.isReg()) {
                const auto reg = MO.getReg();
                if (reg.isVirtual()) {
                    MC.setReg(getRegister(reg));
                }
            } else if (MO.isMBB()) {
                MC.setMBB(getBasicBlock(MO.getMBB()));
            }
        }

        (*currentMBB)->insert(currentInsertPoint, cloned);

        if (LLVM_UNLIKELY(cloned->isBranch())) {
            auto CBB = *(++currentMBB);
            nextItr = CBB->instr_begin();
            currentInsertPoint = getBasicBlock(CBB)->instr_end();
        }
        if (nextItr == CF.End) {
            break;
        }
        ++nextItr;
    }


    const auto m = CF.LiveOut.size();

    RegisterVector mappedLiveOuts(m);
    for (size_t i = 0; i < m; ++i) {
        mappedLiveOuts[i] = getRegister(CF.LiveOut[i]);
    }

    rerouteLiveSets(TRI, MRI, TII, currentInsertPoint, mappedLiveOuts, LiveOut);

    if (LLVM_UNLIKELY(BB.size() != 1)) {
        (*MBBIterator)->splice(currentInsertPoint, (*currentMBB), instrIterator);
    }



    MBBIterator = currentMBB;
    instrIterator = currentInsertPoint;


    return true;
}

void FunctionSnippetTokenReplacerPass::getCallsiteVirtualRegisters(MachineFunction & MF,
                                                                   InstrIterator & callsite,
                                                                   RegisterVector & uses,
                                                                   RegisterVector & defs,
                                                                   GlobalSet & discoveredGlobals) {

    MachineRegisterInfo & MRI = MF.getRegInfo();
    auto & ST = MF.getSubtarget();
    const TargetRegisterInfo * const TRI = ST.getRegisterInfo();
    const TargetInstrInfo * const TII = ST.getInstrInfo();

    // adjcallstackdown
    // copy virtual to physical
    // callsite
    // adjcallstackup
    // copy physical to virtual

    assert (uses.empty());
    assert (defs.empty());

    Register addrVReg;

//    const uint32_t * clobberMask = nullptr;

    for (auto & op : callsite->operands()) {
        if (op.isGlobal()) {
            discoveredGlobals.insert(op.getGlobal());
        } else if (op.isReg()) {
            auto reg = op.getReg();
            assert (reg.isValid());
            if (op.isImplicit()) {
                assert (reg.isPhysical());
                if (MRI.isAllocatable(reg.asMCReg())) {
                    if (op.isDef()) {
                        defs.emplace_back(reg);
                    } else {
                        uses.emplace_back(reg);
                    }
                }
            } else if (op.isUse() && reg.isVirtual()) {
                addrVReg = reg;
            }
//        } else if (op.isRegMask()) {
//            clobberMask = op.getRegMask();
        }
    }

    assert (defs.size() <= uses.size());

    DenseMap<MCPhysReg, Register, DenseMapInfo<MCPhysReg>> PhyRegMap;

    using StackArgTy = std::pair<int64_t, Register>;

    SmallVector<StackArgTy, 16> StackArgs;

    const auto StackDown = TII->getCallFrameSetupOpcode();

    auto remainingUses = uses.size() + 1;

    // Get all input arguments
    for (auto itr = std::next(callsite->getReverseIterator());;) {
        assert (itr != itr->getParent()->instr_rend());

        auto & I = *itr++;

        if (I.isCopy()) {

            const MachineOperand & dst = I.getOperand(0);
            auto dstReg = dst.getReg();
            if (dstReg.isPhysical()) {
                auto srcReg = I.getOperand(1).getReg();
                I.eraseFromParent();
                PhyRegMap.insert(std::make_pair(dstReg.asMCReg(), srcReg));
                if (LLVM_LIKELY(srcReg.isVirtual())) {
                    if (--remainingUses == 0) {
                        break;
                    }
                }
            }
        } else if (I.mayStore() && I.hasOneMemOperand()) {
            int64_t offset;
            const MachineOperand * destAddrOp = nullptr;
            bool scalable;
            if (TII->getMemOperandWithOffset(I, destAddrOp, offset, scalable, TRI)) {
                for (auto & MO : I.operands()) {
                    if (MO.isReg() && MO.isUse()) {
                        assert (&MO != destAddrOp);
                        auto reg = MO.getReg();
                        if (LLVM_LIKELY(reg.isVirtual())) {
                            I.eraseFromParent();
                            StackArgs.emplace_back(offset, reg);
                            if (--remainingUses == 0) {
                                goto done_input_scan;
                            }
                            break;
                        }
                    }
                }
            }
        } else if (I.getOpcode() == StackDown) {
            I.eraseFromParent();
            if (--remainingUses == 0) {
                break;
            }
        }
    }
done_input_scan:
    auto resolveRegisters = [&](auto & V) {
        assert (V.size() > 0);
        const auto n = StackArgs.size();
        assert (n <= V.size());
        const auto m = V.size() - n;
        for (size_t i = 0; i < m; ++i) {
            auto & Vi = V[i];
            for (;;) {
                if (Vi.isVirtual()) {
                    break;
                }
                auto f = PhyRegMap.find(Vi.asMCReg());
                if (LLVM_LIKELY(f != PhyRegMap.end())) {
                    Vi = f->second;
                } else {
                    for (const auto & pair : PhyRegMap) {
                        if (TRI->regsOverlap(Vi, pair.first)) {
                            Vi = pair.second;
                            break;
                        }
                    }
                }
            }
            assert (Vi.isVirtual());
            assert (&V[i] == &Vi);
        }
        if (LLVM_UNLIKELY(StackArgs.size() > 0)) {
            std::sort(StackArgs.begin(), StackArgs.end(), [](const StackArgTy & a, const StackArgTy & b){
                return a.first < b.first;
            });
            for (size_t i = 0; i < n; ++i) {
                assert (StackArgs[i].second.isVirtual());
                V[m + i] = StackArgs[i].second;
            }
            StackArgs.clear();
        }
    };

    resolveRegisters(uses);

    PhyRegMap.clear();

    const auto StackUp = TII->getCallFrameDestroyOpcode();

    auto remainingDefs = defs.size() + 1;

    InstrIterator nextItr = std::next(callsite);
    for (;;) {
        assert (nextItr != callsite->getParent()->end());
        auto & I = *nextItr++;

        if (I.isCopy()) {
            const MachineOperand & src = I.getOperand(1);
            auto srcReg = src.getReg();
            if (srcReg.isPhysical()) {
                auto defReg = I.getOperand(0).getReg();
                I.eraseFromParent();
                PhyRegMap.insert(std::make_pair(srcReg.asMCReg(), defReg));
                if (LLVM_LIKELY(defReg.isVirtual())) {
                    if (--remainingDefs == 0) {
                        break;
                    }
                }
            }
        } else if (I.mayLoad() && I.hasOneMemOperand()) {
            int64_t offset;
            const MachineOperand * destAddrOp = nullptr;
            bool scalable;
            if (TII->getMemOperandWithOffset(I, destAddrOp, offset, scalable, TRI)) {
                for (auto & MO : I.operands()) {
                    if (MO.isReg() && MO.isDef()) {
                        assert (&MO != destAddrOp);
                        auto reg = MO.getReg();
                        if (LLVM_LIKELY(reg.isVirtual())) {
                            I.eraseFromParent();
                            StackArgs.emplace_back(offset, reg);
                            if (--remainingDefs == 0) {
                                goto done_retval_scan;
                            }
                            break;
                        }
                    }
                }
            }
        } else if (I.getOpcode() == StackUp) {
            I.eraseFromParent();
            if (--remainingDefs == 0) {
                break;
            }
        }
    }
done_retval_scan:
    resolveRegisters(defs);


    assert (callsite->getParent());
    assert (nextItr->getParent() == callsite->getParent());
//    if (clobberMask) {
//        auto & MBB = *callsite->getParent();
//        BuildMI(MBB, callsite, DebugLoc{}, TII->get(TargetOpcode::KILL)).addRegMask(clobberMask);
//    }
    callsite->eraseFromParent();
    if (addrVReg.isValid() && MRI.use_empty(addrVReg)) {
        MachineInstr * const addrDef = MRI.getUniqueVRegDef(addrVReg); assert (addrDef);
        addrDef->eraseFromParent();
    }
    assert (nextItr->getParent());
    callsite = nextItr;

}

void replaceWithOrCopyReg(const TargetRegisterInfo * const TRI, MachineRegisterInfo & MRI, const TargetInstrInfo * const TII,
                          InstrIterator ip,
                          const Register & src, Register & dst) {


    auto srcRC = MRI.getRegClass(src); assert (srcRC);
    auto dstRC = MRI.getRegClass(dst); assert (dstRC);

    assert (MRI.getVRegDef(dst) == nullptr);
    assert (MRI.getVRegDef(src));
    auto & MBB = *ip->getParent();
    if (srcRC == dstRC) {
        MRI.replaceRegWith(dst, src);
        dst = src;
    } else {
        const TargetRegisterClass * const subRC = TRI->getCommonSubClass(srcRC, dstRC);
        if (srcRC == subRC || dstRC == subRC) {
            BuildMI(MBB, ip, DebugLoc{}, TII->get(TargetOpcode::COPY), dst).addReg(src);
        } else {
            unsigned srcIndex = 0;
            unsigned dstIndex = 0;
            const TargetRegisterClass * const superRC = findSmallestCommonRegSuperclass(TRI, srcRC, dstRC, srcIndex, dstIndex);
            assert ("Cannot occur if there was a valid physical register intermediary. Check this carefully!" && superRC);
            if (superRC == dstRC) {
                // widen
                BuildMI(MBB, ip, DebugLoc{}, TII->get(TargetOpcode::COPY)).addReg(dst, RegState::Define, srcIndex).addReg(src);
            } else if (superRC == srcRC) {
                // narrow
                BuildMI(MBB, ip, DebugLoc{}, TII->get(TargetOpcode::COPY), dst).addReg(src, 0, dstIndex);
            } else {
                auto bridge = MRI.createVirtualRegister(superRC);
                BuildMI(MBB, ip, DebugLoc{}, TII->get(TargetOpcode::COPY)).addReg(bridge, RegState::Define, srcIndex).addReg(src);
                BuildMI(MBB, ip, DebugLoc{}, TII->get(TargetOpcode::COPY), dst).addReg(bridge, 0, dstIndex);
            }
        }
    }

}

template <typename SrcArrayType, typename DstArrayType>
inline void FunctionSnippetTokenReplacerPass::rerouteLiveSets(const TargetRegisterInfo * const TRI, MachineRegisterInfo & MRI, const TargetInstrInfo * const TII,
                                                              InstrIterator ip,
                                                              const SrcArrayType & srcArray, DstArrayType & dstArray) {

    assert (srcArray.size() == dstArray.size());
    const auto n = srcArray.size();
    for (size_t i = 0; i < n; ++i) {
        replaceWithOrCopyReg(TRI, MRI, TII, ip, srcArray[i], dstArray[i]);
    }
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

void FunctionSnippetTokenReplacerPass::getAnalysisUsage(AnalysisUsage & AU) const {
    AU.addRequired<MachineModuleInfoWrapperPass>();
    AU.addRequired<MachineBranchProbabilityInfo>();
    MachineFunctionPass::getAnalysisUsage(AU);
}

Value * CallFunctionByToken(IDISA::IDISA_Builder & b, Type * retTy, StringRef name, ArrayRef<Value *> params,
                            std::function<Value *(ArrayRef<Value *>)> functionGenerator) {



    const auto mode = getFunctionSnippetMode();
    if (mode == FunctionSnippetMode::Direct) {
        return functionGenerator(params);
    }

    Module * const Mod = b.getModule();

    SmallVector<char, 256> tmp;
    raw_svector_ostream nm(tmp);
    nm << SNIPPET_PREFIX << name;

    // TODO: CallFunctionByToken should be placed into IDISA_Builder itself.

    // TODO: it isn't enough to check if its in the module. We need a user in the current function that Builder b is pointed to.

    CallInst * calleeRetVal = nullptr;

    Function * snippetFunction = Mod->getFunction(nm.str());
    if (snippetFunction == nullptr) {

        const auto n = params.size();
        std::vector<Type *> paramTys(n);
        for (size_t i = 0; i < n; ++i) {
            paramTys[i] = params[i]->getType();
        }

        FunctionType * const funcTy = FunctionType::get(retTy, paramTys, false);

        if (mode == FunctionSnippetMode::LateIR) {

            snippetFunction = Function::Create(funcTy, Function::InternalLinkage, nm.str(), Mod);
            snippetFunction->addFnAttr(llvm::Attribute::AlwaysInline);

            auto ip = b.saveIP();

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

            b.CreateRet(functionGenerator(funcParam));

            b.restoreIP(ip);

            return b.CreateCall(funcTy, snippetFunction, params);

        } else if (mode == FunctionSnippetMode::MIR) {

            const auto n = params.size();
            size_t m = 1;
            if (LLVM_LIKELY(retTy->isStructTy())) {
                m = retTy->getStructNumElements();
            } else if (LLVM_UNLIKELY(retTy->isVoidTy())){
                m = 0;
            }

            SmallVector<Type *, 32> inputParamTys(n);
            for (size_t i = 0; i < n; ++i) {
                inputParamTys[i] = params[i]->getType();
            }

            std::array<Metadata *, 1> C;

            auto applyAttrs = [](Function * func) {
//                func->setCallingConv(llvm::CallingConv::PreserveAll);
                func->addFnAttr(llvm::Attribute::Naked);
                func->setDoesNotAccessMemory();
                func->setDoesNotThrow();
                func->setWillReturn();
                func->setVisibility(GlobalValue::HiddenVisibility);
                func->setDSOLocal(true);
            };


            FunctionType * const funcTy = FunctionType::get(retTy, inputParamTys, false);
            snippetFunction = Function::Create(funcTy, Function::ExternalWeakLinkage, nm.str(), Mod);
            applyAttrs(snippetFunction);

            C[0] = ConstantAsMetadata::get(b.getInt64(CFAnchorCode::ANCHOR_CALL));
            snippetFunction->setMetadata(SNIPPET_METADATA_LABEL, MDNode::get(b.getContext(), C));

            StructType * const inputRetTy = StructType::get(b.getContext(), inputParamTys);
            FunctionType * const inputFuncTy = FunctionType::get(inputRetTy, inputParamTys, false);
            Function *  const inputFunc = Function::Create(inputFuncTy, Function::ExternalWeakLinkage, nm.str() + SNIPPET_INPUT_SUFFIX, Mod);
            applyAttrs(inputFunc);

            C[0] = ConstantAsMetadata::get(b.getInt64(CFAnchorCode::ANCHOR_START));
            inputFunc->setMetadata(SNIPPET_METADATA_LABEL, MDNode::get(b.getContext(), C));


            SmallVector<Type *, 32> retParamTys(m);

            if (LLVM_LIKELY(retTy->isStructTy())) {
                for (size_t i = 0; i < m; ++i) {
                    Type * ty = retTy->getStructElementType(i);
                    assert (ty->isSingleValueType());
                    retParamTys[i] = ty;
                }
            } else if (LLVM_LIKELY(!retTy->isVoidTy())) {
                retParamTys[0] = retTy;
            }

            FunctionType * const outputFuncTy = FunctionType::get(retTy, retParamTys, false);
            Function *  const outputFunc = Function::Create(outputFuncTy, Function::ExternalWeakLinkage, nm.str() + SNIPPET_OUTPUT_SUFFIX, Mod);
            applyAttrs(outputFunc);

            C[0] = ConstantAsMetadata::get(b.getInt64(CFAnchorCode::ANCHOR_END));
            outputFunc->setMetadata(SNIPPET_METADATA_LABEL, MDNode::get(b.getContext(), C));


            b.CreateFence(AtomicOrdering::SequentiallyConsistent);

            // The fences acts as both a barrier and a marker for the pass to indicate where the code fragment exists
            // in the caller. We create and call a stub function so that we know whether the inline code exists somewhere
            // in the current function and also to determine what registers are the "livein" and "liveout" values of the
            // inlined code fragment. This requires a bit of a trick since only one set of output values can be read by
            // the remaining code. We pass the inlined return values into the stub function as input arguments and use
            // the return value of the stub as the outputs the code fragment so that we can establish a one-to-one
            // mapping of the registers for subsequent callsite replacements. These subsequent ones only have a single
            // stub callsite.

            // To prevent CSE from hoisting the computation out of the code fragment, we use a
            // fake input anchor. Normally this is not a problem except when the value is a phi
            // node or other hard to clone computation.

            CallInst * inputRetVal = b.CreateCall(inputFuncTy, inputFunc, params);
            inputRetVal->setCallingConv(llvm::CallingConv::PreserveAll);

            SmallVector<Value *, 16> inputParams(n);
            for (unsigned i = 0; i < n; ++i) {
                inputParams[i] = b.CreateExtractValue(inputRetVal, { i });
            }

            Value * const inlinedRetVal = functionGenerator(inputParams);
            if (LLVM_UNLIKELY(inlinedRetVal->getType() != retTy)) {
                SmallVector<char, 256> tmp;
                raw_svector_ostream msg(tmp);
                msg << "Error generating function token: expected return type ";
                retTy->print(msg);
                msg << " but function generator returned ";
                inlinedRetVal->getType()->print(msg);
                report_fatal_error(msg.str());
            }

            SmallVector<Value *, 16> outputParams(m);
            if (LLVM_LIKELY(retTy->isStructTy())) {
                for (unsigned j = 0; j < m; ++j) {
                    outputParams[j] = b.CreateExtractValue(inlinedRetVal, { j });
                }
            } else if (LLVM_LIKELY(!retTy->isVoidTy())){
                outputParams[0] = inlinedRetVal;
            }

            CallInst * calleeRetVal = b.CreateCall(outputFuncTy, outputFunc, outputParams);
//            calleeRetVal->setCallingConv(llvm::CallingConv::PreserveAll);
            b.CreateFence(AtomicOrdering::SequentiallyConsistent);

            return calleeRetVal;
        }

    } else {
        assert (&snippetFunction->getContext() == &b.getContext());
        FunctionType * const funcTy = snippetFunction->getFunctionType();
        assert (&funcTy->getContext() == &b.getContext());
        #ifndef NDEBUG
        const auto n = funcTy->getNumParams();
        assert (params.size() == n);
        for (size_t i = 0; i < n; ++i) {
            assert (&params[i]->getContext() == &b.getContext());
            assert (funcTy->getParamType(i) == params[i]->getType());
        }
        #endif
        return b.CreateCall(funcTy, snippetFunction, params);
    }

    return nullptr;
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


