#include "../pipeline_compiler.hpp"

namespace kernel {

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getFinalOutputScalars
 ** ------------------------------------------------------------------------------------------------------------- */
std::vector<Value *> PipelineCompiler::getFinalOutputScalars(KernelBuilder & b) {
    std::vector<Value *> args;
    Module * m = b.getModule();

    for (unsigned call = FirstCall; call <= LastCall; ++call) {
        writeOutputScalars(b, call, args);
        const RelationshipNode & rn = mScalarGraph[call];
        const CallBinding & C = rn.Callee;
        Function * const f = m->getFunction(C.Callee->getName()); assert (f);
        assert (&f->getContext() == &b.getContext());
        auto i = f->arg_begin();
        for (auto j = args.begin(); j != args.end(); ++i, ++j) {
            assert (i != f->arg_end());
            Type * ty = i->getType(); assert (&ty->getContext() == &b.getContext());
           // ty = CBuilder::convertTypeToLLVMContext(b.getContext(), ty);
            *j = b.CreateZExtOrTrunc(*j, ty);
        }
        assert (i == f->arg_end());
        mScalarValue[call] = b.CreateCall(f, args);
    }
    writeOutputScalars(b, PipelineOutput, args);
    return args;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief writeOutputScalars
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::writeOutputScalars(KernelBuilder & b, const size_t index, std::vector<Value *> & args) {
    const auto n = in_degree(index, mScalarGraph);
    args.resize(n);
    for (const auto e : make_iterator_range(in_edges(index, mScalarGraph))) {
        const auto scalar = source(e, mScalarGraph);
        const RelationshipType & rt = mScalarGraph[e];
        Value * val = getScalar(b, scalar);
        assert (&val->getContext() == &b.getContext());
        args[rt.Number] = val;
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief initializeScalarValues
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::initializeScalarValues(KernelBuilder & b) {
    mScalarValue.reset(FirstKernel, LastScalar);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getScalar
 ** ------------------------------------------------------------------------------------------------------------- */
Value * PipelineCompiler::getScalar(KernelBuilder & b, const size_t index) {
    assert (index >= FirstKernel && index <= LastScalar);
    Value * value = mScalarValue[index];
    if (value) {
        assert (&value->getContext() == &b.getContext());
        return value;
    }

    const RelationshipNode & rn = mScalarGraph[index];
    assert (rn.Type == RelationshipNode::IsScalar);
    const Relationship * const rel = rn.Relationship; assert (rel);

    if (LLVM_UNLIKELY(in_degree(index, mScalarGraph) == 0)) {
        value = cast<ScalarConstant>(rel)->value(b.getContext());
    } else {
        const auto producer = in_edge(index, mScalarGraph);
        const auto i = source(producer, mScalarGraph);
        const RelationshipType & rt = mScalarGraph[producer];
        if (i == PipelineInput) {
            const Binding & input = mTarget->getInputScalarBinding(rt.Number);
            value = b.getScalarField(input.getName());
            assert (&value->getContext() == &b.getContext());
        } else { // output scalar of some kernel
            Value * const outputScalars = getScalar(b, i);
            assert (&outputScalars->getContext() == &b.getContext());
            if (LLVM_UNLIKELY(outputScalars == nullptr)) {
                report_fatal_error("Internal error: pipeline is unable to locate valid output scalar");
            }
            if (outputScalars->getType()->isAggregateType()) {
                value = b.CreateExtractValue(outputScalars, {rt.Number});
                assert (&value->getContext() == &b.getContext());
            } else { assert (rt.Number == 0 && "scalar type is not an aggregate");
                value = outputScalars;
            }
        }
    }
    assert (value);
    assert (&value->getContext() == &b.getContext());
    mScalarValue[index] = value;
    return value;
}


}
