/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include "idisa_operations.h"

#include <kernel/io/source_kernel.h>
#include <kernel/util/hex_convert.h>

#include "idisa_exerciser.h"
#include "safe_ops.h"

using namespace std;
using namespace llvm;
using namespace kernel;

//////////////////////////////////////////////////////////////////////////////////
// Kernels used by the OperationConfigs

class TestKernel : public MultiBlockKernel {
  public:
    TestKernel(LLVMTypeSystemInterface &ts, OperationConfig &config, const vector<StreamSet *> &operandSSs,
               StreamSet *testOutput)
        : MultiBlockKernel(ts, "test_" + config.getIdentifier().str(), captureOperandBindings(operandSSs),
                           {{"test_output", testOutput}}, {}, {}, {}),
          mConfig(config), mNumOperands(operandSSs.size()) {}

  protected:
    void generateMultiBlockLogic(KernelBuilder &b, Value *const numBlocks) override;

  private:
    OperationConfig &mConfig;
    unsigned mNumOperands;

    Bindings captureOperandBindings(const vector<StreamSet *> &operandSSs);

    TestKernel(const TestKernel &) = delete;
};

void TestKernel::generateMultiBlockLogic(KernelBuilder &b, Value *const numBlocks) {
    mConfig.logKernelBuilder(b);

    BasicBlock *entry = b.GetInsertBlock();
    BasicBlock *processBlock = b.CreateBasicBlock("processBlock");
    BasicBlock *done = b.CreateBasicBlock("done");
    b.CreateBr(processBlock);
    b.SetInsertPoint(processBlock);
    PHINode *blockOffsetPhi = b.CreatePHI(b.getSizeTy(), 2);
    Constant *zero = b.getSize(0);
    blockOffsetPhi->addIncoming(zero, entry);

    vector<Value *> operandBlocks;
    for (unsigned i = 0; i < mNumOperands; ++i) {
        operandBlocks.emplace_back(b.loadInputStreamBlock(OperationConfig::operandIdent(i), zero, blockOffsetPhi));
    }
    Value *testOutputBlock = mConfig.makeTestLogic(b, operandBlocks);

    b.storeOutputStreamBlock(OperationConfig::testOutputIdent, zero, blockOffsetPhi, b.bitCast(testOutputBlock));
    Value *nextBlk = b.CreateAdd(blockOffsetPhi, b.getSize(1));
    blockOffsetPhi->addIncoming(nextBlk, processBlock);
    Value *moreToDo = b.CreateICmpNE(nextBlk, numBlocks);
    b.CreateCondBr(moreToDo, processBlock, done);
    b.SetInsertPoint(done);
}

Bindings TestKernel::captureOperandBindings(const vector<StreamSet *> &operandSSs) {
    Bindings bs;
    bs.reserve(1);
    unsigned i = 0;
    for (auto ss : operandSSs) {
        string name = OperationConfig::operandIdent(i++);
        bs.emplace_back(name, ss);
    }
    return bs;
}

class CheckKernel : public BlockOrientedKernel {
  public:
    CheckKernel(LLVMTypeSystemInterface &ts, OperationConfig &config, const vector<StreamSet *> &operandSSs,
                StreamSet *testOutput, StreamSet *expectedOutput, Scalar *failureCount)
        : BlockOrientedKernel(ts, "check_" + config.getIdentifier().str() + (QuietMode ? "_q" : ""),
                              captureOperandBindings(operandSSs, testOutput),
                              {Binding{OperationConfig::expectedOutputIdent, expectedOutput}}, {},
                              {Binding{OperationConfig::failureCountIdent, failureCount}}, {}),
    mConfig(config), mQuiet(QuietMode), mNumOperands(operandSSs.size()) {}

  protected:
    void generateDoBlockMethod(KernelBuilder &b) override;

  private:
    OperationConfig &mConfig;
    bool mQuiet;
    unsigned mNumOperands;

    Bindings captureOperandBindings(const vector<StreamSet *> &operandSSs, StreamSet *testOutput);
};

void CheckKernel::generateDoBlockMethod(KernelBuilder &b) {
    Constant *zero = b.getSize(0);
    Value *testOutputBlock = b.bitCast(b.loadInputStreamBlock(OperationConfig::testOutputIdent, zero));
    vector<Value *> operandBlocks;
    for (unsigned i = 0; i < mNumOperands; ++i) {
        operandBlocks.emplace_back(b.bitCast(b.loadInputStreamBlock(OperationConfig::operandIdent(i), zero)));
    }
    Value *expectedOutputBlock = b.bitCast(mConfig.makeCheckLogic(b, operandBlocks));

    // unsigned fw = mConfig.getFieldWidth();
    // if (fw <= 8) {
    //     unsigned fn = b.getBitBlockWidth() / fw;

    //     errs() << "fw < 8: bbw=" << b.getBitBlockWidth() << ", fn=" << fn << ", fw=" << fw << ", fvt=";
    //     expectedOutputBlock->getType()->print(errs());
    //     errs() << ", lob=";
    //     b.fwVectorType(b.getLaneWidth())->print(errs());
    //     errs() << "\n";
    //     errs().flush();

    //     Value *laneOutputBlock = Constant::getNullValue(b.fwVectorType(b.getLaneWidth()));
    //     unsigned lanesPerBlock = b.getBitBlockWidth() / b.getLaneWidth();
    //     unsigned fieldsPerLane = b.getLaneWidth() / fw;
    //     for (unsigned i = 0; i < lanesPerBlock; ++i) {
    //         Value *laneField = Constant::getNullValue(b.getLaneTy());
    //         for (unsigned j = 0; j < fieldsPerLane; ++j) {
    //             laneField = b.CreateOr(
    //                 laneField,
    //                 b.CreateShl(
    //                     b.CreateZExt(b.CreateExtractElement(expectedOutputBlock, i * fieldsPerLane + j),
    //                     b.getLaneTy()), j * fw));
    //         }
    //         laneOutputBlock = b.CreateInsertElement(laneOutputBlock, laneField, i);
    //     }
    //     expectedOutputBlock = laneOutputBlock;
    // }

    b.storeOutputStreamBlock(OperationConfig::expectedOutputIdent, zero, expectedOutputBlock);
    Value *failures = b.CreateICmpNE(b.bitCast(testOutputBlock), b.bitCast(expectedOutputBlock));
    Value *failuresWord = b.CreateBitCast(failures, b.getIntNTy(b.getBitBlockWidth() / b.getLaneWidth()));
    assert(b.getSizeTy()->getPrimitiveSizeInBits() >= (b.getBitBlockWidth() / b.getLaneWidth()));
    Value *failureCount = b.CreatePopcount(b.CreateZExt(failuresWord, b.getSizeTy()));
    b.setScalarField(OperationConfig::failureCountIdent,
                     b.CreateAdd(b.getScalarField(OperationConfig::failureCountIdent), failureCount));
    if (!mQuiet) {
        // created here so they always get terminators; unterminated blocks crash the JIT under -q
        BasicBlock *reportFailure = b.CreateBasicBlock("reportFailure");
        BasicBlock *continueTest = b.CreateBasicBlock("continueTest");
        Value *anyFailure = b.CreateICmpNE(failureCount, zero);
        b.CreateCondBr(anyFailure, reportFailure, continueTest);
        b.SetInsertPoint(reportFailure);
        for (unsigned i = 0; i < mNumOperands; ++i) {
            b.CallPrintRegister(OperationConfig::operandIdent(i), b.bitCast(operandBlocks[i]));
        }
        b.CallPrintRegister(mConfig.getDescription(), b.bitCast(testOutputBlock));
        b.CallPrintRegister("expecting", b.bitCast(expectedOutputBlock));
        b.CreateBr(continueTest);
        b.SetInsertPoint(continueTest);
    }
}

Bindings CheckKernel::captureOperandBindings(const vector<StreamSet *> &operandSSs, StreamSet *testOutput) {
    Bindings bs;
    unsigned i = 0;
    for (auto ss : operandSSs) {
        bs.emplace_back(OperationConfig::operandIdent(i++), ss);
    }
    bs.emplace_back(OperationConfig::testOutputIdent, testOutput);
    return bs;
}

class DummyCheckKernel : public Kernel {
  public:
    DummyCheckKernel(LLVMTypeSystemInterface &ts, OperationConfig &config, StreamSet *testOutput, Scalar *failureCount)
        : Kernel(ts, TypeId::SegmentOriented, "dummycheck_" + config.getIdentifier().str(),
                 {Binding(OperationConfig::testOutputIdent, testOutput)}, {}, {},
                 {Binding{OperationConfig::failureCountIdent, failureCount}}, {}) {}

  protected:
    void generateKernelMethod(KernelBuilder &b, llvm::TargetMachine *) override {}

    void generateFinalizeMethod(KernelBuilder &b) override {
        b.setScalarField(OperationConfig::failureCountIdent, b.getSize(0));
    }
};

//////////////////////////////////////////////////////////////////////////////////
// OperationConfig implementation

OperationConfig::~OperationConfig() {
    for (auto const &f : mDtorFuncs) {
        f();
    }
}

void OperationConfig::fillParams(KernelBuilder &b, const vector<Value *> &operands, Params &outParams) const {
    outParams.fw = mFieldWidth;
    outParams.fn = b.getBitBlockWidth() / mFieldWidth;
    outParams.fTy = b.getIntNTy(mFieldWidth);
    outParams.vTy = b.fwVectorType(mFieldWidth);
    outParams.i32Ty = b.getInt32Ty();
    outParams.i64Ty = b.getInt64Ty();
}

void OperationConfig::resetPipeline() {
    for (auto const &f : mResetFuncs) {
        f();
    }
}

bool OperationConfig::configurePipelineFromArgs(cl::list<string> &args, bool doChecks) {
    assert(mPipelineBuilder);
#ifndef NDEBUG
    assert(!mPipelineConfigured);
    mPipelineConfigured = true;
#endif

    size_t cur = 0;
    if (parseRelevantArgs(args, cur)) {
        return true;
    }
    if (cur < args.size()) {
        return args.error("Unexpected operation arguments: expected " + to_string(cur) + ", got " +
                          to_string(args.size()));
    }

    return false;
}

bool OperationConfig::parseRelevantArgs(cl::list<string> &args, size_t &cur) { return false; }

bool OperationConfig::parseUnsigned(cl::list<string> &args, size_t &cur, StringRef expectName, unsigned minVal,
                                    unsigned maxVal, unsigned &outResult) {
    if (cur >= args.size()) {
        return args.error("Not enough arguments, expected " + expectName);
    }
    string argStr = args[cur];
    char *pc = &argStr[0];
    long long result = strtoll(pc, &pc, 10);
    if (*pc != 0) {
        return args.error("Expected " + expectName + "; could not parse value");
    }
    if ((result < static_cast<long long>(minVal)) || (result > static_cast<long long>(maxVal))) {
        return args.error("Expected " + expectName + "; value outside range " + to_string(minVal) + "-" +
                          to_string(maxVal));
    }
    outResult = result;
    ++cur;
    return false;
}

