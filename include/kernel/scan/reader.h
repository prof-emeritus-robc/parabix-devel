/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <initializer_list>
#include <kernel/scan/base.h>
#include <kernel/streamutils/stream_select.h>
#include <kernel/pipeline/driver/driver.h>
#include <kernel/pipeline/pipeline_builder.h>

namespace kernel {

/**
 * Reads a stream of scan indices to trigger a callback function.
 *
 * The scan indices are converted to pointers to elements in a source stream.
 *
 * Any number of scan index streams are allowed in the `scanIndices` stream set.
 * All of which will be converted to pointers and passed to the callback.
 *
 * Optionally, additional streams may be suplied to supplement the scan pointer
 * when performing the callback. A single element will be extracted from each
 * stream and passed to the callback as a parameter. For example, adding a
 * stream of line numbers (generated via the LineNumberGenerator kernel) will
 * pass the line number of of each scan position to the callback. Note that
 * unlike streams in the `scanIndices` streamset, none of these  additional
 * streams will be converted to pointers.
 *
 * Any additional streams will be processed at a rate equal to the scan stream.
 * Items from additional streams will be passed to the callback in the same
 * order that the streams appear in the kernel's constructor.
 *
 * Signature:
 *  kernel ScanReader :: [<i8>[1] source, <i64>[N] scanIndices, func(i8*, i8*..., ...), ...] -> []
 *
 * Callback Signature:
 *  void (*) (const uint8_t * scanPtr, i8*..., ...);
 *
 * Code Example:
 *  Supplying line numbers to a callback:
 *
 *      extern "C" void callback(const uint8_t * ptr, uint64_t linenum) { ... }
 *      // ...
 *      StreamSet * const source = ...;
 *      StreamSet * const scanbits = ...;
 *      StreamSet * const linebreaks = ...;
 *
 *      StreamSet * const scanIndices = P->CreateStreamSet(1, 64);
 *      P->CreateKernelCall<ScanIndexGenerator>(scanbits, scanIndices);
 *      StreamSet * const linenums = P->CreateStreamSet(1, 64);
 *      P->CreateKernelCall<LineNumberGenerator>(scanbits, linebreaks, linenums);
 *      Kernel * const reader = P->CreateKernelCall<ScanReader>(source, scanIndices, "callback", AdditionalStreams{linenums});
 *      driver.LinkFunction(reader, "callback", callback);
 *
 *  Note that in this example, the line numbers are zero-indexed when passed to
 *  the callout. This differs from the LineBasedKernel which internally converts
 *  line numbers to one-indexed before passing them to the callback.
 *
 * Code Example 2:
 *  Using start and end streams to print out some text:
 *
 *      extern "C" void callback(const uint8_t * begin, const uint8_t * end) {
 *          std::cout << std::string(begin, end) << "\n";
 *      }
 *      // ...
 *      StreamSet * const source = ...; // <i8>[1]
 *      StreamSet * const starts = ...; // <i1>[1]
 *      StreamSet * const ends = ...;   // <i1>[1]
 *
 *      StreamSet * const startIndeices = P->CreateStreamSet(1, 64); // <i64>[1]
 *      P->CreateKernelCall<ScanIndexGenerator>(starts, startIndices);
 *      StreamSet * const endIndices = P->CreateStreamSet(1, 64); // <i64>[1]
 *      P->CreateKernelCall<ScanIndexGenerator>(ends, endIndices)
 *      Kernel * const reader = P->CreateKernelCall<ScanReader>(source, streamops::Select(P, {{startIndices, {0}}, {endIndices, {0}}}), "callback");
 *      driver.LinkFunction(reader, "callback", callback);
 */
class ScanReader : public MultiBlockKernel {
public:
    ScanReader(LLVMTypeSystemInterface & ts,
        StreamSet * source,
        StreamSet * scanIndices,
        std::string const & callbackName);

    ScanReader(LLVMTypeSystemInterface & ts,
        StreamSet * source,
        StreamSet * scanIndices,
        std::string const & callbackName,
        std::string const & doneCallbackName);

