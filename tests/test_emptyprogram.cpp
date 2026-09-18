/*
 *  Copyright (c) 2018 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#include <kernel/core/idisa_target.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/io/source_kernel.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <kernel/core/streamsetptr.h>
#include <llvm/IR/Constant.h>
#include <testing/assert.h>
#include <random>
#include <type_traits>
#include <llvm/IR/Instructions.h>

using namespace kernel;
using namespace llvm;

#define BEGIN_SCOPED_REGION {
#define END_SCOPED_REGION }

template <typename Function, typename... Params>
void run_test(const char * testName, bool & success, Function func, Params &&... params) {
    try {
        func(std::forward<Params>(params)...);
    }  catch (std::exception & e) {
        llvm::errs() << testName << " test failed with error message: " << e.what() << "\n";
        success = false;
    }
}

template <typename RetTy, typename Function, typename... Params>
RetTy run_test_with_retval(const char * testName, bool & hasError, Function func, Params  &&... params) {
    try {
        return func(std::forward<Params>(params)...);
    }  catch (std::exception & e) {
        llvm::errs() << testName << " test failed with error message: " << e.what() << "\n";
        hasError = true;
        return RetTy{};
    }
}

bool testEmptyPrograms(CPUDriver & driver) {

    // TODO: need some sort of run and catch program that can handle timeouts? use functions as functors?

    std::random_device rd;
    std::default_random_engine rng(rd());
    std::uniform_int_distribution<uint64_t> dist(0ULL, std::numeric_limits<uint64_t>::max());

    bool hasError = false;

    BEGIN_SCOPED_REGION
    auto P = CreatePipeline(driver);
    const auto f = P.compile();
    run_test("No parameter", hasError, f);
    END_SCOPED_REGION

    BEGIN_SCOPED_REGION
    auto P = CreatePipeline(driver, Input<uint64_t>("a"));
    const auto f = P.compile();
    const uint64_t a = 1234;
    run_test("Single input", hasError, f, a);
    END_SCOPED_REGION

    BEGIN_SCOPED_REGION
    auto P = CreatePipeline(driver, Output<uint64_t>("b"));
    const uint64_t a = dist(rng);
    Scalar * const A = P.CreateConstant(ConstantInt::get(P.getTypeSystem().getInt64Ty(), a));
    assert (isa<ScalarConstant>(A));
    P.setOutputScalar("b", A);
    const auto f = P.compile();
    const auto b = run_test_with_retval<uint64_t>("Single output", hasError, f);
    if (LLVM_UNLIKELY(a != b)) {
        llvm::errs() << "Single output" << " test failed: Output scalar does not match expected constant scalar\n";
        hasError = true;
    }
    END_SCOPED_REGION

    BEGIN_SCOPED_REGION
    auto P = CreatePipeline(driver, Input<uint64_t>("a"), Output<uint64_t>("b"));
    Scalar * const A = P.getInputScalar("a");
    P.setOutputScalar("b", A);
    const auto f = P.compile();
    const uint64_t a = dist(rng);
    const auto b = run_test_with_retval<uint64_t>("Single input and output", hasError, f, a);
    if (LLVM_UNLIKELY(a != b)) {
        llvm::errs() << "Input scalar does not match output scalar\n";
        hasError = true;
    }
    END_SCOPED_REGION

    std::array<uint64_t, 512> actual;
    for (size_t i = 0; i < actual.size(); ++i) {
        actual[i] = dist(rng);
    }

    BEGIN_SCOPED_REGION
    auto P = CreatePipeline(driver, Input<uint8_t*>("buffer"), Input<size_t>("length"));
    Scalar * const buffer = P.getInputScalar("buffer");
    Scalar * const length = P.getInputScalar("length");
    StreamSet * const InternalBytes = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<MemorySourceKernel>(buffer, length, InternalBytes);
    const auto f = P.compile();
    run_test("Deleted MemorySource", hasError, f, (uint8_t*)actual.data(), actual.size()* sizeof(uint64_t));
    END_SCOPED_REGION

    BEGIN_SCOPED_REGION
    auto P = CreatePipeline(driver, Output<streamset_t>("output", 1, 8), Input<uint8_t*>("buffer"), Input<size_t>("length"));
    Scalar * const buffer = P.getInputScalar("buffer");
    Scalar * const length = P.getInputScalar("length");
    StreamSet * OutputBytes = P.getOutputStreamSet("output");
    P.CreateKernelCall<MemorySourceKernel>(buffer, length, OutputBytes);
    StreamSetPtr ss;
    const auto f = P.compile();
    run_test("Kept MemorySource", hasError, f, ss, (uint8_t*)actual.data(), actual.size()* sizeof(uint64_t));
    for (size_t i = 0; i < actual.size(); ++i) {
        if (*ss.data<64>(i) !=  actual[i]) {
            llvm::errs() << "Memory source buffer does not match original value at position " << (i * 8) << "\n";
            hasError = true;
            break;
        }
    }
    END_SCOPED_REGION

    return hasError;
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {});
    CPUDriver driver("test");
    return testEmptyPrograms(driver) ? -1 : 0;
}