bool OperationConfig::parseStreamSource(cl::list<string> &args, size_t &cur, StringRef expectName, StringRef fdIdent,
                                        int32_t &outFDResult, StreamSet *&outSSResult) {
    // Is it a filename? For now assume it is.
    // Future work:
    // - syntax to specify internal generators that generate into memory such as
    //   - random numbers w/seed?
    //   - structured test patterns?
    //   - patterns from an internal library?
    //   - synthesized fuzzing patterns?
    // - syntax to specify processing chains (hex parsing, shift mask)
    if (cur >= args.size()) {
        return args.error("Not enough arguments, expected " + expectName);
    }
    if (openInputFile(args, args[cur], outFDResult)) {
        return true;
    }
    if (outFDResult == STDIN_FILENO) {
        if (mStdinGrabbed) {
            return args.error("At most 1 input can come from STDIN");
        }
        mStdinGrabbed = true;
    }
    ++cur;

    Scalar *fileDescriptor = mPipelineBuilder->getInputScalar(fdIdent);
    assert(fileDescriptor);
    StreamSet *bitStream = mPipelineBuilder->CreateStreamSet(1, 8);
    mPipelineBuilder->CreateKernelCall<ReadSourceKernel>(fileDescriptor, bitStream);
    // Uncomment below (and comment above) to get back the default behaviour of parsing the input as a hex file
    // -- I would much prefer if this was associated with some kind of syntax.
    // Then we might consider having ways to specify different processing chains...
    // Scalar *fileDescriptor = mPipelineBuilder->getInputScalar(outFDResult);
    // StreamSet * hexStream = mPipelineBuilder->CreateStreamSet(1, 8);
    // StreamSet *bitStream = mPipelineBuilder->CreateStreamSet(1, 1);
    // mPipelineBuilder->CreateKernelCall<ReadSourceKernel>(fileDescriptor, hexStream);
    // mPipelineBuilder->CreateKernelCall<HexToBinary>(hexStream, bitStream);
    outSSResult = bitStream;

    return false;
}

bool OperationConfig::openInputFile(cl::Option &arg, StringRef fileName, int32_t &outFDResult) {
    if (fileName == "-") {
        outFDResult = STDIN_FILENO;
        return false;
    } else {
        int32_t fileDescriptor = open(fileName.str().c_str(), O_RDONLY);
        if (fileDescriptor == -1) {
            return arg.error(fileName + ": " + strerror(errno));
        }
        char temp;
        if ((read(fileDescriptor, &temp, 0) == -1) && (errno == EISDIR)) {
            close(fileDescriptor);
            return arg.error(fileName + ": Is a directory");
        }
        mResetFuncs.emplace_back([=]() { lseek(fileDescriptor, 0, SEEK_SET); });
        mDtorFuncs.emplace_back([=]() { close(fileDescriptor); });
        outFDResult = fileDescriptor;
        return false;
    }
}

//////////////////////////////////////////////////////////////////////////////////
// Specializations of config to handle different classes of operations

template <unsigned N> class NaryOpConfig : public OperationConfig {
  public:
    struct Params : public OperationConfig::Params {
        Value *opr[N]; // operands
    };

    explicit NaryOpConfig(StringRef description, StringRef identifier, unsigned fieldWidth)
        : OperationConfig(description, identifier, fieldWidth) {}

    void fillParams(KernelBuilder &b, const vector<Value *> &operands, Params &outParams) const {
        assert(operands.size() == N);
        OperationConfig::fillParams(b, operands, outParams);
        for (unsigned i = 0; i < N; ++i) {
            outParams.opr[i] = operands[i];
        }
    }

  protected:
    vector<int32_t> mOperandFDs;
    function<void()> mCompileFunc;

    void constructPipeline(CPUDriver &driver) override;

    void compilePipeline() override {
        assert(mPipelineBuilder);
        mCompileFunc();
        assert(!mKernelBuilderID.empty());
    }

    size_t executePipeline() override {
        resetPipeline();
        assert(mPipelineBuilder);
        return mExecuteFunc();
    }

    bool configurePipelineFromArgs(cl::list<string> &args, bool doChecks) override {
        if (OperationConfig::configurePipelineFromArgs(args, doChecks)) {
            return true;
        }

        assert(mOperandSSs.size() == N);
        return false;
    }

    bool parseRelevantArgs(cl::list<string> &args, size_t &cur) override {
        if (OperationConfig::parseRelevantArgs(args, cur)) {
            return true;
        }

        for (unsigned i = 0; i < N; ++i) {
            int32_t fd = -1;
            StreamSet *ss = nullptr;
            if (parseStreamSource(args, cur, "operand " + to_string(i + 1), OperationConfig::operandFDIdent(i), fd,
                                  ss)) {
                return true;
            }
            mOperandFDs.emplace_back(fd);
            mOperandSSs.emplace_back(ss);
        }
        return false;
    }
};

template <> void NaryOpConfig<0>::constructPipeline(CPUDriver &driver);
template <> void NaryOpConfig<1>::constructPipeline(CPUDriver &driver);
template <> void NaryOpConfig<2>::constructPipeline(CPUDriver &driver);
template <> void NaryOpConfig<3>::constructPipeline(CPUDriver &driver);

// I found lots of template magic in the PipelineBuilder, but nothing that would assist with building up a
// function signature and matching call programmatically. Since we don't actually need a fully general system,
// this mediocrity will do: at least it keeps the special-casing local, so it can be replaced easily if a better
// solution is wanted.

// Maybe you have a better way? It sure wants to be refactored!

template <> void NaryOpConfig<0>::constructPipeline(CPUDriver &driver) {
    assert(!mPipelineBuilder);
    auto pTmp = CreatePipeline(driver, Input<const char *>{OperationConfig::outputFilenameIdent},
                               Output<size_t>{OperationConfig::failureCountIdent});
    auto p = make_unique<decltype(pTmp)>(std::move(pTmp));
    auto pPtr = p.get();
    mPipelineBuilder = std::move(p);
    mCompileFunc = [=, this]() {
        auto pipelineFunc = pPtr->compile();
        mExecuteFunc = [=, this]() -> size_t { return pipelineFunc(mOutputFilename.c_str()); };
    };
}

template <> void NaryOpConfig<1>::constructPipeline(CPUDriver &driver) {
    assert(!mPipelineBuilder);
    auto pTmp = CreatePipeline(driver, Input<int32_t>{OperationConfig::operandFDIdent(0)},
                               Input<const char *>{OperationConfig::outputFilenameIdent},
                               Output<size_t>{OperationConfig::failureCountIdent});
    auto p = make_unique<decltype(pTmp)>(std::move(pTmp));
    auto pPtr = p.get();
    mPipelineBuilder = std::move(p);
    mCompileFunc = [=, this]() {
        auto pipelineFunc = pPtr->compile();
        mExecuteFunc = [=, this]() -> size_t { return pipelineFunc(mOperandFDs[0], mOutputFilename.c_str()); };
    };
}

template <> void NaryOpConfig<2>::constructPipeline(CPUDriver &driver) {
    assert(!mPipelineBuilder);
    auto pTmp = CreatePipeline(
        driver, Input<int32_t>{OperationConfig::operandFDIdent(0)}, Input<int32_t>{OperationConfig::operandFDIdent(1)},
        Input<const char *>{OperationConfig::outputFilenameIdent}, Output<size_t>{OperationConfig::failureCountIdent});
    auto p = make_unique<decltype(pTmp)>(std::move(pTmp));
    auto pPtr = p.get();
    mPipelineBuilder = std::move(p);
    mCompileFunc = [=, this]() {
        auto pipelineFunc = pPtr->compile();
        mExecuteFunc = [=, this]() -> size_t {
            return pipelineFunc(mOperandFDs[0], mOperandFDs[1], mOutputFilename.c_str());
        };
    };
}

template <> void NaryOpConfig<3>::constructPipeline(CPUDriver &driver) {
    assert(!mPipelineBuilder);
    auto pTmp = CreatePipeline(
        driver, Input<int32_t>{OperationConfig::operandFDIdent(0)}, Input<int32_t>{OperationConfig::operandFDIdent(1)},
        Input<int32_t>{OperationConfig::operandFDIdent(2)}, Input<const char *>{OperationConfig::outputFilenameIdent},
        Output<size_t>{OperationConfig::failureCountIdent});
    auto p = make_unique<decltype(pTmp)>(std::move(pTmp));
    auto pPtr = p.get();
    mPipelineBuilder = std::move(p);
    mCompileFunc = [=, this]() {
        auto pipelineFunc = pPtr->compile();
        mExecuteFunc = [=, this]() -> size_t {
            return pipelineFunc(mOperandFDs[0], mOperandFDs[1], mOperandFDs[2], mOutputFilename.c_str());
        };
    };
}

template <class BaseOpConfig, unsigned MinVal = 0, unsigned MaxVal = UINT_MAX>
class ImmediateOpConfig : public BaseOpConfig {
  public:
    struct Params : public BaseOpConfig::Params {
        uint64_t immed; // immediate
    };

    explicit ImmediateOpConfig(StringRef description, StringRef identifier, unsigned fieldWidth)
        : BaseOpConfig(description, identifier, fieldWidth), mImmediateValue(0) {}

    void fillParams(KernelBuilder &b, const vector<Value *> &operands, Params &outParams) const {
        BaseOpConfig::fillParams(b, operands, outParams);
        outParams.immed = mImmediateValue;
    }

  protected:
    uint64_t mImmediateValue;

    using BaseOpConfig::mDescription;
    using BaseOpConfig::mIdentifier;

    bool parseRelevantArgs(cl::list<string> &args, size_t &cur) override {
        unsigned immedVal = 0;
        if (BaseOpConfig::parseRelevantArgs(args, cur) ||
            BaseOpConfig::parseUnsigned(args, cur, "immediate", MinVal, MaxVal, immedVal)) {
            return true;
        }
        mDescription = mDescription + " #" + to_string(immedVal);
        mIdentifier = mIdentifier + "_#" + to_string(immedVal);
        mImmediateValue = immedVal;
        return false;
    }
};

template <class BaseOpConfig, ExpectedType Typing, class TestF, class ExpectedF>
class GenericOpConfig : public BaseOpConfig {
  public:
    using typename BaseOpConfig::Params;

    GenericOpConfig(StringRef description, StringRef identifier, unsigned fieldWidth, bool quiet, TestF testF,
                    ExpectedF expectedF)
    : BaseOpConfig(description, identifier, fieldWidth), mTestF(testF), mExpectedF(expectedF) {}

    using BaseOpConfig::fillParams;
    using BaseOpConfig::getDescription;
    using BaseOpConfig::getFieldWidth;

