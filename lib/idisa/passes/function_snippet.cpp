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
#include <llvm/IR/ValueSymbolTable.h>
#include <llvm/CodeGen/Passes.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/CodeGen/TargetLowering.h>
#include <llvm/CodeGen/CallingConvLower.h>
#include <llvm/CodeGen/Analysis.h>
#include <llvm/Demangle/Demangle.h>

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

    struct CacheOperand {
        MachineOperand MO;
        unsigned Flags = 0;
        const TargetRegisterClass * RegClass = nullptr;

        CacheOperand(const MachineOperand & src)
        : MO(src) {
            MO.clearParent();
        }
    };

    struct CacheInst {
        unsigned Opcode;
        uint64_t Flags;
        DebugLoc DL;
        SmallVector<CacheOperand, 2> Operands;

        CacheInst() = default;

        CacheInst(const MachineInstr & MI)
        : Opcode(MI.getOpcode())
        , Flags(MI.getFlags())
        , DL(MI.getDebugLoc()) {

        }
    };

    using RegMappingList = std::vector<std::pair<MCRegister, Register>>;

    struct CacheBasicBlock {
        BasicBlock * Source = nullptr;
        std::vector<CacheInst> Instructions;
        SmallVector<std::pair<size_t, BranchProbability>, 2> Successors;
        RegMappingList LiveOuts;

        CacheBasicBlock(BasicBlock * bb = nullptr) : Source(bb) {}
    };

    struct CachedMachineFunction {
        std::vector<CacheBasicBlock> BasicBlock;
        RegMappingList LiveIns;
        DenseMap<Register, const TargetRegisterClass *, DenseMapInfo<Register>> RegClassMap;
        std::vector<MCRegister> LiveOuts;
        size_t NumOfExitBlocks = 0;
    };

    void serializeToCache(Function & F, MachineFunction & MF, MachineModuleInfo & MMI);

    const Function * getCalleeFunction(Module * M, MachineFunction & MF, MachineInstr & call) const;

private:
    DenseMap<Function *, CachedMachineFunction, DenseMapInfo<Function *>> Cache;
    std::unique_ptr<MachineFunction> CacheContainer;
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

