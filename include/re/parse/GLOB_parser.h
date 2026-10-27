/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <vector>
#include <boost/filesystem.hpp>
#include <re/parse/parser.h>
#include <re/parse/ERE_parser.h>

//  GLOB parsing for Filename expansion in accord with Posix rules
//  IEEE-1003.1 XCU section  2.13 Pattern Matching Notation

namespace re {
    // Posix: shell filename expansion, in which ? and * (and negated bracket
    // expressions) do not match a period beginning a path component.
    // Grep: grep's --include/--exclude patterns (fnmatch without FNM_PERIOD),
    // in which a leading period is an ordinary character.
    // GIT: .gitignore patterns, also with ordinary leading periods, and with
    // the ** forms.
    enum class GLOB_kind {Posix, Grep, GIT};
    class FileGLOB_Parser : public RE_Parser  {
    public:
        FileGLOB_Parser(const std::string & glob, GLOB_kind k = GLOB_kind::Posix) : RE_Parser(glob),
            mGLOB_kind(k), mPathComponentStartContext(true) {
            mReSyntax = RE_Syntax::FileGLOB;
        }

    protected:
        RE * parse_alt() override;
        RE * parse_seq() override;
        RE * parse_next_item() override;
        // Does a period beginning a path component require a literal match?
        bool periodRule() const {return mGLOB_kind == GLOB_kind::Posix;}
        RE * parse_bracket_expr();
        RE * range_extend(RE * e1);
    private:
        GLOB_kind mGLOB_kind;
        bool mPathComponentStartContext;
    };


enum class PatternKind {Include, Exclude};
using PatternType = std::pair<PatternKind, RE *>;
using PatternVector = std::vector<PatternType>;

PatternVector parseGitIgnoreFile(boost::filesystem::path dirpath,
                                                             std::string ignoreFileName);

}
