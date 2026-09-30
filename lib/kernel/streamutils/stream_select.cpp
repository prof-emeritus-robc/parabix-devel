/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/streamutils/stream_select.h>

#include <numeric>
#include <unordered_map>
#include <kernel/core/kernel_builder.h>
#include <llvm/Support/ErrorHandling.h>
#include <kernel/pipeline/pipeline_builder.h>

using namespace llvm;

namespace kernel {

namespace __selops {

static std::string to_string(__op op) {
    switch (op) {
    case __op::__select:    return "select";
    case __op::__merge:     return "merge";
    case __op::__intersect: return "intersect";
    default:
        llvm_unreachable("invalid enum value");
        return "";
    }
}

static std::string to_string(StreamSet * ss, uint32_t index) {
    return "<i" + std::to_string(ss->getFieldWidth()) + ">[" + std::to_string(ss->getNumElements()) + "]@" + std::to_string(index);
}

static std::string to_string(SelectOperation const & selop) {
    std::string s = to_string(selop.operation);
    uint32_t index = 0;
    for (auto binding : selop.bindings) {
        s += "_";
        s += to_string(binding.first, index);
        s += ":";
        for (auto idx : binding.second) {
            s += std::to_string(idx);
        }
        s += "_";
        index++;
    }
    return s;
}

__selop<StreamSet *> __selop_init(__op op, StreamSet * from, std::vector<uint32_t> indices) {
    StreamSet * ss = from;
    typename __selop<StreamSet *>::__param_bindings bindings{};
    std::vector<uint32_t> idxVec{};
    idxVec.reserve(indices.size());
    for (auto i : indices) {
        assert ("invalid index" && i < ss->getNumElements());
        idxVec.push_back(i);
    }
    bindings.push_back(std::make_pair(ss, idxVec));
    return __selop<StreamSet *>{op, std::move(bindings)};
}

__selop<StreamSet *> __selop_init(__op op, std::vector<std::pair<StreamSet *, std::vector<uint32_t>>> bindings) {
    typename __selop<StreamSet *>::__param_bindings b{};
    b.reserve(bindings.size());
    for (auto pair : bindings) {
        StreamSet * ss = pair.first;
        std::vector<uint32_t> indices{};
        indices.reserve(pair.second.size());
        for (auto index : pair.second) {
            assert ("invalid index" && index < ss->getNumElements());
            indices.push_back(index);
        }
        b.push_back(std::make_pair(ss, indices));
    }
    return __selop<StreamSet *>{op, std::move(b)};
}

__selop<StreamSet *> __selop_init(__op op, std::vector<StreamSet *> sets) {
    typename __selop<StreamSet *>::__param_bindings b{};
    for (auto set : sets) {
        std::vector<uint32_t> indices(set->getNumElements());
        std::iota(indices.begin(), indices.end(), 0);
        b.push_back(std::make_pair(set, indices));
    }
    return __selop<StreamSet *>{op, std::move(b)};
}

} // namespace __selops

SelectOperation Select(StreamSet * from, std::vector<uint32_t> indices) {
    return __selops::__selop_init(__selops::__op::__select, from, std::move(indices));
}

SelectOperation Select(std::vector<std::pair<StreamSet *, std::vector<uint32_t>>> bindings) {
    return __selops::__selop_init(__selops::__op::__select, std::move(bindings));
}

SelectOperation Select(std::vector<StreamSet *> sets) {
    return __selops::__selop_init(__selops::__op::__select, std::move(sets));
}


SelectOperation Merge(StreamSet * from, std::vector<uint32_t> indices) {
    return __selops::__selop_init(__selops::__op::__merge, from, std::move(indices));
}

SelectOperation Merge(std::vector<std::pair<StreamSet *, std::vector<uint32_t>>> bindings) {
    return __selops::__selop_init(__selops::__op::__merge, std::move(bindings));
}

SelectOperation Merge(std::vector<StreamSet *> sets) {
    return __selops::__selop_init(__selops::__op::__merge, std::move(sets));
}


SelectOperation Intersect(StreamSet * from, std::vector<uint32_t> indices) {
    return __selops::__selop_init(__selops::__op::__intersect, from, std::move(indices));
}

SelectOperation Intersect(std::vector<std::pair<StreamSet *, std::vector<uint32_t>>> bindings) {
    return __selops::__selop_init(__selops::__op::__intersect, std::move(bindings));
}

SelectOperation Intersect(std::vector<StreamSet *> sets) {
    return __selops::__selop_init(__selops::__op::__intersect, std::move(sets));
}

StreamSelect::StreamSelect(LLVMTypeSystemInterface & ts, StreamSet * output, SelectOperation operation)
: BlockOrientedKernel(ts, "StreamSelect" + streamutils::genSignature(operation), {}, {{"output", output}}, {}, {}, {})
{
//    assert (resultStreamCount(operation) == output->getNumElements());
    for (auto const & kv : operation.bindings) {
        if (kv.first->getFieldWidth() != 1) {
            llvm::report_fatal_error("StreamSelect: operations with this kernel are only supported for bitstreams");
        }
    }
    std::unordered_map<StreamSet *, std::string> inputBindings;
    std::tie(mOperations, inputBindings) = streamutils::mapOperationsToStreamNames(operation);
    for (auto const & kv : inputBindings) {
        mInputStreamSets.push_back({kv.second, kv.first});
    }
}

StreamSelect::StreamSelect(LLVMTypeSystemInterface & ts, StreamSet * output, SelectOperationList operations)
: BlockOrientedKernel(ts, "StreamSelect" + streamutils::genSignature(operations), {}, {{"output", output}}, {}, {}, {})
{
//    assert (resultStreamCount(operations) == output->getNumElements());
    std::unordered_map<StreamSet *, std::string> inputBindings;
    std::tie(mOperations, inputBindings) = streamutils::mapOperationsToStreamNames(operations);
    for (auto const & kv : inputBindings) {
        mInputStreamSets.push_back({kv.second, kv.first});
    }
}

void StreamSelect::generateDoBlockMethod(KernelBuilder & b) {
    std::vector<Value *> selectedSet = streamutils::loadInputSelectionsBlock(b, mOperations, b.getSize(0));
    for (unsigned i = 0; i < selectedSet.size(); i++) {
        b.storeOutputStreamBlock("output", b.getInt32(i), selectedSet[i]);
    }
}

IStreamSelect::IStreamSelect(LLVMTypeSystemInterface & ts, StreamSet * output, SelectOperation operation)
: MultiBlockKernel(ts, "IStreamSelect" + streamutils::genSignature(operation),
    {},
    {{"output", output}},
    {}, {}, {})
{
//    assert(resultStreamCount(operation) == output->getNumElements());
    std::unordered_map<StreamSet *, std::string> inputBindings;
    std::vector<__selops::__selop<std::string>> ops;
    std::tie(ops, inputBindings) = streamutils::mapOperationsToStreamNames(operation);
    assert(ops.size() == 1);
    mOperation = ops[0];
    if (mOperation.operation != __selops::__op::__select) {
        llvm::report_fatal_error("IStreamSelect only supports the Select operation");
    }
    for (auto const & kv : inputBindings) {
        assert(mFieldWidth == 0 ? true : mFieldWidth == kv.first->getFieldWidth());
        mFieldWidth = kv.first->getFieldWidth();
        assert(mFieldWidth == output->getFieldWidth());
        mInputStreamSets.push_back({kv.second, kv.first});
    }
}

void IStreamSelect::generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) {
    BasicBlock * const block_Entry = b.GetInsertBlock();
    BasicBlock * const block_Loop = b.CreateBasicBlock("loop");
    BasicBlock * const block_Exit = b.CreateBasicBlock("exit");
    b.CreateBr(block_Loop);

    b.SetInsertPoint(block_Loop);
    PHINode * const positionPhi = b.CreatePHI(b.getSizeTy(), 2);
    positionPhi->addIncoming(b.getSize(0), block_Entry);

    size_t outIdx = 0;
    for (auto const & binding : mOperation.bindings) {
        auto const & name = binding.first;
        for (auto const inIdx : binding.second) {
            for (unsigned i = 0; i < mFieldWidth; ++i) {
                Value * const val = b.loadInputStreamPack(name, b.getSize(inIdx), b.getSize(i), positionPhi);
                b.storeOutputStreamPack("output",  b.getSize(outIdx), b.getSize(i), positionPhi, val);
            }
            outIdx++;
        }
    }
    Value * const nextPosition = b.CreateAdd(positionPhi, b.getSize(1));
    positionPhi->addIncoming(nextPosition, block_Loop);
    b.CreateCondBr(b.CreateICmpNE(nextPosition, numOfStrides), block_Loop, block_Exit);

    b.SetInsertPoint(block_Exit);
}

