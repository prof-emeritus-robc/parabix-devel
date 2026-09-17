#ifndef FUNCTION_SNIPPET_H
#define FUNCTION_SNIPPET_H

#include <idisa/idisa_builder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <functional>


llvm::Value * CallFunctionByToken(IDISA::IDISA_Builder & b,
                                  llvm::Type * retTy, llvm::StringRef name, llvm::ArrayRef<llvm::Value *> params,
                                  std::function<llvm::Value *(llvm::ArrayRef<llvm::Value *>)> functionGenerator);


class FunctionSnippetPassManagerProxy final : public llvm::PassManagerBase {
public:

    FunctionSnippetPassManagerProxy(llvm::Module * M, llvm::PassManagerBase & pm, const bool addPostOptimizations);

    void add(llvm::Pass * P) override;

private:
    llvm::PassManagerBase & BasePM;
    const bool AddPostOptimizations;
};


#endif // FUNCTION_SNIPPET_H
