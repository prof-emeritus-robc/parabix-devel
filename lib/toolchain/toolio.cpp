/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <toolchain/toolio.h>

#include <toolchain/toolchain.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <ostream>
#include <streambuf>
#include <vector>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace llvm;

namespace toolio {

cl::OptionCategory InputOutputOptions("Input and Output Options", "These options control the input and output of the tool.");

namespace {

bool MMapFlag = false;
std::string OutputFileName;

// The first error of toolio_write (an errno value)
std::atomic<int> WriteError{0};

struct OutputState {
    bool opened = false;
    bool finished = false;
    int fd = -1;
    std::string shownName;      // for messages
    std::string target;         // the file replaced by the temporary file
    std::string temporary;      // written, renamed to target by finishOutput
    std::unique_ptr<raw_fd_ostream> stream;
};

OutputState & output() {
    static OutputState state;
    return state;
}

void report(const std::string & what, const int error) {
    errs() << programName() << ": " << what << ": " << std::strerror(error) << "\n";
}

// At exit without finishOutput: what out() buffered is written (an error is
// not reported, nor fatal as in raw_fd_ostream's destructor), but a temporary
// output file is removed rather than put in place.
void atExit() {
    OutputState & o = output();
    if (o.stream) {
        o.stream->flush();
        o.stream->clear_error();
        o.stream.reset();
    }
    if (!o.temporary.empty()) {
        ::unlink(o.temporary.c_str());
        o.temporary.clear();
    }
}

} // anonymous namespace

RegisterInputOptions::RegisterInputOptions() {
    static cl::opt<bool, true> MMapOption("mmap", cl::location(MMapFlag), cl::init(false),
        cl::desc("Map input files into memory, instead of reading them."), cl::cat(InputOutputOptions));
}

RegisterOutputOptions::RegisterOutputOptions() {
    static cl::opt<std::string, true> OutputOption("output", cl::location(OutputFileName), cl::value_desc("file"),
        cl::desc("Write the output to file (- for the standard output)."), cl::cat(InputOutputOptions));
}

const char * programName() {
    const char * const name = codegen::ProgramName;
    if (name == nullptr) {
        return "parabix";
    }
    const char * const slash = std::strrchr(name, '/');
    return slash ? slash + 1 : name;
}

bool mmapRequested() {
    return MMapFlag;
}

bool useMMapFor(const int fd) {
    struct stat st;
    return MMapFlag && ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && ::lseek(fd, 0, SEEK_CUR) == 0;
}

bool openInput(const std::string & name, Input & in) {
    in = Input{};
    if (name.empty() || name == "-") {
        in.name = "(standard input)";
        in.fd = STDIN_FILENO;
        in.isStdIn = true;
    } else {
        in.name = name;
        in.fd = ::open(name.c_str(), O_RDONLY | O_CLOEXEC);
        if (in.fd == -1) {
            report(name, errno);
            return false;
        }
    }
    struct stat st;
    if (::fstat(in.fd, &st) != 0) {
        report(in.name, errno);
        closeInput(in);
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        report(in.name, EISDIR);
        closeInput(in);
        return false;
    }
    in.isRegularFile = S_ISREG(st.st_mode);
    if (in.isRegularFile) {
        // A mapping starts at the beginning of the file, but the standard
        // input may have been read in part already: then it is read from
        // where it is.
        const off_t position = ::lseek(in.fd, 0, SEEK_CUR);
        in.start = (position > 0) ? position : 0;
        in.mappable = (position == 0);
    }
    in.useMMap = MMapFlag && in.mappable;
    return true;
}

bool openSeekableInput(const std::string & name, Input & in) {
    if (!openInput(name, in)) {
        return false;
    }
    if (in.isRegularFile) {
        return true;
    }
    // A temporary file, removed at once (it lasts while open)
    const char * const dir = std::getenv("TMPDIR");
    const std::string pattern = std::string((dir && *dir) ? dir : "/tmp") + "/parabix-input.XXXXXX";
    std::vector<char> temporary(pattern.begin(), pattern.end());
    temporary.push_back('\0');
    const int fd = ::mkstemp(temporary.data());
    if (fd == -1) {
        report("cannot create a temporary file for " + in.name, errno);
        closeInput(in);
        return false;
    }
    ::unlink(temporary.data());
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    std::vector<char> buffer(1 << 16);
    for (;;) {
        const ssize_t n = ::read(in.fd, buffer.data(), buffer.size());
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            report(in.name, errno);
            ::close(fd);
            closeInput(in);
            return false;
        }
        for (ssize_t done = 0; done < n; ) {
            const ssize_t w = ::write(fd, buffer.data() + done, static_cast<size_t>(n - done));
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                report("cannot copy " + in.name + " to a temporary file", errno);
                ::close(fd);
                closeInput(in);
                return false;
            }
            done += w;
        }
    }
    ::lseek(fd, 0, SEEK_SET);
    closeInput(in);
    in.fd = fd;
    in.isStdIn = false;
    in.isRegularFile = true;
    in.mappable = true;
    in.useMMap = MMapFlag;
    in.start = 0;
    in.spooled = true;
    return true;
}

int rewindInput(const Input & in) {
    if (::lseek(in.fd, static_cast<off_t>(in.start), SEEK_SET) == -1) {
        report(in.name, errno);
        return -1;
    }
    return in.fd;
}

void closeInput(Input & in) {
    if (!in.isStdIn && in.fd != -1) {
        ::close(in.fd);
    }
    in.fd = -1;
}