namespace streamutils {

std::string genSignature(SelectOperation const & operation) {
    return "_" + __selops::to_string(operation);
}

std::string genSignature(SelectOperationList const & operations) {
    std::string s = "";
    for (auto const & op : operations) {
        s += "_";
        s += __selops::to_string(op);
    }
    return s;
}

uint32_t resultStreamCount(SelectOperation const & selop) {
    if (selop.operation == __selops::__op::__select) {
        // select operations return the same number of streams as the number of specified indices
        uint32_t rt = 0;
        for (auto const & binding : selop.bindings) {
            rt += binding.second.size();
        }
        return rt;
    } else {
        // merge and intersect operations return a single stream
        return 1;
    }
}

uint32_t resultStreamFieldWidth(SelectOperation const & selop) {
    assert (selop.bindings.size() > 0);
    uint32_t fw = 0;
    for (auto const & binding : selop.bindings) {
        if (fw == 0) {
            fw = binding.first->getFieldWidth();
            continue;
        }

        uint32_t x = binding.first->getFieldWidth();
        if (x != fw) {
            llvm::report_fatal_error(llvm::StringRef("StreamSelect: mismatched field widths: ") + std::to_string(x) + " vs " + std::to_string(fw));
        }
    }
    return fw;
}

uint32_t resultStreamCount(SelectOperationList const & ops) {
    uint32_t count = 0;
    for (auto const & op : ops) {
        count += resultStreamCount(op);
    }
    return count;
}


// returns { mappedOperations, kernelBindings }
std::pair<SelectedInputList, std::unordered_map<StreamSet *, std::string>>
mapOperationsToStreamNames(__selops::__selop<StreamSet *> const & operation) {
    SelectedInput rt{};
    rt.operation = operation.operation;
    uint32_t idx = 0;
    std::unordered_map<StreamSet *, std::string> namingMap{};
    for (auto const & pair : operation.bindings) {
        std::string name;
        auto it = namingMap.find(pair.first);
        if (it != namingMap.end()) {
            name = it->second;
        } else {
            name = "set" + std::to_string(idx);
            idx++;
            namingMap.insert({pair.first, name});
        }
        rt.bindings.push_back({name, pair.second});
    }
    return std::make_pair(SelectedInputList{rt}, namingMap);
}

// returns { mappedOperations, kernelBindings }
std::pair<SelectedInputList, std::unordered_map<StreamSet *, std::string>>
mapOperationsToStreamNames(SelectOperationList const & operations) {
    SelectedInputList mapped{};
    mapped.reserve(operations.size());
    uint32_t idx = 0;
    std::unordered_map<StreamSet *, std::string> namingMap{};
    for (auto const & selop : operations) {
        __selops::__selop<std::string> mappedOp{};
        mappedOp.operation = selop.operation;
        for (auto const & pair : selop.bindings) {
            std::string name;
            auto it = namingMap.find(pair.first);
            if (it != namingMap.end()) {
                name = it->second;
            } else {
                name = "set" + std::to_string(idx);
                idx++;
                namingMap.insert({pair.first, name});
            }
            mappedOp.bindings.push_back({name, pair.second});
        }
        mapped.push_back(mappedOp);
    }
    return std::make_pair(mapped, namingMap);
}

std::vector<Value *> loadInputSelectionsBlock(KernelBuilder & b, SelectedInputList ops, Value * blockOffset) {
    std::vector<Value *> selectedSet;
    for (auto const & selop : ops) {
        if (selop.operation == __selops::__op::__select) {
            for (auto const & binding : selop.bindings) {
                std::string const & iStreamSetName = binding.first;
                for (auto const & index : binding.second) {
                    Value * block = b.loadInputStreamBlock(iStreamSetName, b.getInt32(index), blockOffset);
                    selectedSet.push_back(block);
                }
            }
        } else if (selop.operation == __selops::__op::__merge) {
            Value * accumulator = nullptr;
            for (auto const & binding : selop.bindings) {
                std::string const & iStreamSetName = binding.first;
                for (auto const & index : binding.second) {
                    Value * const block = b.loadInputStreamBlock(iStreamSetName, b.getInt32(index), blockOffset);
                    if (accumulator == nullptr) {
                        accumulator = block;
                    } else {
                        accumulator = b.simd_or(accumulator, block);
                    }
                }
            }
            selectedSet.push_back(accumulator);
        } else if (selop.operation == __selops::__op::__intersect) {
            Value * accumulator = nullptr;
            for (auto const & binding : selop.bindings) {
                std::string const & iStreamSetName = binding.first;
                for (auto const & index : binding.second) {
                    Value * const block = b.loadInputStreamBlock(iStreamSetName, b.getInt32(index), blockOffset);
                    if (accumulator == nullptr) {
                        accumulator = block;
                    } else {
                        accumulator = b.simd_and(accumulator, block);
                    }
                }
            }
            selectedSet.push_back(accumulator);
        } else {
            llvm_unreachable("invalid enum kernel::__selops::__op value");
        }
    }
    return selectedSet;
}

std::vector<uint32_t> Range(uint32_t lb, uint32_t ub) {
    assert (lb < ub);
    std::vector<uint32_t> range{};
    range.reserve(ub - lb);
    for (; lb < ub; ++lb) {
        range.push_back(lb);
    }
    return range;
}

static StreamSet * runOperation(PipelineBuilder & P, SelectOperation op) {
    uint32_t n = resultStreamCount(op);
    uint32_t fw = resultStreamFieldWidth(op);
    StreamSet * const output = P.CreateStreamSet(n, fw);
    if (fw == 1) {
        P.CreateKernelCall<StreamSelect>(output, op);
    } else if (op.operation == __selops::__op::__select) {
        P.CreateKernelCall<IStreamSelect>(output, op);
    } else {
        llvm::report_fatal_error("only Select operations are supported for streams with field width > 1");
    }
    return output;
}

StreamSet * Select(PipelineBuilder &P, StreamSet * from, uint32_t index) {
    return Select(P, from, std::vector<uint32_t>{index});
}

StreamSet * Select(PipelineBuilder & P, StreamSet * from, std::vector<uint32_t> indices) {
    SelectOperation op = Select(from, std::move(indices));
    return runOperation(P, std::move(op));
}

StreamSet * Select(PipelineBuilder & P, std::vector<std::pair<StreamSet *, std::vector<uint32_t>>> selections) {
    SelectOperation op = Select(selections);
    return runOperation(P, std::move(op));
}

StreamSet * Select(PipelineBuilder &P, std::vector<StreamSet *> sets) {
    SelectOperation op = Select(sets);
    return runOperation(P, std::move(op));
}

StreamSet * Merge(PipelineBuilder & P, StreamSet * from, std::vector<uint32_t> indices) {
    SelectOperation op = Merge(from, std::move(indices));
    return runOperation(P, std::move(op));
}

StreamSet * Merge(PipelineBuilder &P, std::vector<std::pair<StreamSet *, std::vector<uint32_t>>> selections) {
    SelectOperation op = Merge(selections);
    return runOperation(P, std::move(op));
}

StreamSet * Intersect(PipelineBuilder & P, StreamSet * from, std::vector<uint32_t> indices) {
    SelectOperation op = Intersect(from, std::move(indices));
    return runOperation(P, std::move(op));
}

StreamSet * Intersect(PipelineBuilder &P, std::vector<std::pair<StreamSet *, std::vector<uint32_t>>> selections) {
    SelectOperation op = Intersect(selections);
    return runOperation(P, std::move(op));
}

} // namespace streamops

} // namespace kernel
