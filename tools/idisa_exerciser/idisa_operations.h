#pragma once

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include "idisa_exerciser.h"

namespace llvm {
class Type;
class Value;
} // namespace llvm

enum class ExpectedType {
    bit,
    field,
    size,
    fieldVector,
    bitBlock,
};

class OperationConfig {
  public:
    struct Params {
        unsigned fw;       // Field width
        unsigned fn;       // Field count (num)
        llvm::Type *fTy;   // Field type (fw-width int)
        llvm::Type *vTy;   // Vector type (fn x fw)
        llvm::Type *i32Ty; // Just handy: int32 type
        llvm::Type *i64Ty; // Just handy: int64 type
    };

    static std::string operandFDIdent(unsigned index) { return std::format("operand{}_fd", index + 1); }
    static std::string operandIdent(unsigned index) { return std::format("operand{}", index + 1); }
    static constexpr const char *testOutputIdent = "test_output";
    static constexpr const char *expectedOutputIdent = "expected_output";
    static constexpr const char *failureCountIdent = "failure_count";
    static constexpr const char *outputFilenameIdent = "output_filename";

    explicit OperationConfig(llvm::StringRef description, llvm::StringRef identifier, unsigned fieldWidth)
        : mDescription(description.str() + " x i" + std::to_string(fieldWidth)),
          mIdentifier(identifier.str() + "_" + std::to_string(fieldWidth)), mFieldWidth(fieldWidth) {}
    virtual ~OperationConfig();

    // String describing how the operation is configured, for logging etc.
    llvm::StringRef getDescription() const { return mDescription; }

    // A more compact name meant to use for e.g. the kernel name
    llvm::StringRef getIdentifier() const { return mIdentifier; }

    unsigned getFieldWidth() const { return mFieldWidth; }

    virtual void constructPipeline(CPUDriver &driver) = 0;
    virtual bool configurePipelineFromArgs(llvm::cl::list<std::string> &args, bool doChecks = true);
    virtual void compilePipeline() = 0;
    virtual size_t executePipeline() = 0;

    kernel::PipelineBuilder &getPipelineBuilder() {
        assert(mPipelineBuilder);
        return *mPipelineBuilder;
    }
    kernel::StreamSet *getOutputStream() const { return mTestOutput; }
    void setOutputFilename(llvm::StringRef outputFilename) {
        assert(!mExecuteFunc);
        mOutputFilename = outputFilename.str();
    }

    bool isStdinGrabbed() const { return mStdinGrabbed; }

    virtual llvm::Value *makeTestLogic(kernel::KernelBuilder &b, const std::vector<llvm::Value *> &operands) = 0;
    virtual llvm::Value *makeCheckLogic(kernel::KernelBuilder &b, const std::vector<llvm::Value *> &operands) = 0;

    void fillParams(kernel::KernelBuilder &b, const std::vector<llvm::Value *> &operands, Params &outParams) const;

    void logKernelBuilder(kernel::KernelBuilder &b) {
        assert(mKernelBuilderID.empty());
        mKernelBuilderID =
            std::format("{} @ block {} b, lane {} b", b.getBuilderUniqueName(), b.getBitBlockWidth(), b.getLaneWidth());
    }
    llvm::StringRef getKernelBuilderID() const { return mKernelBuilderID; }

  protected:
    std::string mDescription;
    std::string mIdentifier;

    unsigned mFieldWidth;
    std::vector<kernel::StreamSet *> mOperandSSs;
    std::string mOutputFilename;
    kernel::StreamSet *mTestOutput;
    kernel::StreamSet *mExpectedOutput;

    bool mStdinGrabbed = false;

    std::unique_ptr<kernel::PipelineBuilder> mPipelineBuilder;
#ifndef NDEBUG
    // Make it obvious that if asserts are disabled we won't check this anyway
    bool mPipelineConfigured = false;
#endif
    std::function<size_t()> mExecuteFunc;
    std::vector<std::function<void()>> mResetFuncs;
    std::vector<std::function<void()>> mDtorFuncs;

    std::string mKernelBuilderID;

    // Parse args that are relevant to this particular type: we might be called as a parent type, so if there are some
    // left over we leave it to the child to decide what to do with them. If they're genuinely extra,
    // configurePipelineFromArgs() (who will have indirectly called us) will notice
    virtual bool parseRelevantArgs(llvm::cl::list<std::string> &args, size_t &cur);
    bool parseUnsigned(llvm::cl::list<std::string> &args, size_t &cur, llvm::StringRef expectName, unsigned minVal,
                       unsigned maxVal, unsigned &outResult);
    bool parseStreamSource(llvm::cl::list<std::string> &args, size_t &cur, llvm::StringRef expectName,
                           llvm::StringRef fdIdent, int32_t &outFDResult, kernel::StreamSet *&outSSResult);
    bool openInputFile(llvm::cl::Option &arg, llvm::StringRef fileName, int32_t &outFDResult);

    // Child should before executing in executePipeline()
    virtual void resetPipeline();

    // Noncopyable
    OperationConfig(const OperationConfig &) = delete;
    OperationConfig &operator=(const OperationConfig &) = delete;
};

extern llvm::cl::extrahelp SupportedOperationsHelp;

bool makeOperationConfig(std::unique_ptr<OperationConfig> &outOpConfig);