bool FunctionSnippetTokenReplacerPass::runOnMachineFunction(MachineFunction & MF) {

    using Prop = MachineFunctionProperties::Property;

    using RegisterMap = DenseMap<Register, Register, DenseMapInfo<Register>>;

    const auto & props = MF.getProperties();

    const auto usedGlobalISel = props.hasProperty(Prop::Legalized) || props.hasProperty(Prop::RegBankSelected);

    auto & MMI = getAnalysis<MachineModuleInfoWrapperPass>().getMMI();
    auto & MBPI = getAnalysis<MachineBranchProbabilityInfo>();
    auto & MRI = MF.getRegInfo();

    const TargetInstrInfo * const TII = MF.getSubtarget().getInstrInfo();

    RegisterMap CallerRetMap;

    RegisterMap GlobalRegMap;

    SmallVector<MachineBasicBlock *, 0> BBMap;

    errs() << "Running FunctionSnippetTokenReplacerPass on " << MF.getName() << "\n";

    auto doSplice = [&](const Function * const callee,
            MachineBasicBlock & MBB, MachineBasicBlock::iterator callsite,
            const CachedMachineFunction & cachedMF) {

        assert (callsite->isCall());

        assert (GlobalRegMap.empty());

        auto getRegister = [&](RegisterMap & M, Register calleeReg) -> Register {
            if (calleeReg.isPhysical() || !calleeReg.isValid()) {
                return calleeReg;
            }
            auto f = M.find(calleeReg);
            assert (f != M.end());
            return f->second;
        };


        // Add the callee register -> caller output mappings

        assert (CallerRetMap.empty());

        const auto & liveOuts = cachedMF.LiveOuts;
        if (liveOuts.size() > 0) {
            auto itr = callsite;
            const auto end = MBB.end();

            for (++itr; itr != end; ++itr) {
                auto & inst = *itr;
                if (inst.isCopy()) {
                    Register srcReg = inst.getOperand(1).getReg();
                    if (srcReg.isPhysical()) {
                        auto phyReg = srcReg.asMCReg();
                        if (llvm::is_contained(liveOuts, phyReg)) {
                            Register dstReg = inst.getOperand(0).getReg();
                            if (dstReg.isVirtual()) {
                                CallerRetMap.insert(std::make_pair(phyReg, dstReg));
                            }
                        }

                    }
                }
            }
        }

        // Add the caller register -> callee input mappings

        for (const auto & entry : cachedMF.RegClassMap) {
            auto reg = MRI.createVirtualRegister(entry.second);
            GlobalRegMap.insert(std::make_pair(entry.first, reg));
        }

        const auto expectedArgCount = callee->arg_size();

        if (usedGlobalISel) {

            assert (callsite->getNumOperands() > expectedArgCount);

            const auto & L = cachedMF.LiveIns;
            for (unsigned i = 0; i < expectedArgCount; ++i) {
                const auto & mo = callsite->getOperand(i + 1);
                if (LLVM_LIKELY(mo.isReg() && mo.isUse())) {
                    auto callerVReg = mo.getReg();
                    auto calleeVReg = L[i].second;
                    GlobalRegMap.insert(std::make_pair(callerVReg, calleeVReg));
                }
            }

        } else if (expectedArgCount > 0) { // using SelectionDAG

            auto itr = callsite.getReverse();
            auto end = MBB.rend();

            size_t argCount = 0;

            const auto CallFrameSetup = TII->getCallFrameSetupOpcode();

            for (++itr; itr != end; ++itr) {

                const auto & MI = *itr;
                if (MI.isCall() || MI.getOpcode() == CallFrameSetup) {
                    break;
                }
                if (MI.isCopy()) {

                    assert (MI.getNumOperands() > 1);
                    const auto & A = MI.getOperand(0);
                    const auto & B = MI.getOperand(1);
                    if (A.isReg() && B.isReg()) {
                        const auto & regA = A.getReg();
                        const auto & regB = B.getReg();
                        if (regA.isPhysical() && regB.isVirtual()) {
                            if (GlobalRegMap.count(regA) == 0) {
                                GlobalRegMap.insert(std::make_pair(regA, regB));
                                if (++argCount == expectedArgCount) {
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }


        const auto & CBBs = cachedMF.BasicBlock;

        errs() << "snippet.size()=" << CBBs.size() << "\n";

        auto & HRI = MF.getRegInfo();

        if (LLVM_LIKELY(CBBs.size() == 1)) {

            const auto & CBB = CBBs[0];

            assert (cachedMF.NumOfExitBlocks == 1);

            for (const CacheInst & CI : CBB.Instructions) {

                if (LLVM_UNLIKELY(CI.Opcode == TargetOpcode::PATCHABLE_RET)) {
                    for (auto & op : MI.operands()) {
                        if (op.isReg() && op.isUse()) {
                            const auto r = op.getReg();
                            HRI.replaceRegWith(r, getRegister(r));
                        }
                    }
                } else {

                    auto MIB = BuildMI(MBB, callsiteItr, CI.DL, TII->get(CI.Opcode));
                    MIB.setMIFlags(CI.Flags);

                    for (auto & op : CI.Operands) {
                        const auto & MO = op.MO;
                        if (MO.isReg()) {
                            const auto r = getRegister(MO.getReg());
                            MIB.addReg(r, op.Flags, MO.getSubReg());
                        } else {
                            MIB.add(MO);
                        }
                    }

                    MBB.insert(callsite, MIB);
                }
            }

        } else { // snippet function has multiple basicblocks.

            MachineBasicBlock * const entryBlock = &MBB;

            const auto n = CBBs.size();

            BBMap.resize(n);
            BBMap[0] = entryBlock;

            MachineBasicBlock * exitBlock = nullptr;
            if (LLVM_UNLIKELY(cachedMF.NumOfExitBlocks > 1)) {

            }


            MachineBasicBlock * const exitBlock = MF.CreateMachineBasicBlock();
            exitBlock->transferSuccessorsAndUpdatePHIs(entryBlock);
            auto exitBlockItr = exitBlock->getIterator();

            for (unsigned i = 1; i < n; ++i) {
                auto newBB = MF.CreateMachineBasicBlock(CBBs[i].Source);
                BBMap[i] = entryBlock;
                MF.insert(exitBlockItr, newBB);
            }

            for (unsigned i = 0; i < n; ++i) {

                const auto & CBB = CBBs[i];

                MachineBasicBlock * const targetBB = BBMap[i];
                for (auto succ : CBB.Successors) {
                    targetBB->addSuccessor(BBMap[succ.first], succ.second);
                }

                RegisterMap LocalMap = GlobalRegMap;

                for (auto & ret : CBB.LiveOuts) {




                }

                for (auto & MI : *snippetBB) {
                    if (LLVM_UNLIKELY(MI.isReturn())) {

                        for (auto & op : MI.operands()) {
                            if (op.isReg() && op.isUse()) {
                                const auto r = op.getReg();
                                auto f = RegMap.find(r);
                                assert (f != RegMap.end());
                                HRI.replaceRegWith(r, f->second);
                            }
                        }

                        targetBB->addSuccessor(exitBlock);
                        const auto CallFrameSetup = TII->getCallFrameSetupOpcode();
                        BuildMI(targetBB, callsite.getDebugLoc(), TII->get(CallFrameSetup)).addMBB(exitBlock);
                        exitBlock->transferSuccessorsAndUpdatePHIs(entryBlock);

                    } else {


                        MachineInstr * const cloned = MF.CloneMachineInstr(&MI);
                        cloned->setDebugLoc(MI.getDebugLoc());
                        for (auto & op : cloned->operands()) {
                            if (op.isReg()) {
                                op.setReg(getRegister(op.getReg()));
                            } else if (op.isMBB()) {
                                const auto bbf = BBMap.find(op.getMBB());
                                if (LLVM_LIKELY(bbf != BBMap.end())) {
                                    op.setMBB(bbf->second);
                                }
                            }
                        }
                        targetBB->push_back(cloned);
                    }
                }
            }

            BBMap.clear();
        }

        GlobalRegMap.clear();

    };


    bool Changed = false;

    Function & F = MF.getFunction();

    if (F.getMetadata(FUNCTION_SNIPPET_METADATA_LABEL)) {

        errs() << " -- adding snippet from " << MF.getName() << "\n";

        assert (F.getCallingConv() == CallingConv::Fast);

        serializeToCache(F, MF, MMI);

        return false;

    } else if (Cache.size() > 0) {

        errs() << " -- running snippet replacement on " << MF.getName() << "\n";

        Module * const M = F.getParent();

        for (auto & MBB : MF) {
            auto itr = MBB.begin();
            while (itr != MBB.end()) {
                MachineInstr & I = *itr++;
                if (I.isCall()) {
                    const Function * const callee = getCalleeFunction(M, MF, I);
                    if (callee == nullptr) continue;
                    const auto f = Cache.find(callee);
                    assert (f != Cache.end());
                    doSplice(callee, MBB, itr, f->second);
                    I.eraseFromParent();
                    Changed = true;
                }
            }
        }

    }

    return Changed;
}

inline const Function * FunctionSnippetTokenReplacerPass::getCalleeFunction(Module * M, MachineFunction & MF, MachineInstr & call) const {
    assert (call.isCall());
    for (auto & callee : call.operands()) {
        if (callee.isReg()) {
            auto reg = callee.getReg();
            if (reg.isVirtual()) {
                auto & MRI = MF.getRegInfo();
                auto defMI = MRI.getVRegDef(reg);
                if (defMI) {
                    for (auto & callee : defMI->operands()) {
                        if (callee.isGlobal()) {
                            const Function * f = dyn_cast_or_null<Function>(callee.getGlobal());
                            if (f && f->hasMetadata(FunctionSnippetTokenReplacerPass::FUNCTION_SNIPPET_METADATA_LABEL)) {
                                return f;
                            }
                        }
                        if (callee.isCPI()) {
                            const MachineConstantPool * P = MF.getConstantPool();
                            const auto & Constants = P->getConstants();
                            const auto idx = callee.getCFIIndex();
                            if (idx < Constants.size()) {
                                auto & C = Constants[idx];
                                if (C.isMachineConstantPoolEntry()) {
                                    continue;
                                }
                                auto f = dyn_cast_or_null<Function>(C.Val.ConstVal);
                                if (f && f->hasMetadata(FunctionSnippetTokenReplacerPass::FUNCTION_SNIPPET_METADATA_LABEL)) {
                                    return f;
                                }
                            }
                        }

                    }

                }
            }
        }
        if (callee.isGlobal()) {
            const Function * const f = dyn_cast<Function>(callee.getGlobal());
            if (f && f->hasMetadata(FunctionSnippetTokenReplacerPass::FUNCTION_SNIPPET_METADATA_LABEL)) {
                return f;
            }
        }
        if (callee.isSymbol()) {
            auto symbolName = callee.getSymbolName();
            if (LLVM_UNLIKELY(symbolName == nullptr)) {

            }
            const auto prefix = MF.getDataLayout().getGlobalPrefix();
            if (prefix && *symbolName == prefix)  {
                ++symbolName;
            }
            // Any SymbolName over 4K is almost-certainly malicious
            StringRef nm{symbolName, strnlen(symbolName, MAXIMUM_SYMBOL_LENGTH)};
            const Function * const f = M->getFunction(nm);
            if (f && f->hasMetadata(FunctionSnippetTokenReplacerPass::FUNCTION_SNIPPET_METADATA_LABEL)) {
                return f;
            }
        }
    }
    return nullptr;
}

inline void FunctionSnippetTokenReplacerPass::serializeToCache(Function &F, MachineFunction &MF, MachineModuleInfo & MMI) {

    asser (F.getCallingConv() == CallingConv::Fast);

    const MachineRegisterInfo & MRI = MF.getRegInfo();
    const TargetRegisterInfo * TRI = MF.getSubtarget().getRegisterInfo();
    auto & MBPI = getAnalysis<MachineBranchProbabilityInfo>();

    CachedMachineFunction cache;

    for (const auto & LI : MRI.liveins()) {
        cache.LiveIns.emplace_back(LI.first, LI.second);
    }

    DenseMap<MachineBasicBlock *, size_t, DenseMapInfo<MachineBasicBlock *>> BBIndex;

    DenseSet<MCRegister, DenseMapInfo<MCRegister>> ReturnRegSet;

    BBIndex.reserve(MF.size());

    cache.BasicBlock.reserve(MF.size());

    for (auto & MBB : MF) {
        const auto idx = cache.BasicBlock.size();
        BBIndex.insert(std::make_pair(&MBB, idx));
        cache.BasicBlock.emplace_back(MBB.getBasicBlock());
    }

    SmallVector<CCValAssign, 4> RVLocs;
    SmallVector<ISD::OutputArg, 4> OutArg;
    BEGIN_SCOPED_REGION
    Type * const retTy = F.getReturnType();
    if (LLVM_LIKELY(!retTy->isVoidTy())) {
        const TargetLowering * TLI = MF.getSubtarget().getTargetLowering();
        const DataLayout & DL = MF.getDataLayout();
        SmallVector<EVT, 4> RetVTs;
        ComputeValueVTs(*TLI, DL, retTy, RetVTs);
        const auto n = RetVTs.size();
        OutArg.resize(n);
        for (unsigned i = 0; i < n; ++i) {
            auto argVT = RetVTs[i].getSimpleVT();
            ISD::ArgFlagsTy flags;
            OutArg[i] = ISD::OutputArg(flags, argVT, RetVTs[i], true, i, 0);
        }
        LLVMContext & C = F.getContext();
        CCState CCInfo(CallingConv::Fast, F.isVarArg(), MF, RVLocs, C);
        const auto canLower = TLI->CanLowerReturn(CallingConv::Fast, MF, F.isVarArg(), OutArg, C);


        if (LLVM_UNLIKELY(!canLower)) {
            SmallVector<char, 256> tmp;
            raw_svector_ostream msg(tmp);
            msg << "MachineFunction " << F.getName() << " cannot lower return type ";
            retTy->print(msg);
            msg << " to register values?";
            report_fatal_error(msg.str());
        }

        assert (ReturnRegSet.empty());

        for (const auto & VA : RVLocs) {
            if (LLVM_LIKELY(VA.isRegLoc())) {
                ReturnRegSet.insert(VA.getLocReg());
            }
        }
        cache.LiveOuts.assign(ReturnRegSet.begin(), ReturnRegSet.end());
    }

    END_SCOPED_REGION

    size_t index = 0;

    for (auto & MBB : MF) {

        auto & CBB = cache.BasicBlock[index++];

        BEGIN_SCOPED_REGION
        CBB.Successors.reserve(MBB.succ_size());
        auto succ = MBB.succ_begin();
        auto succ_end = MBB.succ_end();
        for (; succ != succ_end; ++succ) {
            auto f = BBIndex.find(*succ);
            assert (f != BBIndex.end());
            auto prob = MBPI.getEdgeProbability(&MBB, succ);
            CBB.Successors.emplace_back(f->second, prob);
        }
        END_SCOPED_REGION

        for (MachineInstr & MI : MBB) {
            if (MI.getFlag(MachineInstr::FrameSetup) || MI.getFlag(MachineInstr::FrameDestroy)) {
                continue;
            }
            if (MI.isReturn()) {

                ReturnRegSet.insert(cache.LiveOuts.begin(), cache.LiveOuts.end());

                auto findAnyLiveOutRegMappings = [&](auto inst, auto end) -> bool {
                    while (inst != end) {
                        if (inst->isCopy()) {
                            auto dst = inst->getOperand(0).getReg();
                            if (dst.isPhysical()) {
                                auto phyReg = dst.asMCReg();
                                auto f = ReturnRegSet.find(phyReg);
                                if (f != ReturnRegSet.end()) {
                                    auto src = inst->getOperand(1).getReg();
                                    if (src.isVirtual()) {
                                        CBB.LiveOuts.emplace_back(phyReg, src);
                                        ReturnRegSet.erase(f);
                                        if (ReturnRegSet.empty()) {
                                            return true;
                                        }
                                    }
                                }
                            }
                        }
                        ++inst;
                    }
                    return false;
                };

                if (LLVM_UNLIKELY(!findAnyLiveOutRegMappings(std::next(MI.getReverseIterator()), MBB.rend()))) {
                    for (auto bbItr = std::next(MBB.getReverseIterator()); bbItr != MF.rend(); ++bbItr) {
                        if (findAnyLiveOutRegMappings(bbItr->rbegin(), bbItr->rend())) {
                            break;
                        }
                    }
                }
                assert (ReturnRegSet.empty());

                CacheInst ret(MI);
                // We mark a return statement with TargetOpcode::PATCHABLE_RET since it cannot occur normally
                // at this state of the compilation process and acts as a clear IsReturn flag. It must not be
                // used as an actual OpCode by the inlining function.
                ret.Opcode = TargetOpcode::PATCHABLE_RET;
                CBB.Instructions.emplace_back(CacheInst(MI));

                cache.NumOfExitBlocks++;
                break;

            } else {
                CacheInst cinst(MI);
                for (const MachineOperand & op : MI.operands()) {
                    CacheOperand CO(op);
                    if (op.isReg()) {
                        auto reg = op.getReg();
                        if (reg.isVirtual()) {
                            auto c = MRI.getRegClass(reg);
                            CO.RegClass = c;
                            cache.RegClassMap.insert(std::make_pair(reg, c));
                        }

                        unsigned Flags = 0;
                        if (op.isDef()) Flags |= RegState::Define;
                        if (op.isImplicit()) Flags |= RegState::Define;
                        if (op.isKill()) Flags |= RegState::Kill;
                        if (op.isDead()) Flags |= RegState::Dead;
                        if (op.isUndef()) Flags |= RegState::Undef;
                        if (op.isEarlyClobber()) Flags |= RegState::EarlyClobber;
                        CO.Flags = Flags;

                    }
                    cinst.Operands.emplace_back(CO);
                }
                CBB.Instructions.emplace_back(cinst);
            }
        }
    }

    assert (cache.NumOfExitBlocks > 0);

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
                snippetFunction->addFnAttr(Attribute::NoInline);
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

FunctionSnippetPassManagerProxy::FunctionSnippetPassManagerProxy(llvm::Module * M, llvm::PassManagerBase & pm, const bool addPostOptimizations)
: BasePM(pm)
, AddPostOptimizations(addPostOptimizations) {
    // TODO: we need to scan through the function declarations in M to see if any snippets are single use ones.
    // We can add a LLVM AlwaysInline attribute to those to let LLVM deal with them completely. Only if we have a
    // multi-use snippet do we need to run the snippet inliner pass on M. This has the added benefit of the pass
    // knowing it will have some work to do on at least one function in M.
}


void FunctionSnippetPassManagerProxy::add(Pass * P) {
    if (LLVM_UNLIKELY(P == nullptr)) return;
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
}