    ScanReader(
        LLVMTypeSystemInterface & ts,
        StreamSet * source,
        StreamSet * scanIndices,
        std::string const & callbackName,
        std::initializer_list<StreamSet *> additionalStreams);

    ScanReader(
        LLVMTypeSystemInterface & ts,
        StreamSet * source,
        StreamSet * scanIndices,
        std::string const & callbackName,
        std::string const & doneCallbackName,
        std::initializer_list<StreamSet *> additionalStreams);
protected:
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;
private:
    std::string              mCallbackName;
    std::string              mDoneCallbackName;
//    std::vector<std::string> mAdditionalStreamNames;
    const uint32_t           mNumScanStreams;
};


namespace scan {

/**
 * A pair of a function and its name; used as a parameter to `scan::Reader`.
 *
 * Use the `SCAN_CALLBACK` macro to construct an instance of this type from an
 * `extern "C"` function.
 */
template<typename... Args>
struct CallbackPair {
    using FunctionType = void(Args...);
    using FunctionPointerType = void(*)(Args...);
    std::string name;
    FunctionPointerType func;
    CallbackPair(std::string name, FunctionPointerType func)
    : name(std::move(name)), func(func)
    {}
};

template<typename... U>
CallbackPair<U...> make_callback_pair(std::string const & name, void(* func)(U...)) {
    return CallbackPair<U...>(name, func);
}

/**
 * Creates a `CallbackPair<Fn>` instance.
 *
 * Usage Example:
 *  In this example, `SCAN_CALLBACK` is used to pass the `extern "C"` function
 *  `foo` to a call to `scan::Reader`.
 *
 *      extern "C" foo(const uint8_t * ptr, uint64_t lineNum);
 *      // -- snip --
 *      CPUDriver driver = ...;
 *      PipelineBuilder P = ...;
 *      StreamSet * Source = ...; // <i8>[1]
 *      StreamSet * Indices = ...; // <i64>[1]
 *      StreamSet * LineNums = ...; // <i64>[1]
 *      scan::Reader(P, driver, SCAN_CALLBACK(foo), Source, Indices, { LineNums });
 */
#define SCAN_CALLBACK(Fn) (kernel::scan::make_callback_pair(std::string(#Fn), Fn))

/**
 * Invokes the `ScanReader` kernel to perform a callback for each item in
 * `scanIndicies`.
 *
 * The `scanIndicies` stream set may contain multiple streams, all of which are
 * converted to pointers to items in the `source` stream before invoking the
 * callback function.
 *
 * The `callback` parameter should be supplied via the `SCAN_CALLBACK` marco.
 *
 * Callback Type:
 *  The template parameter pack `Args...` is inferred via the parameters in the
 *  callback function. These parameters must line up with the input streams
 *  provided to the reader. In this overload case, the callback must have the
 *  same number of parameters as there are streams in `scanIndices` with each
 *  parameter being a constant pointer to an integer with the same field width
 *  as `source`.
 *
 *  The callback function must have a return type of `void` and be declared
 *  extern "C".
 *
 *  Example:
 *      Given the following types for the input stream sets:
 *
 *          source: <i8>[1]
 *          scanIndicies: <i64>[2]
 *
 *      `callback` must have the following prototype (disregarding function name):
 *
 *          extern "C" void callback(const uint8_t *, const uint8_t *);
 *
 *      or possibly (due to an implicit cast from uint8_t to char):
 *
 *          extern "C" void callback(const char *, const char *);
 *
 *
 * Preconditions:
 *  - `source` must contain only a string stream (i.e., <iN>[1])
 *  - `source` may not have a field width of 1
 *  - `scanIndicies` must have a field width of 64 (i.e., <i64>[N])
 *  - The function type of `callback` must be equivalent to the layout of input
 *    streams (see Callback Type)
 */
template<typename... Args>
inline
void Reader(
    PipelineBuilder & P,
    BaseDriver & driver,
    CallbackPair<Args...> callback,
    StreamSet * source,
    StreamSet * scanIndices)
{
    using Fn = typename CallbackPair<Args...>::FunctionType;
    assert(scanIndices->getFieldWidth() == 64);
    assert(source->getFieldWidth() != 1);
    Kernel * const reader = P. template CreateKernelCall<ScanReader>(source, scanIndices, callback.name);
    P. template LinkFunction<Fn>(reader, callback.name, *callback.func);
}

template<typename... Args, typename... Args2>
inline
void Reader(
    PipelineBuilder & P,
    BaseDriver & driver,
    CallbackPair<Args...> callback,
    CallbackPair<Args2...> doneCallback,
    StreamSet * source,
    StreamSet * scanIndices)
{
    using Fn = typename CallbackPair<Args...>::FunctionType;
    using Fn2 = typename CallbackPair<Args2...>::FunctionType;
    assert(scanIndices->getFieldWidth() == 64);
    assert(source->getFieldWidth() != 1);
    Kernel * const reader = P. template CreateKernelCall<ScanReader>(source, scanIndices, callback.name, doneCallback.name);
    P. template LinkFunction<Fn>(reader, callback.name, *callback.func);
    P. template LinkFunction<Fn2>(reader, doneCallback.name, &doneCallback.func);
}

/**
 * An overload of `scan::Reader` allowing additional streams to pass items to
 * the callback.
 *
 * Items in `additionalStreams` are not converted to pointers unlike the items
 * in `scanIndicies`. As such, they may be of varying field widths.
 *
 * Callback Type:
 *  Along with the pointers from `scanIndicies`, the callback function must also
 *  contain parameters for each stream from the `additionalStreams` stream set
 *  list.
 *
 *  Example:
 *      Given a source stream of type <i8>, 1 `scanIndicies` stream, and the
 *      following additional streams:
 *
 *          additional stream 0: <i8>[1]
 *          additional stream 1: <i64>[1]
 *
 *      `callback` must have the following prototype:
 *
 *          extern "C" void callback(const uint8_t *, int8_t, int64_t)
 *
 *      Note that unsigned varients will also work.
 */
template<typename... Args>
inline
void Reader(
    PipelineBuilder & P,
    BaseDriver & driver,
    CallbackPair<Args...> callback,
    StreamSet * source,
    StreamSet * scanIndices,
    std::initializer_list<StreamSet *> additionalStreams)
{
    using Fn = typename CallbackPair<Args...>::FunctionType;
    Kernel * const reader = P. template CreateKernelCall<ScanReader>(source, scanIndices, callback.name, std::move(additionalStreams));
    P. template LinkFunction<Fn>(reader, callback.name, *callback.func);
}

/**
 * An overloaded `scan::Reader` that allows a callback that is called
 * when the kernel is finishing processing.
 *
 * Note: You should not mark the EOFbit - it should be unset or will not work.
 */
template<typename... Args, typename... Args2>
inline
void Reader(
    PipelineBuilder & P,
    BaseDriver & driver,
    CallbackPair<Args...> callback,
    CallbackPair<Args2...> doneCallback,
    StreamSet * source,
    StreamSet * scanIndices,
    std::initializer_list<StreamSet *> additionalStreams)
{
    using Fn = typename CallbackPair<Args...>::FunctionType;
    using Fn2 = typename CallbackPair<Args2...>::FunctionType;
    Kernel * const reader = P. template CreateKernelCall<ScanReader>(source, scanIndices, callback.name, doneCallback.name, std::move(additionalStreams));
    P. template LinkFunction<Fn>(reader, callback.name, *callback.func);
    P. template LinkFunction<Fn2>(reader, doneCallback.name, *doneCallback.func);
}

/**
 * An overloaded `scan::Reader` allowing multiple stream sets to be passed as
 * scan indicies.
 *
 * Equivalent to calling `streamutils::Select(P, {ptrStreams...})` and passing
 * the result into `scanIndicies`.
 */
template<typename... Args>
inline
void Reader(
    PipelineBuilder & P,
    BaseDriver & driver,
    CallbackPair<Args...> callback,
    StreamSet * source,
    std::initializer_list<StreamSet *> ptrStreams)
{
    namespace su = kernel::streamutils;
    using Fn = typename CallbackPair<Args...>::FunctionType;
    StreamSet * const indices = su::Select(P, std::vector<StreamSet *>(ptrStreams));
    Kernel * const reader = P. template CreateKernelCall<ScanReader>(source, indices, callback.name);
    P. template LinkFunction<Fn>(reader, callback.name, *callback.func);
}

/**
 * An overloaded `scan::Reader` that allows a callback that is called
 * when the kernel is finishing processing.
 *
 * Note: You should not mark the EOFbit - it should be unset or will not work.
 */
template<typename... Args, typename... Args2>
inline
void Reader(
    PipelineBuilder & P,
    BaseDriver & driver,
    CallbackPair<Args...> callback,
    CallbackPair<Args2...> doneCallback,
    StreamSet * source,
    std::initializer_list<StreamSet *> ptrStreams)
{
    namespace su = kernel::streamutils;
    using Fn = typename CallbackPair<Args...>::FunctionType;
    using Fn2 = typename CallbackPair<Args2...>::FunctionType;
    StreamSet * const indices = su::Select(P, std::vector<StreamSet *>(ptrStreams));
    Kernel * const reader = P. template CreateKernelCall<ScanReader>(source, indices, callback.name, doneCallback.name);
    P. template LinkFunction<Fn>(reader, callback.name, *callback.func);
    P. template LinkFunction<Fn2>(reader, doneCallback.name, *doneCallback.func);
}

/**
 * An overloaded `scan::Reader` allowing multiple stream sets to be passed as
 * scan indicies while also allowing `additionalStreams`.
 *
 * Equivalent to calling `streamutils::Select(P, {ptrStreams...})` and passing
 * the result into `scanIndicies`.
 */
template<typename... Args>
inline
void Reader(
    PipelineBuilder & P,
    BaseDriver & driver,
    CallbackPair<Args...> callback,
    StreamSet * source,
    std::initializer_list<StreamSet *> ptrStreams,
    std::initializer_list<StreamSet *> additionalStreams)
{
    namespace su = kernel::streamutils;
    using Fn = typename CallbackPair<Args...>::FunctionType;
    StreamSet * const indices = su::Select(P, std::move(ptrStreams));
    Kernel * const reader = P. template CreateKernelCall<ScanReader>(source, indices, callback.name, std::move(additionalStreams));
    P. template LinkFunction<Fn>(reader, callback.name, *callback.func);
}

/**
 * An overloaded `scan::Reader` that allows a callback that is called
 * when the kernel is finishing processing.
 *
 * Note: You should not mark the EOFbit - it should be unset or will not work.
 */
template<typename... Args, typename... Args2>
inline
void Reader(
    PipelineBuilder & P,
    BaseDriver & driver,
    CallbackPair<Args...> callback,
    CallbackPair<Args2...> doneCallback,
    StreamSet * source,
    std::initializer_list<StreamSet *> ptrStreams,
    std::initializer_list<StreamSet *> additionalStreams)
{
    namespace su = kernel::streamutils;
    using Fn = typename CallbackPair<Args...>::FunctionType;
    using Fn2 = typename CallbackPair<Args2...>::FunctionType;
    StreamSet * const indices = su::Select(P, std::move(ptrStreams));
    Kernel * const reader = P.template CreateKernelCall<ScanReader>(source, indices, callback.name, doneCallback.name, std::move(additionalStreams));
    P. template LinkFunction<Fn>(reader, callback.name, *callback.func);
    P. template LinkFunction<Fn2>(reader, doneCallback.name, *doneCallback.func);
}

} // namespace kernel::scan

} // namespace kernel
