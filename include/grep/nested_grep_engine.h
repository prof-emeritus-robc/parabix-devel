#include "grep_engine.h"
#include <map>
#include <string>

namespace grep {

class NestedInternalSearchEngine {
    typedef void (*GrepFunctionType)(const char * buffer, const size_t length, MatchAccumulator &);
public:

    NestedInternalSearchEngine(BaseDriver & driver);

    ~NestedInternalSearchEngine();

    void setRecordBreak(GrepRecordBreakKind b) {mGrepRecordBreak = b;}

    // Push a level of patterns, applied in order to the selection of the
    // nearest enclosing level that is not a filter (or to all records): its
    // includes add records and its excludes remove them.  An empty level
    // that is not a filter selects what the level below it selects.  A filter level is
    // instead applied on its own, and the selection of every level is
    // restricted to the records selected by the filter levels below it
    // (e.g. command line --include/--exclude options, which .gitignore
    // patterns cannot override).
    void push(const re::PatternVector & REs, bool filter = false);

    void pop();

    void doGrep(const char * search_buffer, size_t bufferLength, MatchAccumulator & accum);

private:
    GrepRecordBreakKind mGrepRecordBreak;
    BaseDriver & mGrepDriver;

    std::vector<GrepFunctionType>   mMainMethod;
    std::vector<kernel::Kernel *>   mNested;    // nullptr for an empty level
    std::vector<bool>               mIsFilter;  // parallel to mNested

    // The levels compiled so far, by what determines them (see push), so
    // that a level identical to an earlier one is not compiled again.
    struct CompiledLevel {
        kernel::Kernel * kernel;
        GrepFunctionType main;
    };
    std::map<std::string, CompiledLevel> mCompiledLevels;

};


}
