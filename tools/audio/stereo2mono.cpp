#include <cstdio>
#include <vector>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Module.h>
#include <re/adt/re_name.h>
#include <re/adt/re_re.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/core/streamsetptr.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/streamutils/stream_select.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/scan/scanmatchgen.h>
#include <string>
#include <toolchain/toolchain.h>
#include <fcntl.h>
#include <iostream>
#include <kernel/pipeline/driver/cpudriver.h>
#include <audio/audio.h>
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
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(DemoOptions));
static cl::opt<std::string> outputFile("o", cl::desc("Specify a file to save the modified .wav file."), cl::cat(DemoOptions));

typedef void (*PipelineFunctionType)(StreamSetPtr & ss_buf, int32_t fd);

PipelineFunctionType generatePipeline(CPUDriver & pxDriver, const unsigned int& numChannels, const unsigned int& bitsPerSample)
{

    auto P = CreatePipeline(pxDriver,
                            Output<streamset_t>("OutputBytes", 1, bitsPerSample, ReturnedBuffer(1)),
                            Input<int32_t>("inputFileDecriptor"));

    Scalar * const fileDescriptor = P.getInputScalar("inputFileDecriptor");

    std::vector<StreamSet *> ChannelSampleStreams(numChannels);
    for (unsigned i=0;i<numChannels;++i)
    {
        ChannelSampleStreams[i] = P.CreateStreamSet(1,bitsPerSample);
    }
    ParseAudioBuffer(P, fileDescriptor, numChannels, bitsPerSample, ChannelSampleStreams);

    StreamSet *FirstChannelBasisBits = P.CreateStreamSet(bitsPerSample);
    S2P(P, bitsPerSample, ChannelSampleStreams[0], FirstChannelBasisBits);
    //SHOW_STREAM(FirstChannelBasisBits);

    StreamSet *SecondChannelBasisBits = P.CreateStreamSet(bitsPerSample);
    S2P(P, bitsPerSample, ChannelSampleStreams[1], SecondChannelBasisBits);
    //SHOW_STREAM(SecondChannelBasisBits);

    StreamSet *MonoBasisBits = P.CreateStreamSet(bitsPerSample);
    P.CreateKernelCall<Stereo2MonoPabloKernel>(FirstChannelBasisBits, SecondChannelBasisBits, MonoBasisBits);
    //SHOW_STREAM(MonoBasisBits);

    StreamSet * OutputBytes = P.getOutputStreamSet(0);
    P2S(P, MonoBasisBits, OutputBytes);
    SHOW_BYTES(OutputBytes);

    return P.compile();
}

int main(int argc, char *argv[])
{
    codegen::ParseCommandLineOptions(argc, argv, {&DemoOptions, codegen::codegen_flags()});

    CPUDriver driver("demo");
    const int fd = open(inputFile.c_str(), O_RDONLY);
    unsigned int sampleRate = 0, numChannels = 2, bitsPerSample = 16, numSamples = 0;
    bool isWav = true;
    try
    {
        readWAVHeader(fd, numChannels, sampleRate, bitsPerSample, numSamples);
        std::cout << "numChannels: " << numChannels << ", sampleRate: " << sampleRate << ", bitsPerSample: " << bitsPerSample << ", numSamples: " << numSamples << "\n";
        lseek(fd, 44, SEEK_SET);
    }
    catch (const std::exception &e)
    {
        llvm::errs() << "Warning: cannot parse " << inputFile << " WAV header for processing. Processing file as text.\n";
        lseek(fd, 0, SEEK_SET);
        isWav = false;
    }

    auto fn = generatePipeline(driver, numChannels, bitsPerSample);
    StreamSetPtr wavStream;
    fn(wavStream, fd);
    if (outputFile.getNumOccurrences() != 0) {
        const int fd_out = open(outputFile.c_str(), O_WRONLY | O_CREAT, 0666);
        if (LLVM_UNLIKELY(fd_out == -1)) {
            llvm::errs() << "Error: cannot write to " << outputFile << ".\n";
        } else {
            if (isWav) {
                auto header = createWAVHeader(1, sampleRate, bitsPerSample, numSamples);
                std::ignore = write(fd_out, header.c_str(), header.size());
            }
            // NOTE: Despite a sample can be 8, 16, 32, etc. we treat the stream as bytestream (8-bit) to make it consistent with existing kernels.
            std::ignore = write(fd_out, wavStream.data<8>(), wavStream.length() * (bitsPerSample / 8));
            close(fd_out);
        }
    }
    close(fd);
    return 0;
}
