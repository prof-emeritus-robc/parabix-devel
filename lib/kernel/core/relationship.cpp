#include <kernel/core/relationship.h>

#include <llvm/IR/Constant.h>
#include <llvm/IR/Constant.h>
#include <idisa/idisa_builder.h>
#include <toolchain/toolchain.h>

using namespace llvm;
using namespace IDISA;

namespace kernel {

inline VectorType * LLVM_READNONE getStreamTy(LLVMContext & C, const unsigned FieldWidth) {
    return FixedVectorType::get(IntegerType::getIntNTy(C, FieldWidth), static_cast<unsigned>(0));
}

inline ArrayType * LLVM_READNONE getStreamSetTy(LLVMContext & C, const unsigned NumElements, const unsigned FieldWidth) {
    return ArrayType::get(getStreamTy(C, FieldWidth), NumElements);
}

unsigned StreamSet::getNumElements() const {
    return mType->getArrayNumElements();
}

unsigned StreamSet::getFieldWidth() const {
    return cast<VectorType>(mType->getArrayElementType())->getElementType()->getIntegerBitWidth();
}

std::string StreamSet::shapeString() {
    return std::to_string(getNumElements()) + "x" + std::to_string(getFieldWidth());
}

unsigned Scalar::getFieldWidth() const {
    return mType->getIntegerBitWidth();
}

llvm::Constant * ScalarConstant::value(llvm::LLVMContext & C) const {
    return CBuilder::convertConstantToLLVMContext(C, mConstant);
}

StreamSet::StreamSet(LLVMContext & C, const ClassTypeId typeId, const unsigned NumElements, const unsigned FieldWidth) noexcept
: Relationship(typeId, getStreamSetTy(C, NumElements, FieldWidth)) {

}

Scalar::Scalar(const ClassTypeId typeId, llvm::Type * type) noexcept
: Relationship(typeId, type){

}

Scalar::Scalar(not_null<Type *> type) noexcept
: Scalar(Relationship::ClassTypeId::Scalar, type.get()){

}

CommandLineScalar::CommandLineScalar(const CommandLineScalarType clType, llvm::Type * type) noexcept
: Scalar(Relationship::ClassTypeId::CommandLineScalar, type)
, mCLType(clType) {

}

ScalarConstant::ScalarConstant(not_null<Constant *> constant) noexcept
: Scalar(Relationship::ClassTypeId::ScalarConstant, constant->getType())
, mConstant(constant.get()) {

}


}