    Value *makeTestLogic(KernelBuilder &b, const vector<Value *> &operands) override {
        Params params;

        fillParams(b, operands, params);
        Value *result = mTestF(b, *this, params);
// Initially I wanted to be kind of picky about the types we get back, but LLVM has this habit of widening
// vector of i2 and i4 to vector of i8, and so we can't actually return the "correct" vector type most of the
// time.
#ifndef NDEBUG
        switch (Typing) {
        case ExpectedType::bit:
            assert(result->getType() == b.getInt1Ty());
            break;
        case ExpectedType::field:
            assert(result->getType() == b.getIntNTy(getFieldWidth()));
            break;
        case ExpectedType::size:
            assert(result->getType() == b.getSizeTy());
            break;
        case ExpectedType::fieldVector:
        case ExpectedType::bitBlock:
            assert(result->getType()->getPrimitiveSizeInBits() == b.getBitBlockWidth());
            // if (getFieldWidth() >= 8) {
            //     assert(result->getType() == b.fwVectorType(getFieldWidth()));
            // } else {
            //     assert(result->getType() == b.fwVectorType(8));
            // }
            // assert(result->getType() == b.getBitBlockType()));
            break;
        }
// Type *expectedTy = nullptr;
// if (result->getType() != expectedTy) {
//     errs() << "Actual returned type: ";
//     result->getType()->print(errs());
//     errs() << ", expected ";
//     expectedTy->print(errs());
//     errs() << "\n";
//     errs().flush();
//     assert(result->getType() == expectedTy);
// }
#endif
        if (Typing == ExpectedType::bit || Typing == ExpectedType::field || Typing == ExpectedType::size) {
            unsigned maxSize = max<unsigned>(b.getSizeTy()->getPrimitiveSizeInBits(), params.fw);
            result = SafeInsertElement(b, maxSize, Constant::getNullValue(b.fwVectorType(maxSize)),
                                       b.CreateZExtOrTrunc(result, b.getIntNTy(maxSize)), uint64_t(0));
        }
        return result;
    }

    Value *makeCheckLogic(KernelBuilder &b, const vector<Value *> &operands) override {
        Params params;
        fillParams(b, operands, params);
        Value *result = mExpectedF(b, *this, params);
        unsigned resBits = max<unsigned>(params.fw, b.getSizeTy()->getPrimitiveSizeInBits());
        switch (Typing) {
        case ExpectedType::bit:
        case ExpectedType::field:
        case ExpectedType::size: {
            Type *eltTy = b.getIntNTy(resBits);
            assert(resBits >= result->getType()->getPrimitiveSizeInBits());
            return b.CreateInsertElement(Constant::getNullValue(b.fwVectorType(resBits)), b.CreateZExt(result, eltTy),
                                         uint64_t(0));
        }
        default:
            return b.bitCast(result);
        }
    }

    bool configurePipelineFromArgs(cl::list<string> &args, bool doChecks) override {
        if (BaseOpConfig::configurePipelineFromArgs(args, doChecks)) {
            return true;
        }

        mTestOutput = mPipelineBuilder->CreateStreamSet(1, 1);
        mPipelineBuilder->template CreateKernelCall<TestKernel>(*this, mOperandSSs, mTestOutput);
        if (doChecks) {
            mExpectedOutput = mPipelineBuilder->CreateStreamSet(1, 1);
            mPipelineBuilder->template CreateKernelCall<CheckKernel>(
                *this, mOperandSSs, mTestOutput, mExpectedOutput,
                mPipelineBuilder->getOutputScalar(OperationConfig::failureCountIdent));
        } else {
            // Dummy kernel just to provide a 0 error count
            mPipelineBuilder->template CreateKernelCall<DummyCheckKernel>(
                *this, mTestOutput, mPipelineBuilder->getOutputScalar(OperationConfig::failureCountIdent));
        }
        return false;
    }

  protected:
    using BaseOpConfig::mDescription;
    using BaseOpConfig::mExpectedOutput;
    using BaseOpConfig::mFieldWidth;
    using BaseOpConfig::mIdentifier;
    using BaseOpConfig::mOperandSSs;
    using BaseOpConfig::mPipelineBuilder;
    using BaseOpConfig::mTestOutput;

    bool mQuiet;
    TestF mTestF;
    ExpectedF mExpectedF;
};

// Handy aliases
using UnaryOpConfig = NaryOpConfig<1>;
using BinaryOpConfig = NaryOpConfig<2>;
using TernaryOpConfig = NaryOpConfig<3>;
using ImmedUnOpConfig = ImmediateOpConfig<UnaryOpConfig, 0, 512>;
using ImmedBinOpConfig = ImmediateOpConfig<BinaryOpConfig, 0, 512>;
using ImmedTernOpConfig = ImmediateOpConfig<TernaryOpConfig, 0, 512>;

//////////////////////////////////////////////////////////////////////////////////
// And now, the actual index of operations...

struct OperationIndexEntry {
    typedef bool (*FactoryFunc)(unique_ptr<OperationConfig> &outOpConfig);

    const char *name;
    const char *helpOprs;
    const char *helpDesc;
    FactoryFunc factory;
};

template <size_t N> struct StrucString {
    constexpr StrucString(const char (&str)[N]) { copy_n(str, N, value); }

    char value[N];
    auto operator<=>(const StrucString &) const = default;
    bool operator==(const StrucString &) const = default;
};

template <class BaseConfigT, ExpectedType Typing, StrucString Name, StrucString HelpOprs, StrucString HelpDesc,
          auto TestF, auto ExpectedF>
static constexpr OperationIndexEntry genericEntry() {
    return OperationIndexEntry{
        Name.value, HelpOprs.value, HelpDesc.value, [](unique_ptr<OperationConfig> &outOpConfig) {
            unsigned fw = OperationFieldWidth;
            if ((fw < 1) || (fw > 128)) {
                return OperationFieldWidth.error("Must be in range 1-128");
            }
            if ((fw & (fw - 1)) != 0) {
                return OperationFieldWidth.error("Must be a power of 2");
            }
            outOpConfig = make_unique<GenericOpConfig<BaseConfigT, Typing, decltype(TestF), decltype(ExpectedF)>>(
                Name.value, Name.value, OperationFieldWidth, QuietMode, TestF, ExpectedF);
            return false;
        }};
}

// I ended up with 2 versions of these wrappers by an accident of refactoring order. It shouldn't be too hard to
// minimize them to 1 shared pair, but it's not quite trivial so dear reader, it's your problem now.

template <class ConfigT, ExpectedType Typing, StrucString Name, StrucString HelpOprs, StrucString HelpDesc, auto TestF,
          auto HorizontalStoreExpectedF>
static constexpr OperationIndexEntry horizontalStoreCheckEntry() {
    constexpr auto expectedF = [](KernelBuilder &b, const ConfigT &c, const ConfigT::Params &p) -> Value * {
        unsigned fieldCount = b.getBitBlockWidth() / p.fw;
        // Since this is for the check, use null to ensure consistent results rather than poison
        Value *outBlock = Constant::getNullValue(p.vTy);
        typename ConfigT::Params scalarP = p;
        for (unsigned i = fieldCount; i > 0; i--) {
            for (unsigned j = 0; j < size(scalarP.opr); ++j) {
                scalarP.opr[j] = SafeExtractElement(b, p.fw, p.opr[j], i - 1);
            }
            outBlock = HorizontalStoreExpectedF(b, c, scalarP, outBlock, i - 1);
        }
        return outBlock;
    };
    return genericEntry<ConfigT, Typing, Name, HelpOprs, HelpDesc, TestF, expectedF>();
}

template <class ConfigT, ExpectedType Typing, StrucString Name, StrucString HelpOprs, StrucString HelpDesc, auto TestF,
          auto ScalarExpectedF>
static constexpr OperationIndexEntry scalarCheckEntry() {
    constexpr auto expectedF = [](KernelBuilder &b, const ConfigT &c, const ConfigT::Params &p, Value *outBlock,
                                  unsigned i) -> Value * {
        Value *out = ScalarExpectedF(b, c, p);
        return SafeInsertElement(b, p.fw, outBlock, out, i);
    };
    return horizontalStoreCheckEntry<ConfigT, Typing, Name, HelpOprs, HelpDesc, TestF, expectedF>();
}

// Sort of redundant with the above, oops

template <class ConfigT, class HorizontalStoreCheckF>
auto wrapHorizontalStore(HorizontalStoreCheckF &&horizontalStoreCheckF) {
    return [=](KernelBuilder &b, const ConfigT &c, const ConfigT::Params &p) {
        unsigned fieldCount = b.getBitBlockWidth() / p.fw;
        Value *expectedBlock = Constant::getNullValue(b.fwVectorType(p.fw));
        for (unsigned i = 0; i < fieldCount; i++) {
            typename ConfigT::Params scalarP = p;
            for (unsigned j = 0; j < size(scalarP.opr); ++j) {
                scalarP.opr[j] = SafeExtractElement(b, p.fw, p.opr[j], i);
            }
            expectedBlock = horizontalStoreCheckF(b, c, scalarP, expectedBlock, i);
        }
        return expectedBlock;
    };
};

template <class ConfigT, class ScalarCheckF> auto wrapScalar(ScalarCheckF &&scalarCheckF) {
    return wrapHorizontalStore<ConfigT>(
        [=](KernelBuilder &b, const ConfigT &c, const ConfigT::Params &p, Value *expectedBlock, unsigned i) {
            Value *expected = scalarCheckF(b, c, p);
            return SafeInsertElement(b, p.fw, expectedBlock, expected, i);
        });
}

