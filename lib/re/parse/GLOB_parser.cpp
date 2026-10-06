/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/parse/GLOB_parser.h>

#include <fstream>
#include <re/adt/re_cc.h>
#include <re/adt/re_range.h>
#include <re/adt/re_alt.h>
#include <re/adt/re_seq.h>
#include <re/adt/re_diff.h>
#include <re/adt/re_rep.h>
#include <re/adt/re_start.h>
#include <re/adt/re_end.h>
#include <re/adt/re_utility.h>

namespace re {

//  GLOB parsing for Filename expansion in accord with Posix rules
//  IEEE-1003.1 XCU section  2.13 Pattern Matching Notation

// GLOB parsing:  no alternation operators.
RE * FileGLOB_Parser::parse_alt() {
    return parse_seq();
}

RE * FileGLOB_Parser::parse_seq() {
    mPathComponentStartContext = true;
    std::vector<RE *> seq;
    if (!mCursor.more()) return Seq::Create();
    for (;;) {
        RE * re = parse_next_item();
        if (re == nullptr) {
            break;
        }
        seq.push_back(re);
    }
    return Seq::Create(seq.begin(), seq.end());
}

const codepoint_t NUL = 0;

// We use symbolic constants for the path separator characters.
// PATTERN_PATH_SEPARATOR is the path separator character in GLOB patterns.
// FILE_PATH_SEPARATOR is the path separator in actual filenames.
//
const codepoint_t PATTERN_PATH_SEPARATOR = '/';
#ifdef WINDOWS
const codepoint_t FILE_PATH_SEPARATOR = '\\';
#else
const codepoint_t FILE_PATH_SEPARATOR = '/';
#endif

//
// GLOB metacharacters:
//  ? stands for any one character, except as below
//  * stands for zero or more characters, except as below
//  Exceptions:
//     Neither ? nore * may match any of the following.
//     (a) a NUL character.
//     (b) a slash ('/')
//     (c) a period ('.') when it appears as the first character of a
//         filename or path component (immediately after a '/'), for the
//         Posix GLOB kind only.

static RE * makeAnyFileCC() {
    return makeComplement(CC::Create(CC::Create(NUL), CC::Create(FILE_PATH_SEPARATOR)));
}

//  Allowing PATH separator for /**/ in GLOB_kind == GIT mode.
static RE * makeAnyPathCC() {
    return makeComplement(CC::Create(NUL));
}

static RE * makeAnyButDot() {
    return makeComplement(CC::Create(CC::Create(CC::Create(NUL), CC::Create(FILE_PATH_SEPARATOR)), CC::Create('.')));
}

RE * FileGLOB_Parser::parse_next_item() {
    if (mCursor.noMore()) return nullptr;
    if (accept('?')) {
        const bool componentStart = mPathComponentStartContext;
        mPathComponentStartContext = false;  // After this ? metacharacter.
        return (componentStart && periodRule()) ? makeAnyButDot() : makeAnyFileCC();
    } else if (accept('*')) {
        // Check for pattern beginning **/, containing /**/, or ending /**, and
        // process according to GIT special GLOB rules if required.
        if ((mGLOB_kind == GLOB_kind::GIT) && mPathComponentStartContext && accept('*')) {
            if (mCursor.noMore() || at(PATTERN_PATH_SEPARATOR))
                return Rep::Create(makeAnyPathCC(), 0, Rep::UNBOUNDED_REP);
        }
        // Otherwise fall through and ignore redundant * characters (normal GLOB rule).
        // Then check for a ? character.
        while (accept('*')) {}
        if (at('?')) {
            // Accept the ? and build the ?* regexp in place of *?
            RE * first = parse_next_item();  // Checks/sets mPathComponentStartContext as required.
            return Seq::Create({first, Rep::Create(makeAnyFileCC(), 0, Rep::UNBOUNDED_REP)});
        } else if (mPathComponentStartContext && periodRule()) {
            mPathComponentStartContext = false;   // After this * metacharacter.
            return Rep::Create(Seq::Create({makeAnyButDot(), Rep::Create(makeAnyFileCC(), 0, Rep::UNBOUNDED_REP)}), 0, 1);
        } else {
            mPathComponentStartContext = false;
            return Rep::Create(makeAnyFileCC(), 0, Rep::UNBOUNDED_REP);
        }
    } else if (accept('\\')) {
        mPathComponentStartContext = at('/');
        return CC::Create(parse_literal_codepoint());
    } else if (accept('[')) {
        RE * cc = parse_bracket_expr();
        mPathComponentStartContext = false; // After the bracket expression, which cannot match /
        return cc;
    } else if (accept(PATTERN_PATH_SEPARATOR)) {
        mPathComponentStartContext = true;
        return CC::Create(FILE_PATH_SEPARATOR);
    } else {
        mPathComponentStartContext = false;
        return CC::Create(parse_literal_codepoint());
    }
}

// Parsing items within a bracket expression.
// Items represent individual characters or sets of characters.
// Ranges may be formed by individual character items separated by '-'.
// GLOB notation uses the "!" character for set negation.
// Note that there are no backslash escapes for GLOB bracket expressions.
RE * FileGLOB_Parser::parse_bracket_expr () {
    bool negated = accept('!');
    std::vector<RE *> items;
    do {
        if (accept(PATTERN_PATH_SEPARATOR)) {
            // A path separator ('/') is not legal in a GLOB bracket expr.  If a
            // found, the items and the bracket expr are interpreted as ordinary characters.
            // See section 2.13.3
            std::vector<RE *> seqItems;
            seqItems.push_back(CC::Create('['));
            if (negated) seqItems.push_back(CC::Create('!'));
            for (auto a : items) {
                if (Range * rg = llvm::dyn_cast<Range>(a)) {
                    seqItems.push_back(rg->getLo());
                    seqItems.push_back(CC::Create('-'));
                    seqItems.push_back(rg->getHi());
                } else {
                    seqItems.push_back(a);
                }
            }
            seqItems.push_back(CC::Create(FILE_PATH_SEPARATOR));
            return Seq::Create(seqItems.begin(), seqItems.end());
        }
        if (accept('[')) {
            if (accept('=')) items.push_back(parse_equivalence_class());
            else if (accept('.')) items.push_back(range_extend(parse_collation_element()));
            else if (accept(':')) items.push_back(parse_Posix_class());
            else items.push_back(parse_bracket_expr());
        } else {
            items.push_back(range_extend(CC::Create(parse_literal_codepoint())));
        }
    } while (mCursor.more() && !at(']'));
    require(']');
    if (negated) {
        if (mPathComponentStartContext && periodRule()) {
            // In mPathComponentStartContext, a dot cannot be matched by a negated bracket expression
            items.push_back(CC::Create('.'));
        }
        return makeComplement(Alt::Create(items.begin(), items.end()));
    }
    else {
        RE * t = Alt::Create(items.begin(), items.end());
        if (mPathComponentStartContext && periodRule()) {
            t = Diff::Create(t, CC::Create('.'));
        }
        return t;
    }
}

//  Given an individual character expression, check for and parse
//  a range extension if one exists, or return the individual expression.
RE * FileGLOB_Parser::range_extend(RE * char_expr1) {
    // A range extension is signalled by a hyphen '-'.
    if (!accept('-')) return char_expr1;
    RE * char_expr2 = nullptr;
    if (accept('[')) {
        if (accept('.')) char_expr2 = parse_collation_element();
        else ParseFailure("Error in range expression");
    } else {
        char_expr2 = CC::Create(parse_literal_codepoint());
    }
    return Range::Create(char_expr1, char_expr2);
}

// Parsing a file using .gitignore conventions, returning a vector of REs.
//
// Lines beginning "!" are ignore overrides, these are returned as negative lookbehind assertions.
// Lines beginning "/" are anchored to the given directory path.
// Lines ending "/" match directories only (directory entries in the search buffer
//    must include a trailing slash.)
//
namespace fs = boost::filesystem;

PatternVector parseGitIgnoreFile(fs::path dirpath, std::string ignoreFileName) {
    PatternVector ignoreREs;
    fs::path ignoreFilePath = dirpath/ignoreFileName;
    std::ifstream ignoreFile(ignoreFilePath.string());
    if (ignoreFile.is_open()) {
        std::string line;
        while (std::getline(ignoreFile, line)) {
            bool is_local_pattern = false;
            bool is_directory_only_pattern = false;
            bool is_include_override = false;
            if (line.empty() || (line[0] == '#')) continue;  // skip empty and comment lines.
            unsigned line_start = 0;
            unsigned line_end = line.size() - 1;
            while ((line[line_end] == ' ') && (line_end > 0)) {
                line_end--;
            }
            if (line[line_end] == ' ') continue;  // skip blank lines (only spaces).
            if (line[line_end] == '\\') {
                // Escape character found, but is it an escaped escape (\\)?
                // Determine whether we have an odd or even number of escapes.
                unsigned escape_pos = line_end;
                while ((escape_pos > 0) && (line[escape_pos-1] == '\\')) {
                    escape_pos--;
                }
                if (((line_end - escape_pos) & 1) == 1) {
                    // Odd number of escapes - the final space is escaped, not trimmed.
                    line_end++;
                }
            }
            if (line[line_end] == '/') {
                is_directory_only_pattern = true;
            }
            if (line[0] == '!') { // negated ignore is an overriding include.
                is_include_override = true;
                line_start++;
            }
            // Convert any local patterns to full path patterns.
            if (line[0] == '/') {
                is_local_pattern = true;
                line_start++;
            }
            if ((line_start != 0) || (line_end != line.size() - 1)) {
                line = line.substr(line_start, line_end - line_start + 1);
            }
            RE * lineRE = RE_Parser::parse(line, DEFAULT_MODE, RE_Syntax::GitGLOB);
            if (is_local_pattern) {
                // The full path must be matched.
                lineRE = Seq::Create({Start::Create(), lineRE});
            }
            else {
                // Ensure that the pattern matches a full path component.
                lineRE = Seq::Create({Alt::Create({Start::Create(), CC::Create('/')}), lineRE});
            }
            if (is_directory_only_pattern) {
                // The full path must be matched including the trailing slash.
                lineRE = Seq::Create({lineRE, End::Create()});
            } else {
                // Match both files and directories
                lineRE = Seq::Create({lineRE, Rep::Create(CC::Create('/'), 0, 1), End::Create()});
            }
            ignoreREs.push_back(std::make_pair(is_include_override ? PatternKind::Include : PatternKind::Exclude, lineRE));
        }
        ignoreFile.close();
    }
    return ignoreREs;
}

}
