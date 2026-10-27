/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once
#include <string>
#include <utility>
#include <vector>
#include <re/transforms/re_transformer.h>
#include <re/transforms/name_intro.h>
#include <re/alphabet/alphabet.h>
namespace re { class Assertion; }
namespace re { class Name; }
namespace re { class RE; }
namespace cc { class Alphabet; }

namespace re {

/* Transform a regular expression r so that all lookahead
   assertions of length more than maxLookahead are transformed into
   names to be externally defined.  Lookaheads that cannot be compiled are
   left in place and recorded in mUnsupported, with the reasons. */
class LookAheadNamer final : public NameIntroduction {
public:
    LookAheadNamer(const cc::Alphabet & alpha = cc::Unicode, unsigned maxLookahead = 1) : 
         NameIntroduction("LookAheadNamer"), mAlphabet(alpha), mMaxLookahead(maxLookahead) {}
    RE * transformAssertion (Assertion * a) override;
    std::vector<std::pair<Assertion *, std::string>> mUnsupported;
private:
   const cc::Alphabet & mAlphabet;
   const unsigned mMaxLookahead;
};

/*  Lookaheads compiled as lookahead chains (see parseLookaheadChain) are
    named by the LookAheadNamer with this prefix. */
const std::string LookaheadChainPrefix = "LAchain_";

/*  Lookaheads matched by reversal (see RE_PipelineBuilder::reversedStarts)
    are named with this prefix, before the other passes. */
const std::string ReversedLookaheadPrefix = "LArev_";

/*  Lookbehinds of an RE matched by reversal that are matched forward, as
    externals of the backward pass (see RE_PipelineBuilder::reversedStarts),
    are named with this prefix. */
const std::string ForwardLookbehindPrefix = "LBfwd_";
}

