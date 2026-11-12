#include <cstdio>
#include <vector>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Module.h>
#include <re/adt/re_name.h>
#include <re/adt/re_re.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/core/streamsetptr.h>
#include <kernel/scan/scanmatchgen.h>
#include <kernel/streamutils/stream_select.h>
#include <string>
#include <toolchain/toolchain.h>
#include <toolchain/toolio.h>
#include <fcntl.h>
#include <iostream>
#include <kernel/pipeline/driver/cpudriver.h>
#include <audio/audio.h>
#include <audio/stream_manipulation.h>
#include <iostream>

using namespace kernel;
using namespace llvm;
using namespace codegen;
using namespace audio;

#define SHOW_STREAM(name)           \
    if (codegen::EnableIllustrator) \
    P.captureBitstream(#name, name)
#define SHOW_BIXNUM(name)           \
    if (codegen::EnableIllustrator) \
    P.captureBixNum(#name, name)
#define SHOW_BYTES(name)            \
    if (codegen::EnableIllustrator) \
    P.captureByteData(#name, name)

static cl::OptionCategory DemoOptions("Demo Options", "Demo control options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::cat(DemoOptions));
static cl::opt<int> threshold("t", cl::desc("Difference threshold"), cl::Required, cl::cat(DemoOptions));

typedef void (*PipelineFunctionType)(StreamSetPtr & marker_1, StreamSetPtr & marker_2, int32_t fd);

PipelineFunctionType generatePipeline(BaseDriver &pxDriver, const unsigned int threshold, const unsigned int numChannels, const unsigned int bitsPerSample)
{

    auto P = CreatePipeline(pxDriver, Output<streamset_t>("Marker1", 1, 1, ReturnedBuffer(1)),
                                      Output<streamset_t>("Marker2", 1, 1, ReturnedBuffer(1)),
                                      Input<int32_t>("inputFileDecriptor"));

    Scalar * const fileDescriptor = P.getInputScalar("inputFileDecriptor");

    std::vector<StreamSet *> ChannelSampleStreams(numChannels);
    for (unsigned i=0;i<numChannels;++i)
    {
        ChannelSampleStreams[i] = P.CreateStreamSet(1, bitsPerSample);
    }
    ParseAudioBuffer(P, fileDescriptor, numChannels, bitsPerSample, ChannelSampleStreams);

    for (unsigned i = 0; i < numChannels; ++i)
    {
        StreamSet *BasisBits = P.CreateStreamSet(bitsPerSample);
        S2P(P, bitsPerSample, ChannelSampleStreams[i], BasisBits);

        StreamSet * markers = P.getOutputStreamSet(i);
        P.CreateKernelCall<DiscontinuityKernel>(BasisBits, threshold, markers);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            P.captureBitstream("markers" + std::to_string(i), markers);
        }
    }
    return P.compile();
}

int main(int argc, char *argv[])
{
    codegen::ParseCommandLineOptions(argc, argv, {&DemoOptions, &toolio::InputOutputOptions, codegen::codegen_flags()});

    toolio::Input in;
    if (!toolio::openInput(inputFile, in)) {
        return 1;
    }
    const int fd = in.fd;
    CPUDriver driver("demo");
    unsigned int sampleRate = 0, numChannels = 1, bitsPerSample = 16, numSamples = 0;
    try
    {
        readWAVHeader(fd, numChannels, sampleRate, bitsPerSample, numSamples);
        lseek(fd, 44, SEEK_SET);
        llvm::errs() << "numChannels: " << numChannels << ", sampleRate: " << sampleRate << ", bitsPerSample: " << bitsPerSample << ", numSamples: " << numSamples << "\n";
    }
    catch (const std::exception &e)
    {
        llvm::errs() << "Warning: cannot parse " << in.name << " WAV header for processing. Processing file as text.\n";
        if (lseek(fd, 0, SEEK_SET) == -1) {
            llvm::errs() << toolio::programName() << ": " << in.name << ": cannot be read again as text (a pipe)\n";
            return 1;
        }
    }

    auto fn = generatePipeline(driver, threshold, numChannels, bitsPerSample);
    StreamSetPtr wavStream, wavStream1;
    fn(wavStream, wavStream1, fd);
    toolio::closeInput(in);
    return 0;
}
