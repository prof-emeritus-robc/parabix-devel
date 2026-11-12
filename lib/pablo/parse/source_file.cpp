/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <pablo/parse/source_file.h>

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Path.h>
#include <toolchain/install_paths.h>

namespace pablo {
namespace parse {

std::shared_ptr<SourceFile> SourceFile::Relative(std::string const & path) {
    llvm::SmallString<128> relative("pablo");
    llvm::sys::path::append(relative, path);
    const std::string found = codegen::findInstalledFile(codegen::InstallDir::Data, relative);
    if (found.empty()) {
        return nullptr;
    }
    try {
        return std::make_shared<SourceFile>(found);
    } catch (...) {
        return nullptr;
    }
}

std::shared_ptr<SourceFile> SourceFile::Absolute(std::string const & path) {
    try {
        return std::make_shared<SourceFile>(path);
    } catch (...) {
        return nullptr;
    }
}

bool SourceFile::nextLine(boost::string_view & view) {
    if (mCursor == mSource.end())
        return false;

    const char * start = mCursor;
    while (*mCursor != '\n' && mCursor != mSource.end())
        mCursor++;
    mCursor = std::min(mCursor + 1, mSource.end()); // advance past EOL if not at EOF
    ptrdiff_t len = mCursor - start;
    boost::string_view line(start, static_cast<size_t>(len));
    mLineRefs.push_back(line);
    view = line;
    return true;
}

boost::string_view const & SourceFile::line(size_t num) const {
    assert (num - 1 < mLineRefs.size());
    return mLineRefs[num - 1];
}

SourceFile::SourceFile(std::string const & filename)
: mFilename(filename)
, mSource(filename)
, mLineRefs()
, mCursor(mSource.data())
{
    assert (mSource.is_open());
}

SourceFile::~SourceFile() {
    mSource.close();
}

} // namespace pablo::parse
} // namespace pablo
