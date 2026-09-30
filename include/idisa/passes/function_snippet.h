#ifndef FUNCTION_SNIPPET_H
#define FUNCTION_SNIPPET_H

#include <idisa/idisa_builder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <functional>


// How function snippets are compiled (--function-snippets):
//   Direct: no snippet functions; the generated code is emitted in place.
//   LateIR: snippets are noinline functions during IR optimization and are
//           inlined by inlineFunctionSnippets just before code generation.
//   MIR:    snippets are spliced after instruction selection by the
//           FunctionSnippetTokenReplacerPass (see FunctionSnippetPassManagerProxy).
enum class FunctionSnippetMode { Direct, LateIR, MIR };

FunctionSnippetMode getFunctionSnippetMode();

// LateIR mode: inline all snippet functions in M and delete them.
void inlineFunctionSnippets(llvm::Module & M);

llvm::Value * CallFunctionByToken(IDISA::IDISA_Builder & b,
                                  llvm::Type * retTy, llvm::StringRef name, llvm::ArrayRef<llvm::Value *> params,
                                  std::function<llvm::Value *(llvm::ArrayRef<llvm::Value *>)> functionGenerator);


class FunctionSnippetPassManagerProxy final : public llvm::PassManagerBase {
public:

    FunctionSnippetPassManagerProxy(llvm::Module & M, llvm::PassManagerBase & pm, const bool addPostOptimizations);

    void add(llvm::Pass * P) override;

private:
    llvm::PassManagerBase & BasePM;
    const bool AddPostOptimizations;
    bool AlreadyInsertedFunctionSnippetPass;
};


#endif // FUNCTION_SNIPPET_H