bool openOutput() {
    OutputState & o = output();
    if (o.opened) {
        return o.fd != -1;
    }
    o.opened = true;
    static const bool registered = (std::atexit(atExit) == 0);
    (void)registered;
    if (OutputFileName.empty() || OutputFileName == "-") {
        o.fd = STDOUT_FILENO;
        o.shownName = "write error";
        return true;
    }
    const std::string & name = OutputFileName;
    o.shownName = name;
    // The file that a symbolic link refers to is the one replaced.
    std::string target = name;
    char resolved[PATH_MAX];
    if (::realpath(name.c_str(), resolved)) {
        target = resolved;
    }
    struct stat st;
    const bool exists = (::stat(target.c_str(), &st) == 0);
    if (exists) {
        if (S_ISDIR(st.st_mode)) {
            report(name, EISDIR);
            return false;
        }
        if (::access(target.c_str(), W_OK) != 0) {
            report(name, errno);
            return false;
        }
        if (!S_ISREG(st.st_mode)) {
            // e.g., /dev/null or a FIFO: written in place
            o.fd = ::open(target.c_str(), O_WRONLY | O_CLOEXEC);
            if (o.fd == -1) {
                report(name, errno);
            }
            return o.fd != -1;
        }
    }
    // A temporary file in the same directory, renamed into place by
    // finishOutput: an output file is never left incomplete, and may also be
    // the input.
    const auto slash = target.rfind('/');
    const std::string dir = (slash == std::string::npos) ? std::string{} : target.substr(0, slash + 1);
    const std::string base = (slash == std::string::npos) ? target : target.substr(slash + 1);
    const std::string pattern = dir + "." + base + ".XXXXXX";
    std::vector<char> temporary(pattern.begin(), pattern.end());
    temporary.push_back('\0');
    const int fd = ::mkstemp(temporary.data());
    if (fd == -1) {
        report(name, errno);
        return false;
    }
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    // The permissions of the file replaced, or those of a new file
    mode_t mode;
    if (exists) {
        mode = st.st_mode & 07777;
    } else {
        const mode_t mask = ::umask(0);
        ::umask(mask);
        mode = 0666 & ~mask;
    }
    ::fchmod(fd, mode);
    o.fd = fd;
    o.target = target;
    o.temporary = temporary.data();
    return true;
}

void setOutputFile(const std::string & name) {
    OutputFileName = name;
}

bool outputRequested() {
    return !OutputFileName.empty();
}

int outputFD() {
    OutputState & o = output();
    if (LLVM_UNLIKELY(o.finished)) {
        report_fatal_error("toolio::outputFD after finishOutput");
    }
    if (!openOutput()) {
        std::exit(EXIT_FAILURE);
    }
    return o.fd;
}

bool outputIsTerminal() {
    return ::isatty(outputFD()) == 1;
}

raw_ostream & out() {
    OutputState & o = output();
    if (!o.stream) {
        o.stream = std::make_unique<raw_fd_ostream>(outputFD(), /*shouldClose*/ false);
    }
    return *o.stream;
}

namespace {

// A std::streambuf writing to out() (which buffers).
class OutStreamBuf final : public std::streambuf {
protected:
    int_type overflow(int_type c) override {
        if (!traits_type::eq_int_type(c, traits_type::eof())) {
            out() << traits_type::to_char_type(c);
        }
        return traits_type::not_eof(c);
    }
    std::streamsize xsputn(const char * s, std::streamsize n) override {
        out().write(s, static_cast<size_t>(n));
        return n;
    }
    int sync() override {
        out().flush();
        return 0;
    }
};

} // anonymous namespace

std::ostream & stream() {
    static OutStreamBuf buffer;
    static std::ostream os(&buffer);
    return os;
}

bool finishOutput() {
    OutputState & o = output();
    if (!o.opened || o.finished) {
        return true;
    }
    o.finished = true;
    int error = WriteError.load();
    if (o.stream) {
        o.stream->flush();
        if (o.stream->has_error() && error == 0) {
            error = o.stream->error().value();
        }
        o.stream->clear_error();
        o.stream.reset();
    }
    if (!o.temporary.empty()) {
        if (::close(o.fd) != 0 && error == 0) {
            error = errno;
        }
        if (error == 0 && ::rename(o.temporary.c_str(), o.target.c_str()) != 0) {
            error = errno;
        }
        if (error != 0) {
            ::unlink(o.temporary.c_str());
        }
        o.temporary.clear();
    }
    if (error != 0) {
        report(o.shownName, error);
        return false;
    }
    return true;
}

bool useColor(const ColorMode mode, const int fd) {
    if (mode != ColorMode::Auto) {
        return mode == ColorMode::Always;
    }
    if (::isatty(fd) != 1) {
        return false;
    }
    // https://no-color.org: set and not empty
    const char * const noColor = std::getenv("NO_COLOR");
    if (noColor && *noColor) {
        return false;
    }
    const char * const term = std::getenv("TERM");
    return !(term && std::strcmp(term, "dumb") == 0);
}

}

extern "C" void toolio_write(const int fd, const void * const buf, size_t n) {
    // After an error, nothing more is written (finishOutput reports it).
    if (LLVM_UNLIKELY(toolio::WriteError.load() != 0)) {
        return;
    }
    const char * p = static_cast<const char *>(buf);
    while (n > 0) {
        const ssize_t written = ::write(fd, p, n);
        if (LLVM_UNLIKELY(written < 0)) {
            if (errno == EINTR) {
                continue;
            }
            int expected = 0;
            toolio::WriteError.compare_exchange_strong(expected, errno);
            return;
        }
        p += written;
        n -= static_cast<size_t>(written);
    }
}