OperationIndexEntry allOperations[] = {
    scalarCheckEntry<UnaryOpConfig, ExpectedType::fieldVector, "simd_select_hi", "x0", "y[i] <- x[i] & high-half mask",
                     [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                         return b.simd_select_hi(p.fw, p.opr[0]);
                     },
                     [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                         return b.CreateAnd(p.opr[0], APInt::getHighBitsSet(p.fw, p.fw / 2));
                     }>(),
    scalarCheckEntry<UnaryOpConfig, ExpectedType::fieldVector, "simd_select_lo", "x0", "y[i] <- x[i] & low-half mask",
                     [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                         return b.simd_select_lo(p.fw, p.opr[0]);
                     },
                     [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                         return b.CreateAnd(p.opr[0], APInt::getLowBitsSet(p.fw, p.fw / 2));
                     }>(),
    genericEntry<UnaryOpConfig, ExpectedType::fieldVector, "simd_fill", "x0", "y[i] <- x[0]; splatting operator",
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     Value *scalarOpr0 = SafeExtractElement(b, p.fw, p.opr[0], uint64_t(0));
                     return b.simd_fill(p.fw, scalarOpr0);
                 },
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     Value *expectedBlock = Constant::getNullValue(p.vTy);
                     Value *scalarOpr0 = SafeExtractElement(b, p.fw, p.opr[0], uint64_t(0));
                     for (unsigned i = 0; i < p.fn; ++i) {
                         expectedBlock = SafeInsertElement(b, p.fw, expectedBlock, scalarOpr0, i);
                     }
                     return expectedBlock;
                 }>(),
    scalarCheckEntry<UnaryOpConfig, ExpectedType::fieldVector, "simd_any", "x0", "",
                     [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                         return b.simd_any(p.fw, p.opr[0]);
                     },
                     [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                         return b.CreateSExt(b.CreateICmpNE(p.opr[0], b.getIntN(p.fw, 0)), b.getIntNTy(p.fw));
                     }>(),
    scalarCheckEntry<UnaryOpConfig, ExpectedType::fieldVector, "simd_popcount", "x0", "",
                     [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                         return b.simd_popcount(p.fw, p.opr[0]);
                     },
                     [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                         Value *accum = b.getIntN(p.fw, 0);
                         for (unsigned i = 0; i < p.fw; ++i) {
                             accum = b.CreateAdd(b.CreateAnd(b.CreateLShr(p.opr[0], i), 1), accum);
                         }
                         return accum;
                     }>(),
    scalarCheckEntry<UnaryOpConfig, ExpectedType::fieldVector, "simd_cttz", "x0", "",
                     [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                         return b.simd_cttz(p.fw, p.opr[0]);
                     },
                     [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                         Value *accum = b.getIntN(p.fw, p.fw);
                         Value *zero = b.getIntN(p.fw, 0);
                         // Eval order is opposite to construction order (LIFO)
                         for (unsigned i = 0; i < p.fw; ++i) {
                             accum = b.CreateSelect(
                                 b.CreateICmpNE(b.CreateAnd(b.CreateLShr(p.opr[0], p.fw - 1 - i), 1), zero),
                                 b.getIntN(p.fw, p.fw - 1 - i), accum);
                         }
                         return accum;
                     }>(),
    scalarCheckEntry<UnaryOpConfig, ExpectedType::fieldVector, "simd_bitreverse", "x0", "",
                     [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                         return b.simd_bitreverse(p.fw, p.opr[0]);
                     },
                     [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                         Value *accum = nullptr;
                         for (unsigned i = 0; i < p.fw; ++i) {
                             Value *shifted;
                             if (i * 2 <= p.fw - 1) {
                                 shifted = b.CreateLShr(p.opr[0], p.fw - 1 - i * 2);
                             } else {
                                 shifted = b.CreateShl(p.opr[0], i * 2 - (p.fw - 1));
                             }
                             shifted = b.CreateAnd(shifted, uint64_t(1) << i);
                             if (accum) {
                                 accum = b.CreateOr(accum, shifted);
                             } else {
                                 accum = shifted;
                             }
                         }
                         return accum;
                     }>(),
    genericEntry<UnaryOpConfig, ExpectedType::fieldVector, "hsimd_partial_sum", "x0", "",
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     return b.hsimd_partial_sum(p.fw, p.opr[0]);
                 },
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     Value *expectedBlock = p.opr[0];
                     for (unsigned i = 2; i <= p.fn; i *= 2) {
                         for (unsigned j = p.fn; j != i / 2; --j) {
                             Value *partialIJ = b.CreateAdd(SafeExtractElement(b, p.fw, expectedBlock, j - 1),
                                                            SafeExtractElement(b, p.fw, expectedBlock, j - 1 - i / 2));
                             expectedBlock = SafeInsertElement(b, p.fw, expectedBlock, partialIJ, j - 1);
                         }
                     }
                     return expectedBlock;
                 }>(),
    genericEntry<UnaryOpConfig, ExpectedType::fieldVector, "esimd_bitspread", "x0", "",
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     Value *scalarOpr0 = SafeExtractElement(b, p.fw, p.opr[0], uint64_t(0));
                     return b.esimd_bitspread(b.getBitBlockWidth(), p.fw, scalarOpr0);
                 },
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     Value *scalarOpr0 = SafeExtractElement(b, p.fw, p.opr[0], uint64_t(0));
                     Value *expectedBlock = Constant::getNullValue(p.vTy);
                     for (unsigned i = 0; (i < p.fn) && (i < p.fw); ++i) {
                         expectedBlock =
                             SafeInsertElement(b, p.fw, expectedBlock, b.CreateAnd(b.CreateLShr(scalarOpr0, i), 1), i);
                     }
                     return expectedBlock;
                 }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_add", "x0, x1", "y[i] <- x0[i] + x1[i]",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_add(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateAdd(p.opr[0], p.opr[1]);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_sub", "x0, x1", "y[i] <- x0[i] - x1[i]",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_sub(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateSub(p.opr[0], p.opr[1]);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_mult", "x0, x1", "y[i] <- x0[i] * x1[i]",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_mult(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateMul(p.opr[0], p.opr[1]);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_eq", "x0, x1", "y[i] <- -(x0[i] == x1[i])",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_eq(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateSExt(b.CreateICmpEQ(p.opr[0], p.opr[1]), p.fTy);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_ne", "x0, x1", "y[i] <- -(x0[i] != x1[i])",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_ne(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateSExt(b.CreateICmpNE(p.opr[0], p.opr[1]), p.fTy);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_gt", "x0, x1",
                     "y[i] <- -(x0[i] > x1[i]) (signed comparison)",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_gt(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateSExt(b.CreateICmpSGT(p.opr[0], p.opr[1]), p.fTy);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_ugt", "x0, x1",
                     "y[i] <- -(x0[i] > x1[i]) (unsigned comparison)",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_ugt(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateSExt(b.CreateICmpUGT(p.opr[0], p.opr[1]), p.fTy);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_ge", "x0, x1",
                     "y[i] <- -(x0[i] >= x1[i]) (signed comparison)",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_ge(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateSExt(b.CreateICmpSGE(p.opr[0], p.opr[1]), p.fTy);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_uge", "x0, x1",
                     "y[i] <- -(x0[i] >= x1[i]) (unsigned comparison)",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_uge(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateSExt(b.CreateICmpUGE(p.opr[0], p.opr[1]), p.fTy);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_lt", "x0, x1",
                     "y[i] <- -(x0[i] < x1[i]) (signed comparison)",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_lt(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateSExt(b.CreateICmpSLT(p.opr[0], p.opr[1]), p.fTy);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_le", "x0, x1",
                     "y[i] <- -(x0[i] <= x1[i]) (signed comparison)",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_le(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateSExt(b.CreateICmpSLE(p.opr[0], p.opr[1]), p.fTy);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_ult", "x0, x1",
                     "y[i] <- -(x0[i] < x1[i]) (unsigned comparison)",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_ult(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateSExt(b.CreateICmpULT(p.opr[0], p.opr[1]), p.fTy);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_ule", "x0, x1",
                     "y[i] <- -(x0[i] <= x1[i]) (unsigned comparison)",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_ule(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateSExt(b.CreateICmpULE(p.opr[0], p.opr[1]), p.fTy);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_max", "x0, x1",
                     "y[i] <- x0[i] > x1[i] ? x0 : x1 (signed comparison)",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_max(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateSelect(b.CreateICmpSGT(p.opr[0], p.opr[1]), p.opr[0], p.opr[1]);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_min", "x0, x1",
                     "y[i] <- x0[i] < x1[i] ? x0 : x1 (signed comparison)",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_min(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateSelect(b.CreateICmpSLT(p.opr[0], p.opr[1]), p.opr[0], p.opr[1]);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_umax", "x0, x1",
                     "y[i] <- x0[i] > x1[i] ? x0 : x1 (unsigned comparison)",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_umax(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateSelect(b.CreateICmpUGT(p.opr[0], p.opr[1]), p.opr[0], p.opr[1]);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_umin", "x0, x1",
                     "y[i] <- x0[i] < x1[i] ? x0 : x1 (unsigned comparison)",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_umin(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateSelect(b.CreateICmpULT(p.opr[0], p.opr[1]), p.opr[0], p.opr[1]);
                     }>(),
    scalarCheckEntry<TernaryOpConfig, ExpectedType::fieldVector, "simd_if", "x0, x1, x2",
                     "y[i] <- x0[i][fw-1] ? x1[i] : x2[i]",
                     [](KernelBuilder &b, const TernaryOpConfig &c, const TernaryOpConfig::Params &p) {
                         return b.simd_if(p.fw, p.opr[0], p.opr[1], p.opr[2]);
                     },
                     [](KernelBuilder &b, const TernaryOpConfig &c, const TernaryOpConfig::Params &p) {
                         // simd_if checks the top bit set, which is equivalent to signed < 0
                         return b.CreateSelect(b.CreateICmpSLT(p.opr[0], b.getIntN(p.fw, 0)), p.opr[1], p.opr[2]);
                     }>(),
    genericEntry<ImmedBinOpConfig, ExpectedType::bitBlock, "simd_binary", "x0, x1, imm",
                 "y[i][j] <- imm[ (x0[i][j] << 1) | x1[i][j] ]; use <imm> as a truth table",
                 [](KernelBuilder &b, const ImmedBinOpConfig &c, const ImmedBinOpConfig::Params &p) {
                     // Caution! Parameter order is MSB first, i.e. opr[0] = bit_1 and opr[1] = bit_0
                     return b.simd_binary(p.immed & 0x0F, p.opr[0], p.opr[1]);
                 },
                 [](KernelBuilder &b, const ImmedBinOpConfig &c, const ImmedBinOpConfig::Params &p) {
                     Value *zero = b.getIntN(b.getLaneWidth(), 0);
                     Value *one = b.getIntN(b.getLaneWidth(), 1);
                     Value *bitOpr0 = b.bitCast(p.opr[0]);
                     Value *bitOpr1 = b.bitCast(p.opr[1]);
                     Value *funcBits = b.getIntN(b.getLaneWidth(), p.immed);
                     Value *expectedBitBlock = Constant::getNullValue(b.getBitBlockType());
                     for (unsigned i = 0; i < b.getBitBlockWidth() / b.getLaneWidth(); ++i) {
                         Value *laneResult = zero;
                         Value *x0i = b.CreateExtractElement(bitOpr0, i);
                         Value *x1i = b.CreateExtractElement(bitOpr1, i);
                         for (unsigned j = 0; j < b.getLaneWidth(); ++j) {
                             Value *x0ij = b.CreateAnd(b.CreateLShr(x0i, j), one);
                             Value *x1ij = b.CreateAnd(b.CreateLShr(x1i, j), one);
                             Value *funcIndex = b.CreateOr(b.CreateShl(x0ij, one), x1ij);
                             Value *yij = b.CreateAnd(b.CreateLShr(funcBits, funcIndex), one);
                             laneResult = b.CreateOr(laneResult, b.CreateShl(yij, j));
                         }
                         expectedBitBlock = b.CreateInsertElement(expectedBitBlock, laneResult, i);
                     }
                     return expectedBitBlock;
                 }>(),
    genericEntry<ImmedTernOpConfig, ExpectedType::bitBlock, "simd_ternary", "x0, x1, x2, imm",
                 "y[i][j] <- imm[ (x0[i][j] << 2) | (x1[i][j] << 1) | x2 ]; use <imm> as a truth table",
                 [](KernelBuilder &b, const ImmedTernOpConfig &c, const ImmedTernOpConfig::Params &p) {
                     // Caution! Parameter order is MSB first, i.e. opr[0] = bit_2 and opr[2] = bit_0
                     return b.simd_ternary(p.immed & 0xFF, p.opr[0], p.opr[1], p.opr[2]);
                 },
                 [](KernelBuilder &b, const ImmedTernOpConfig &c, const ImmedTernOpConfig::Params &p) {
                     Value *zero = b.getIntN(b.getLaneWidth(), 0);
                     Value *one = b.getIntN(b.getLaneWidth(), 1);
                     Value *two = b.getIntN(b.getLaneWidth(), 2);
                     Value *bitOpr0 = b.bitCast(p.opr[0]);
                     Value *bitOpr1 = b.bitCast(p.opr[1]);
                     Value *bitOpr2 = b.bitCast(p.opr[2]);
                     Value *funcBits = b.getIntN(b.getLaneWidth(), p.immed);
                     Value *expectedBitBlock = Constant::getNullValue(b.getBitBlockType());
                     for (unsigned i = 0; i < b.getBitBlockWidth() / b.getLaneWidth(); ++i) {
                         Value *laneResult = zero;
                         Value *x0i = b.CreateExtractElement(bitOpr0, i);
                         Value *x1i = b.CreateExtractElement(bitOpr1, i);
                         Value *x2i = b.CreateExtractElement(bitOpr2, i);
                         for (unsigned j = 0; j < b.getLaneWidth(); ++j) {
                             Value *x0ij = b.CreateAnd(b.CreateLShr(x0i, j), one);
                             Value *x1ij = b.CreateAnd(b.CreateLShr(x1i, j), one);
                             Value *x2ij = b.CreateAnd(b.CreateLShr(x2i, j), one);
                             Value *funcIndex =
                                 b.CreateOr(b.CreateOr(b.CreateShl(x0ij, two), b.CreateShl(x1ij, one)), x2ij);
                             Value *yij = b.CreateAnd(b.CreateLShr(funcBits, funcIndex), one);
                             laneResult = b.CreateOr(laneResult, b.CreateShl(yij, j));
                         }
                         expectedBitBlock = b.CreateInsertElement(expectedBitBlock, laneResult, i);
                     }
                     return expectedBitBlock;
                 }>(),
    scalarCheckEntry<ImmedUnOpConfig, ExpectedType::fieldVector, "simd_slli", "x0, imm", "y[i] <- x[0] << imm",
                     [](KernelBuilder &b, const ImmedUnOpConfig &c, const ImmedUnOpConfig::Params &p) {
                         return b.simd_slli(p.fw, p.opr[0], p.immed & (p.fw - 1));
                     },
                     [](KernelBuilder &b, const ImmedUnOpConfig &c, const ImmedUnOpConfig::Params &p) {
                         return b.CreateShl(p.opr[0], p.immed & (p.fw - 1));
                     }>(),
    scalarCheckEntry<ImmedUnOpConfig, ExpectedType::fieldVector, "simd_srli", "x0, imm",
                     "y[i] <- x[0] >> imm (zero extend x0)",
                     [](KernelBuilder &b, const ImmedUnOpConfig &c, const ImmedUnOpConfig::Params &p) {
                         return b.simd_srli(p.fw, p.opr[0], p.immed & (p.fw - 1));
                     },
                     [](KernelBuilder &b, const ImmedUnOpConfig &c, const ImmedUnOpConfig::Params &p) {
                         return b.CreateLShr(p.opr[0], p.immed & (p.fw - 1));
                     }>(),
    scalarCheckEntry<ImmedUnOpConfig, ExpectedType::fieldVector, "simd_srai", "x0, imm",
                     "y[i] <- x[0] >> imm (sign extend x0)",
                     [](KernelBuilder &b, const ImmedUnOpConfig &c, const ImmedUnOpConfig::Params &p) {
                         return b.simd_srai(p.fw, p.opr[0], p.immed & (p.fw - 1));
                     },
                     [](KernelBuilder &b, const ImmedUnOpConfig &c, const ImmedUnOpConfig::Params &p) {
                         return b.CreateAShr(p.opr[0], p.immed & (p.fw - 1));
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_sllv", "x0, x1", "y[i] <- x[0] << x1[i]",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_sllv(p.fw, p.opr[0], SafeURem(b, p.fw, p.opr[1], p.fw));
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateShl(p.opr[0], b.CreateAnd(p.opr[1], b.getIntN(p.fw, p.fw - 1)));
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_srlv", "x0, x1",
                     "y[i] <- x[0] << x1[i] (zero extend x0)",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_srlv(p.fw, p.opr[0], SafeURem(b, p.fw, p.opr[1], p.fw));
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.CreateLShr(p.opr[0], b.CreateAnd(p.opr[1], b.getIntN(p.fw, p.fw - 1)));
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_rotl", "x0, x1", "",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_rotl(p.fw, p.opr[0], SafeURem(b, p.fw, p.opr[1], p.fw));
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         Constant *fwConst = ConstantInt::get(p.fTy, p.fw);
                         Constant *fwMaskConst = ConstantInt::get(p.fTy, p.fw - 1);
                         Value *shl = b.CreateShl(p.opr[0], b.CreateAnd(p.opr[1], fwMaskConst));
                         Value *shr = b.CreateLShr(p.opr[0], b.CreateAnd(b.CreateSub(fwConst, p.opr[1]), fwMaskConst));
                         return b.CreateOr(shl, shr);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_rotr", "x0, x1", "",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_rotr(p.fw, p.opr[0], SafeURem(b, p.fw, p.opr[1], p.fw));
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         Constant *fwConst = b.getIntN(p.fw, p.fw);
                         Constant *fwMaskConst = b.getIntN(p.fw, p.fw - 1);
                         Value *shl = b.CreateShl(p.opr[0], b.CreateAnd(b.CreateSub(fwConst, p.opr[1]), fwMaskConst));
                         Value *shr = b.CreateLShr(p.opr[0], b.CreateAnd(p.opr[1], fwMaskConst));
                         return b.CreateOr(shl, shr);
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_pext", "x0, x1", "",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_pext(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         Constant *zeroConst = b.getIntN(p.fw, 0);
                         Constant *oneConst = b.getIntN(p.fw, 1);
                         Value *expected = zeroConst;
                         Value *out_bit = oneConst;
                         for (unsigned i = 0; i < p.fw; i++) {
                             Value *i_bit = b.getIntN(p.fw, 1LL << i);
                             Value *operand_i_isSet = b.CreateICmpEQ(b.CreateAnd(p.opr[0], i_bit), i_bit);
                             Value *mask_i_isSet = b.CreateICmpEQ(b.CreateAnd(p.opr[1], i_bit), i_bit);
                             expected = b.CreateSelect(b.CreateAnd(operand_i_isSet, mask_i_isSet),
                                                       b.CreateOr(expected, out_bit), expected);
                             out_bit = b.CreateSelect(mask_i_isSet, b.CreateAdd(out_bit, out_bit), out_bit);
                         }
                         return expected;
                     }>(),
    scalarCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "simd_pdep", "x0, x1", "",
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         return b.simd_pdep(p.fw, p.opr[0], p.opr[1]);
                     },
                     [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                         Constant *zeroConst = ConstantInt::getNullValue(p.fTy);
                         Constant *oneConst = ConstantInt::get(p.fTy, 1);
                         Value *expected = zeroConst;
                         Value *shft = zeroConst;
                         Value *select_bit = oneConst;
                         for (unsigned i = 0; i < p.fw; i++) {
                             expected = b.CreateOr(
                                 b.CreateAnd(p.opr[1], b.CreateShl(b.CreateAnd(p.opr[0], select_bit), shft)), expected);
                             Value *i_bit = b.getIntN(p.fw, 1LL << i);
                             Value *mask_i_isSet = b.CreateICmpEQ(b.CreateAnd(p.opr[1], i_bit), i_bit);
                             select_bit = b.CreateSelect(mask_i_isSet, b.CreateAdd(select_bit, select_bit), select_bit);
                             shft = b.CreateSelect(mask_i_isSet, shft, b.CreateAdd(shft, oneConst));
                         }
                         return expected;
                     }>(),
    horizontalStoreCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "esimd_mergeh", "x0, x1", "",
                              [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                                  return b.esimd_mergeh(p.fw, p.opr[0], p.opr[1]);
                              },
                              [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p,
                                 Value *expectedBlock, unsigned i) {
                                  if (i >= p.fn / 2) {
                                      expectedBlock =
                                          SafeInsertElement(b, p.fw, expectedBlock, p.opr[0], 2 * (i - p.fn / 2));
                                      expectedBlock = b.bitCast(
                                          SafeInsertElement(b, p.fw, expectedBlock, p.opr[1], 2 * (i - p.fn / 2) + 1));
                                  }
                                  return expectedBlock;
                              }>(),
    horizontalStoreCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "esimd_mergel", "x0, x1", "",
                              [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                                  return b.esimd_mergel(p.fw, p.opr[0], p.opr[1]);
                              },
                              [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p,
                                 Value *expectedBlock, unsigned i) {
                                  if (i < p.fn / 2) {
                                      expectedBlock = SafeInsertElement(b, p.fw, expectedBlock, p.opr[0], 2 * i);
                                      expectedBlock =
                                          b.bitCast(SafeInsertElement(b, p.fw, expectedBlock, p.opr[1], 2 * i + 1));
                                  }
                                  return expectedBlock;
                              }>(),
    horizontalStoreCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "hsimd_packh", "x0, x1", "",
                              [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                                  return b.hsimd_packh(p.fw, p.opr[0], p.opr[1]);
                              },
                              [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p,
                                 Value *expectedBlock, unsigned i) {
                                  if (p.fw == 1) {
                                      return expectedBlock;
                                  }
                                  expectedBlock = SafeInsertElement(
                                      b, p.fw / 2, expectedBlock,
                                      b.CreateTrunc(b.CreateLShr(p.opr[0], p.fw / 2), b.getIntNTy(p.fw / 2)), i);
                                  expectedBlock = SafeInsertElement(
                                      b, p.fw / 2, expectedBlock,
                                      b.CreateTrunc(b.CreateLShr(p.opr[1], p.fw / 2), b.getIntNTy(p.fw / 2)), i + p.fn);
                                  return expectedBlock;
                              }>(),
    horizontalStoreCheckEntry<BinaryOpConfig, ExpectedType::fieldVector, "hsimd_packl", "x0, x1", "",
                              [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                                  return b.hsimd_packl(p.fw, p.opr[0], p.opr[1]);
                              },
                              [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p,
                                 Value *expectedBlock, unsigned i) {
                                  if (p.fw == 1) {
                                      return expectedBlock;
                                  }
                                  expectedBlock = SafeInsertElement(b, p.fw / 2, expectedBlock,
                                                                    b.CreateTrunc(p.opr[0], b.getIntNTy(p.fw / 2)), i);
                                  expectedBlock =
                                      SafeInsertElement(b, p.fw / 2, expectedBlock,
                                                        b.CreateTrunc(p.opr[1], b.getIntNTy(p.fw / 2)), i + p.fn);
                                  return expectedBlock;
                              }>(),
    genericEntry<BinaryOpConfig, ExpectedType::fieldVector, "hsimd_packus", "x0, x1", "",
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     Value *newOpr0 = b.simd_srai(p.fw, p.opr[0], p.fw / 2 - 1);
                     Value *newOpr1 = b.simd_srai(p.fw, p.opr[1], p.fw / 2 - 1);
                     return b.hsimd_packus(p.fw, newOpr0, newOpr1);
                 },
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     BinaryOpConfig::Params newP = p;
                     newP.opr[0] = b.simd_srai(p.fw, p.opr[0], p.fw / 2 - 1);
                     newP.opr[1] = b.simd_srai(p.fw, p.opr[1], p.fw / 2 - 1);
                     auto expectedF = wrapHorizontalStore<BinaryOpConfig>([=](KernelBuilder &b, const BinaryOpConfig &c,
                                                                              const BinaryOpConfig::Params &p,
                                                                              Value *expectedBlock, unsigned i) {
                         Value *zeroes = ConstantInt::getNullValue(p.fTy);
                         Value *newOpr0 = b.CreateSelect(b.CreateICmpSLT(p.opr[0], zeroes), zeroes, p.opr[0]);
                         Value *newOpr1 = b.CreateSelect(b.CreateICmpSLT(p.opr[1], zeroes), zeroes, p.opr[1]);
                         Value *testVal = ConstantInt::get(b.getContext(), APInt::getLowBitsSet(p.fw, p.fw / 2));
                         newOpr0 = b.CreateSelect(b.CreateICmpSGT(newOpr0, testVal), testVal, newOpr0);
                         newOpr1 = b.CreateSelect(b.CreateICmpSGT(newOpr1, testVal), testVal, newOpr1);
                         newOpr0 = b.CreateTrunc(newOpr0, b.getIntNTy(p.fw / 2));
                         newOpr1 = b.CreateTrunc(newOpr1, b.getIntNTy(p.fw / 2));
                         expectedBlock = b.mvmd_insert(p.fw / 2, expectedBlock, newOpr0, i);
                         expectedBlock = b.bitCast(b.mvmd_insert(p.fw / 2, expectedBlock, newOpr1, p.fn + i));
                         return expectedBlock;
                     });
                     return expectedF(b, c, newP);
                 }>(),
    genericEntry<BinaryOpConfig, ExpectedType::fieldVector, "hsimd_packss", "x0, x1", "",
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     Value *newOpr0 = b.simd_srai(p.fw, p.opr[0], p.fw / 2 - 1);
                     Value *newOpr1 = b.simd_srai(p.fw, p.opr[1], p.fw / 2 - 1);
                     return b.hsimd_packss(p.fw, newOpr0, newOpr1);
                 },
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     BinaryOpConfig::Params newP = p;
                     newP.opr[0] = b.simd_srai(p.fw, p.opr[0], p.fw / 2 - 1);
                     newP.opr[1] = b.simd_srai(p.fw, p.opr[1], p.fw / 2 - 1);
                     auto expectedF = wrapHorizontalStore<BinaryOpConfig>([=](KernelBuilder &b, const BinaryOpConfig &c,
                                                                              const BinaryOpConfig::Params &p,
                                                                              Value *expectedBlock, unsigned i) {
                         Value * hiSat = ConstantInt::get(b.getContext(), APInt::getLowBitsSet(p.fw, p.fw / 2 - 1));
                         Value *newOpr0 = b.CreateSelect(b.CreateICmpSLE(p.opr[0], hiSat), p.opr[0], hiSat);
                         Value *newOpr1 = b.CreateSelect(b.CreateICmpSLE(p.opr[1], hiSat), p.opr[1], hiSat);
                         Value * loSat = b.CreateNot(hiSat);
                         newOpr0 = b.CreateSelect(b.CreateICmpSGE(newOpr0, loSat), newOpr0, loSat);
                         newOpr1 = b.CreateSelect(b.CreateICmpSGE(newOpr1, loSat), newOpr1, loSat);
                         newOpr0 = b.CreateTrunc(newOpr0, b.getIntNTy(p.fw / 2));
                         newOpr1 = b.CreateTrunc(newOpr1, b.getIntNTy(p.fw / 2));
                         expectedBlock = b.mvmd_insert(p.fw / 2, expectedBlock, newOpr0, i);
                         expectedBlock = b.bitCast(b.mvmd_insert(p.fw / 2, expectedBlock, newOpr1, p.fn + i));
                         return expectedBlock;
                     });
                     return expectedF(b, c, newP);
                 }>(),
    genericEntry<ImmedUnOpConfig, ExpectedType::field, "mvmd_extract", "x0, imm", "y[0] <- Extract element x0[imm]",
                 [](KernelBuilder &b, const ImmedUnOpConfig &c, const ImmedUnOpConfig::Params &p) {
                     return b.mvmd_extract(p.fw, p.opr[0], p.immed % p.fn);
                 },
                 [](KernelBuilder &b, const ImmedUnOpConfig &c, const ImmedUnOpConfig::Params &p) {
                     return SafeExtractElement(b, p.fw, p.opr[0], p.immed % p.fn);
                 }>(),
    genericEntry<ImmedBinOpConfig, ExpectedType::fieldVector, "mvmd_insert", "x0, x1, imm",
                 "y[i] <- i == imm ? x1[0] : x0[i]; i.e. insert",
                 [](KernelBuilder &b, const ImmedBinOpConfig &c, const ImmedBinOpConfig::Params &p) {
                     Value *scalarOpr1 = SafeExtractElement(b, p.fw, p.opr[1], uint64_t(0));
                     return b.mvmd_insert(p.fw, p.opr[0], scalarOpr1, p.immed % p.fn);
                 },
                 [](KernelBuilder &b, const ImmedBinOpConfig &c, const ImmedBinOpConfig::Params &p) {
                     Value *scalarOpr1 = SafeExtractElement(b, p.fw, p.opr[1], uint64_t(0));
                     return SafeInsertElement(b, p.fw, p.opr[0], scalarOpr1, p.immed % p.fn);
                 }>(),
    genericEntry<BinaryOpConfig, ExpectedType::fieldVector, "mvmd_sll", "x0, x1",
                 "y[i] <- x0[i - x1[0]] or 0; shuffle elements left",
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     Value *scalarOpr1 = SafeExtractElement(b, p.fw, p.opr[1], uint64_t(0));
                     Value *shift = b.CreateAnd(scalarOpr1, p.fn - 1);
                     return b.mvmd_sll(p.fw, p.opr[0], shift);
                 },
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     Value *expectedBlock = Constant::getNullValue(p.vTy);
                     Value *scalarOpr1 = SafeExtractElement(b, p.fw, p.opr[1], uint64_t(0));
                     Value *shift = b.CreateZExtOrTrunc(b.CreateAnd(scalarOpr1, p.fn - 1), b.getInt64Ty());
                     Value *zero = b.getIntN(p.fw, 0);
                     for (unsigned i = 0; i < p.fn; ++i) {
                         Value *index = b.getInt64(i);
                         Value *shiftedElement = SafeExtractElement(b, p.fw, p.opr[0], b.CreateSub(index, shift));
                         Value *selection = b.CreateSelect(b.CreateICmpUGE(index, shift), shiftedElement, zero);
                         expectedBlock = SafeInsertElement(b, p.fw, expectedBlock, selection, i);
                     }
                     return expectedBlock;
                 }>(),
    genericEntry<BinaryOpConfig, ExpectedType::fieldVector, "mvmd_srl", "x0, x1",
                 "y[i] <- x0[i + x1[0]] or 0; shuffle elements right",
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     Value *scalarOpr1 = SafeExtractElement(b, p.fw, p.opr[1], uint64_t(0));
                     Value *shift = b.CreateAnd(scalarOpr1, p.fn - 1);
                     return b.mvmd_srl(p.fw, p.opr[0], shift);
                 },
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     Value *expectedBlock = Constant::getNullValue(p.vTy);
                     Value *scalarOpr1 = SafeExtractElement(b, p.fw, p.opr[1], uint64_t(0));
                     Value *shift = b.CreateZExtOrTrunc(b.CreateAnd(scalarOpr1, p.fn - 1), b.getInt64Ty());
                     Value *zero = b.getIntN(p.fw, 0);
                     for (unsigned i = 0; i < p.fn; ++i) {
                         Value *index = b.getInt64(i);
                         Value *shiftedElement = SafeExtractElement(b, p.fw, p.opr[0], b.CreateAdd(index, shift));
                         Value *selection =
                             b.CreateSelect(b.CreateICmpULT(shift, b.getInt64(p.fn - i)), shiftedElement, zero);
                         expectedBlock = SafeInsertElement(b, p.fw, expectedBlock, selection, i);
                     }
                     return expectedBlock;
                 }>(),
    genericEntry<ImmedUnOpConfig, ExpectedType::fieldVector, "mvmd_slli", "x0, imm",
                 "y[i] <- x0[i - imm], or 0; shuffle elements left",
                 [](KernelBuilder &b, const ImmedUnOpConfig &c, const ImmedUnOpConfig::Params &p) {
                     return b.mvmd_slli(p.fw, p.opr[0], p.immed % p.fn);
                 },
                 [](KernelBuilder &b, const ImmedUnOpConfig &c, const ImmedUnOpConfig::Params &p) {
                     Value *expectedBlock = Constant::getNullValue(p.vTy);
                     for (unsigned i = 0; i < p.fn; ++i) {
                         if (i >= p.immed) {
                             Value *selection = SafeExtractElement(b, p.fw, p.opr[0], i - p.immed);
                             expectedBlock = SafeInsertElement(b, p.fw, expectedBlock, selection, i);
                         }
                     }
                     return expectedBlock;
                 }>(),
    genericEntry<ImmedUnOpConfig, ExpectedType::fieldVector, "mvmd_srli", "x0, imm",
                 "y[i] <- x0[i + imm], or 0; shuffle elements right",
                 [](KernelBuilder &b, const ImmedUnOpConfig &c, const ImmedUnOpConfig::Params &p) {
                     return b.mvmd_srli(p.fw, p.opr[0], p.immed % p.fn);
                 },
                 [](KernelBuilder &b, const ImmedUnOpConfig &c, const ImmedUnOpConfig::Params &p) {
                     Value *expectedBlock = Constant::getNullValue(p.vTy);
                     for (unsigned i = 0; i < p.fn; ++i) {
                         if (i + p.immed < p.fn) {
                             Value *selection = SafeExtractElement(b, p.fw, p.opr[0], i + p.immed);
                             expectedBlock = SafeInsertElement(b, p.fw, expectedBlock, selection, i);
                         }
                     }
                     return expectedBlock;
                 }>(),

    genericEntry<ImmedBinOpConfig, ExpectedType::fieldVector, "mvmd_dslli", "x0, x1, imm",
                 "y[i] <- x0[i - imm], or x1[i + vl - imm]; shuffle elements left",
                 [](KernelBuilder &b, const ImmedBinOpConfig &c, const ImmedBinOpConfig::Params &p) {
                     return b.mvmd_dslli(p.fw, p.opr[0], p.opr[1], p.immed);
                 },
                 [](KernelBuilder &b, const ImmedBinOpConfig &c, const ImmedBinOpConfig::Params &p) {
                     Value *expectedBlock = Constant::getNullValue(p.vTy);
                     for (unsigned i = 0; i < p.fn; i++) {
                         Value *elt = nullptr;
                         if (i < p.immed)
                             elt = SafeExtractElement(b, p.fw, p.opr[1], p.fn - p.immed + i);
                         else
                             elt = SafeExtractElement(b, p.fw, p.opr[0], i - p.immed);
                         expectedBlock = SafeInsertElement(b, p.fw, expectedBlock, elt, i);
                     }
                     return expectedBlock;
                 }>(),
    genericEntry<BinaryOpConfig, ExpectedType::fieldVector, "mvmd_shuffle", "x0, x1", "y[i] <- x0[x1[i] mod vl]",
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     return b.mvmd_shuffle(p.fw, p.opr[0], p.opr[1], IDISA::ShuffleMode::TruncateIndex);
                 },
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     Value *expectedBlock = Constant::getNullValue(p.vTy);
                     for (unsigned i = 0; i < p.fn; i++) {
                         Value *idx_field = SafeExtractElement(b, p.fw, p.opr[1], i);
                         Value *idx = b.CreateURem(idx_field, ConstantInt::get(p.fTy, p.fn));
                         Value *elt = SafeExtractElement(b, p.fw, p.opr[0], b.CreateZExtOrTrunc(idx, p.i64Ty));
                         // ShuffleMode::TruncateIndex behaviour
                         expectedBlock = SafeInsertElement(b, p.fw, expectedBlock, elt, i);
                     }
                     return expectedBlock;
                 }>(),
    genericEntry<BinaryOpConfig, ExpectedType::fieldVector, "mvmd_shuffle:over", "x0, x1",
                 "y[i] <- x0[x1[i] if i < vl else 0]",
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     return b.mvmd_shuffle(p.fw, p.opr[0], p.opr[1], IDISA::ShuffleMode::ZeroOnIndexOver);
                 },
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     Constant *fieldLimit = ConstantInt::get(p.fTy, p.fn);
                     Value *expectedBlock = Constant::getNullValue(p.vTy);
                     for (unsigned i = 0; i < p.fn; i++) {
                         Value *idx_field = SafeExtractElement(b, p.fw, p.opr[1], i);
                         Value *idx = b.CreateURem(idx_field, ConstantInt::get(p.fTy, p.fn));
                         Value *elt = SafeExtractElement(b, p.fw, p.opr[0], b.CreateZExtOrTrunc(idx, p.i64Ty));
                         // ShuffleMode::ZeroOnIndexOver behaviour
                         elt = b.CreateSelect(b.CreateICmpUGE(idx_field, fieldLimit), ConstantInt::getNullValue(p.fTy),
                                              elt);
                         expectedBlock = SafeInsertElement(b, p.fw, expectedBlock, elt, i);
                     }
                     return expectedBlock;
                 }>(),
    genericEntry<BinaryOpConfig, ExpectedType::fieldVector, "mvmd_shuffle:highbit", "x0, x1",
                 "y[i] <- x0[x1[i] if i & (1 << (fw - 1)) else 0]",
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     return b.mvmd_shuffle(p.fw, p.opr[0], p.opr[1], IDISA::ShuffleMode::ZeroOnHighIndexBit);
                 },
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     Value *expectedBlock = Constant::getNullValue(p.vTy);
                     for (unsigned i = 0; i < p.fn; i++) {
                         Value *idx_field = SafeExtractElement(b, p.fw, p.opr[1], i);
                         Value *idx = b.CreateURem(idx_field, ConstantInt::get(p.fTy, p.fn));
                         Value *elt = SafeExtractElement(b, p.fw, p.opr[0], b.CreateZExtOrTrunc(idx, p.i64Ty));
                         // ShuffleMode::ZeroOnHighIndexBit behaviour
                         elt = b.CreateSelect(b.CreateICmpSLT(idx_field, ConstantInt::getNullValue(p.fTy)),
                                              ConstantInt::getNullValue(p.fTy), elt);
                         expectedBlock = SafeInsertElement(b, p.fw, expectedBlock, elt, i);
                     }
                     return expectedBlock;
                 }>(),
    genericEntry<BinaryOpConfig, ExpectedType::fieldVector, "mvmd_compress", "x0, x1", "",
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     // Real callers (e.g. deletion.cpp) pass a scalar bitmask built with
                     // hsimd_signmask, not a raw data block. Derive a realistic one from
                     // p.opr[1] here so this test actually matches production usage.
                     Value *scalarMask = b.hsimd_signmask(p.fw, p.opr[1]);
                     return b.mvmd_compress(p.fw, p.opr[0], scalarMask);
                 },
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     // Match the scalar bitmask built in IdisaBinaryOpTestKernel above.
                     Value *scalarMask = b.hsimd_signmask(p.fw, p.opr[1]);
                     Type *maskTy = scalarMask->getType();
                     // For each input field i, precompute whether it's selected and its
                     // rank (how many selected fields come before it) - this is the
                     // output slot it should land in if selected.
                     vector<Value *> isSelected(p.fn);
                     vector<Value *> rank(p.fn);
                     Value *runningCount = ConstantInt::get(p.i32Ty, 0);
                     Value *expectedBlock = Constant::getNullValue(p.vTy);
                     for (unsigned i = 0; i < p.fn; i++) {
                         Value *bit = b.CreateAnd(b.CreateLShr(scalarMask, ConstantInt::get(maskTy, i)),
                                                  ConstantInt::get(maskTy, 1));
                         isSelected[i] = b.CreateICmpNE(bit, ConstantInt::get(maskTy, 0));
                         rank[i] = runningCount;
                         runningCount = b.CreateAdd(runningCount, b.CreateZExt(isSelected[i], p.i32Ty));
                     }
                     // For each output slot j, find the (at most one) selected input
                     // field whose rank equals j, and place its value there.
                     for (unsigned j = 0; j < p.fn; j++) {
                         Value *chosen = ConstantInt::get(p.fTy, 0);
                         for (unsigned i = 0; i < p.fn; i++) {
                             Value *matches =
                                 b.CreateAnd(isSelected[i], b.CreateICmpEQ(rank[i], ConstantInt::get(p.i32Ty, j)));
                             Value *elt = SafeExtractElement(b, p.fw, p.opr[0], i);
                             chosen = b.CreateSelect(matches, elt, chosen);
                         }
                         expectedBlock = SafeInsertElement(b, p.fw, expectedBlock, chosen, j);
                     }
                     return expectedBlock;
                 }>(),
    genericEntry<BinaryOpConfig, ExpectedType::fieldVector, "mvmd_expand", "x0, x1", "",
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     Value *scalarMask = b.hsimd_signmask(p.fw, p.opr[1]);
                     return b.mvmd_expand(p.fw, p.opr[0], scalarMask);
                 },
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     // Mirror image of mvmd_compress's reference above: for each output
                     // slot j, if it's selected, it should hold p.opr[0]'s field at
                     // rank(j) (how many selected slots come before j); otherwise 0.
                     Value *scalarMask = b.hsimd_signmask(p.fw, p.opr[1]);
                     Type *maskTy = scalarMask->getType();
                     vector<Value *> isSelected(p.fn);
                     vector<Value *> rank(p.fn);
                     Value *runningCount = ConstantInt::get(p.i32Ty, 0);
                     Value *expectedBlock = Constant::getNullValue(p.vTy);
                     for (unsigned j = 0; j < p.fn; j++) {
                         Value *bit = b.CreateAnd(b.CreateLShr(scalarMask, ConstantInt::get(maskTy, j)),
                                                  ConstantInt::get(maskTy, 1));
                         isSelected[j] = b.CreateICmpNE(bit, ConstantInt::get(maskTy, 0));
                         rank[j] = runningCount;
                         runningCount = b.CreateAdd(runningCount, b.CreateZExt(isSelected[j], p.i32Ty));
                     }
                     for (unsigned j = 0; j < p.fn; j++) {
                         Value *chosen = ConstantInt::get(p.fTy, 0);
                         for (unsigned k = 0; k < p.fn; k++) {
                             Value *matches =
                                 b.CreateAnd(isSelected[j], b.CreateICmpEQ(rank[j], ConstantInt::get(p.i32Ty, k)));
                             Value *elt = SafeExtractElement(b, p.fw, p.opr[0], k);
                             chosen = b.CreateSelect(matches, elt, chosen);
                         }
                         expectedBlock = SafeInsertElement(b, p.fw, expectedBlock, chosen, j);
                     }
                     return expectedBlock;
                 }>(),
    genericEntry<UnaryOpConfig, ExpectedType::bit, "bitblock_any", "x0", "",
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     return b.bitblock_any(p.opr[0]);
                 },
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     Value *bitOpr0 = b.bitCast(p.opr[0]);
                     Value *merge = Constant::getNullValue(b.getIntNTy(b.getLaneWidth()));
                     for (unsigned i = 0; i < b.getBitBlockWidth() / b.getLaneWidth(); ++i) {
                         merge = b.CreateOr(merge, b.CreateExtractElement(bitOpr0, i));
                     }
                     return b.CreateICmpNE(merge, b.getIntN(b.getLaneWidth(), 0));
                 }>(),
    genericEntry<TernaryOpConfig, ExpectedType::bitBlock, "bitblock_add_with_carry.sum", "x0, x1, x2",
                 "y <- x0 + x1 + x2",
                 [](KernelBuilder &b, const TernaryOpConfig &c, const TernaryOpConfig::Params &p) {
                     return b.bitblock_add_with_carry(p.opr[0], p.opr[1], p.opr[2]).first;
                 },
                 [](KernelBuilder &b, const TernaryOpConfig &c, const TernaryOpConfig::Params &p) {
                     Type *bbIntTy = b.getIntNTy(b.getBitBlockWidth());
                     return b.bitCast(b.CreateAdd(
                         b.CreateAdd(b.CreateBitCast(p.opr[0], bbIntTy), b.CreateBitCast(p.opr[1], bbIntTy)),
                         b.CreateZExt(p.opr[2], bbIntTy)));
                 }>(),
    genericEntry<TernaryOpConfig, ExpectedType::bitBlock, "bitblock_add_with_carry.carry", "x0, x1, x2",
                 "y <- (x0 + x1 + x2) >= (1 << bbw)? 1 : 0",
                 [](KernelBuilder &b, const TernaryOpConfig &c, const TernaryOpConfig::Params &p) {
                     return b.bitblock_add_with_carry(p.opr[0], p.opr[1], p.opr[2]).second;
                 },
                 [](KernelBuilder &b, const TernaryOpConfig &c, const TernaryOpConfig::Params &p) {
                     // TODO implement
                     return Constant::getNullValue(b.getBitBlockType());
                 }>(),
    genericEntry<TernaryOpConfig, ExpectedType::bitBlock, "bitblock_subtract_with_borrow.diff", "x0, x1, x2",
                 "y <- x0 - x1 - x2",
                 [](KernelBuilder &b, const TernaryOpConfig &c, const TernaryOpConfig::Params &p) {
                     return b.bitblock_subtract_with_borrow(p.opr[0], p.opr[1], p.opr[2]).first;
                 },
                 [](KernelBuilder &b, const TernaryOpConfig &c, const TernaryOpConfig::Params &p) {
                     // TODO implement
                     return Constant::getNullValue(b.getBitBlockType());
                 }>(),
    genericEntry<TernaryOpConfig, ExpectedType::bitBlock, "bitblock_subtract_with_borrow.borrow", "x0, x1, x2",
                 "y <- (x0 - x1 - x2) < (-1 << (bbw - 1)) ? 1 : 0",
                 [](KernelBuilder &b, const TernaryOpConfig &c, const TernaryOpConfig::Params &p) {
                     return b.bitblock_subtract_with_borrow(p.opr[0], p.opr[1], p.opr[2]).second;
                 },
                 [](KernelBuilder &b, const TernaryOpConfig &c, const TernaryOpConfig::Params &p) {
                     // TODO implement
                     return Constant::getNullValue(b.getBitBlockType());
                 }>(),
    genericEntry<ImmedBinOpConfig, ExpectedType::bitBlock, "bitblock_advance.shiftout", "x0, x1, imm", "",
                 [](KernelBuilder &b, const ImmedBinOpConfig &c, const ImmedBinOpConfig::Params &p) {
                     return b.bitblock_advance(p.opr[0], p.opr[1], p.immed).first;
                 },
                 [](KernelBuilder &b, const ImmedBinOpConfig &c, const ImmedBinOpConfig::Params &p) {
                     // TODO implement
                     return Constant::getNullValue(b.getBitBlockType());
                 }>(),
    genericEntry<ImmedBinOpConfig, ExpectedType::bitBlock, "bitblock_advance.shifted", "x0, x1, imm", "",
                 [](KernelBuilder &b, const ImmedBinOpConfig &c, const ImmedBinOpConfig::Params &p) {
                     return b.bitblock_advance(p.opr[0], p.opr[1], p.immed).second;
                 },
                 [](KernelBuilder &b, const ImmedBinOpConfig &c, const ImmedBinOpConfig::Params &p) {
                     // TODO implement
                     return Constant::getNullValue(b.getBitBlockType());
                 }>(),
    genericEntry<ImmedTernOpConfig, ExpectedType::bitBlock, "bitblock_indexed_advance.shiftout", "x0, x1, x2, imm", "",
                 [](KernelBuilder &b, const ImmedTernOpConfig &c, const ImmedTernOpConfig::Params &p) {
                     return b.bitblock_indexed_advance(p.opr[0], p.opr[1], p.opr[2], p.immed).first;
                 },
                 [](KernelBuilder &b, const ImmedTernOpConfig &c, const ImmedTernOpConfig::Params &p) {
                     // TODO implement
                     return Constant::getNullValue(b.getBitBlockType());
                 }>(),
    genericEntry<ImmedTernOpConfig, ExpectedType::bitBlock, "bitblock_indexed_advance.shifted", "x0, x1, x2, imm", "",
                 [](KernelBuilder &b, const ImmedTernOpConfig &c, const ImmedTernOpConfig::Params &p) {
                     return b.bitblock_indexed_advance(p.opr[0], p.opr[1], p.opr[2], p.immed).second;
                 },
                 [](KernelBuilder &b, const ImmedTernOpConfig &c, const ImmedTernOpConfig::Params &p) {
                     // TODO implement
                     return Constant::getNullValue(b.getBitBlockType());
                 }>(),
    genericEntry<UnaryOpConfig, ExpectedType::bitBlock, "bitblock_mask_from", "x0", "",
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     Value *scalarOpr0 = SafeExtractElement(b, p.fw, p.opr[0], uint64_t(0));
                     Value *maskOpr0 = b.CreateAnd(scalarOpr0, b.getBitBlockWidth() - 1);
                     return b.bitblock_mask_from(maskOpr0);
                 },
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     Value *scalarOpr0 = SafeExtractElement(b, p.fw, p.opr[0], uint64_t(0));
                     Value *maskOpr0 = b.CreateAnd(scalarOpr0, b.getBitBlockWidth() - 1);
                     Value *widenOpr0 = b.CreateZExt(maskOpr0, b.getIntNTy(b.getBitBlockWidth()));
                     return b.bitCast(b.CreateSub(Constant::getNullValue(b.getIntNTy(b.getBitBlockWidth())),
                                                  b.CreateShl(b.getIntN(b.getBitBlockWidth(), 1), widenOpr0)));
                 }>(),
    genericEntry<UnaryOpConfig, ExpectedType::bitBlock, "bitblock_mask_to", "x0", "",
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     Value *scalarOpr0 = SafeExtractElement(b, p.fw, p.opr[0], uint64_t(0));
                     Value *maskOpr0 = b.CreateAnd(scalarOpr0, b.getBitBlockWidth() - 1);
                     return b.bitblock_mask_to(maskOpr0);
                 },
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     Value *scalarOpr0 = SafeExtractElement(b, p.fw, p.opr[0], uint64_t(0));
                     Value *maskOpr0 = b.CreateAnd(scalarOpr0, b.getBitBlockWidth() - 1);
                     Value *widenOpr0 = b.CreateZExt(maskOpr0, b.getIntNTy(b.getBitBlockWidth()));
                     return b.bitCast(b.CreateSub(b.CreateShl(b.getIntN(b.getBitBlockWidth(), 2), widenOpr0),
                                                  b.getIntN(b.getBitBlockWidth(), 1)));
                 }>(),
    genericEntry<UnaryOpConfig, ExpectedType::bitBlock, "bitblock_set_bit", "x0", "",
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     Value *scalarOpr0 = SafeExtractElement(b, p.fw, p.opr[0], uint64_t(0));
                     Value *maskOpr0 = b.CreateAnd(scalarOpr0, b.getBitBlockWidth() - 1);
                     return b.bitblock_set_bit(maskOpr0);
                 },
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     Value *scalarOpr0 = SafeExtractElement(b, p.fw, p.opr[0], uint64_t(0));
                     Value *maskOpr0 = b.CreateAnd(scalarOpr0, b.getBitBlockWidth() - 1);
                     Value *widenOpr0 = b.CreateZExt(maskOpr0, b.getIntNTy(b.getBitBlockWidth()));
                     return b.bitCast(b.CreateShl(b.getIntN(b.getBitBlockWidth(), 1), widenOpr0));
                 }>(),
    genericEntry<UnaryOpConfig, ExpectedType::size, "bitblock_popcount", "x0", "",
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     return b.bitblock_popcount(p.opr[0]);
                 },
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     Value *bitOpr0 = b.bitCast(p.opr[0]);
                     Value *total = b.getSize(0);
                     for (unsigned i = 0; i < b.getBitBlockWidth() / b.getLaneWidth(); ++i) {
                         Value *bitOpr0i = b.CreateExtractElement(bitOpr0, i);
                         total = b.CreateAdd(total, b.CreatePopcount(bitOpr0i));
                     }
                     return total;
                 }>(),
    genericEntry<BinaryOpConfig, ExpectedType::bitBlock, "simd_and", "x0", "",
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     return b.simd_and(p.opr[0], p.opr[1]);
                 },
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     Value *bitOpr0 = b.bitCast(p.opr[0]);
                     Value *bitOpr1 = b.bitCast(p.opr[1]);
                     Value *expectedBitBlock = Constant::getNullValue(b.getBitBlockType());
                     for (unsigned i = 0; i < b.getBitBlockWidth() / b.getLaneWidth(); ++i) {
                         Value *bitOpr0i = b.CreateExtractElement(bitOpr0, i);
                         Value *bitOpr1i = b.CreateExtractElement(bitOpr1, i);
                         expectedBitBlock = b.CreateInsertElement(expectedBitBlock, b.CreateAnd(bitOpr0i, bitOpr1i), i);
                     }
                     return expectedBitBlock;
                 }>(),
    genericEntry<BinaryOpConfig, ExpectedType::bitBlock, "simd_or", "x0", "",
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     return b.simd_or(p.opr[0], p.opr[1]);
                 },
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     Value *bitOpr0 = b.bitCast(p.opr[0]);
                     Value *bitOpr1 = b.bitCast(p.opr[1]);
                     Value *expectedBitBlock = Constant::getNullValue(b.getBitBlockType());
                     for (unsigned i = 0; i < b.getBitBlockWidth() / b.getLaneWidth(); ++i) {
                         Value *bitOpr0i = b.CreateExtractElement(bitOpr0, i);
                         Value *bitOpr1i = b.CreateExtractElement(bitOpr1, i);
                         expectedBitBlock = b.CreateInsertElement(expectedBitBlock, b.CreateOr(bitOpr0i, bitOpr1i), i);
                     }
                     return expectedBitBlock;
                 }>(),
    genericEntry<BinaryOpConfig, ExpectedType::bitBlock, "simd_xor", "x0", "",
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     return b.simd_xor(p.opr[0], p.opr[1]);
                 },
                 [](KernelBuilder &b, const BinaryOpConfig &c, const BinaryOpConfig::Params &p) {
                     Value *bitOpr0 = b.bitCast(p.opr[0]);
                     Value *bitOpr1 = b.bitCast(p.opr[1]);
                     Value *expectedBitBlock = Constant::getNullValue(b.getBitBlockType());
                     for (unsigned i = 0; i < b.getBitBlockWidth() / b.getLaneWidth(); ++i) {
                         Value *bitOpr0i = b.CreateExtractElement(bitOpr0, i);
                         Value *bitOpr1i = b.CreateExtractElement(bitOpr1, i);
                         expectedBitBlock = b.CreateInsertElement(expectedBitBlock, b.CreateXor(bitOpr0i, bitOpr1i), i);
                     }
                     return expectedBitBlock;
                 }>(),
    genericEntry<UnaryOpConfig, ExpectedType::bitBlock, "simd_not", "x0", "",
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     return b.simd_not(p.opr[0]);
                 },
                 [](KernelBuilder &b, const UnaryOpConfig &c, const UnaryOpConfig::Params &p) {
                     Value *bitOpr0 = b.bitCast(p.opr[0]);
                     Value *expectedBitBlock = Constant::getNullValue(b.getBitBlockType());
                     for (unsigned i = 0; i < b.getBitBlockWidth() / b.getLaneWidth(); ++i) {
                         Value *bitOpr0i = b.CreateExtractElement(bitOpr0, i);
                         expectedBitBlock = b.CreateInsertElement(expectedBitBlock, b.CreateNot(bitOpr0i), i);
                     }
                     return expectedBitBlock;
                 }>(),
};

/*
// Need to add to list, still:

    llvm::Value *hsimd_packh_in_lanes(unsigned lanes, unsigned fw, llvm::Value *a, llvm::Value *b)
    llvm::Value *hsimd_packl_in_lanes(unsigned lanes, unsigned fw, llvm::Value *a, llvm::Value *b)

    llvm::Value *hsimd_signmask(unsigned fw, llvm::Value *a)

    llvm::Value *mvmd_dsll(unsigned fw, llvm::Value *a, llvm::Value *b, llvm::Value *shift)

    llvm::Value *mvmd_shuffle2(unsigned fw, llvm::Value *table0, llvm::Value *table1, llvm::Value *index_vector,
                               ShuffleMode m = ShuffleMode::TruncateIndex)
*/

static string supportedOpsString = []() {
    stringstream result;
    result << "\nSUPPORTED OPERATIONS:\n\n";
    for (auto const &entry : allOperations) {
        result << setw(4) << " " << setw(36) << right << entry.name << "  " << setw(16) << left << entry.helpOprs;
        if (entry.helpDesc[0] != 0) {
            result << " - " << entry.helpDesc;
        }
        result << '\n';
    }
    result << '\n';
    return result.str();
}();

cl::extrahelp SupportedOperationsHelp(supportedOpsString);

bool makeOperationConfig(unique_ptr<OperationConfig> &outOpConfig) {
    StringRef opName = OperationName;
    for (auto const &entry : allOperations) {
        if (opName == entry.name) {
            return entry.factory(outOpConfig);
        }
    }
    return OperationName.error("Operation " + opName + " unknown or not implemented yet");
}
