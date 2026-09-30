/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 */
#pragma once

#include <string>
#include <vector>
#include <re/transforms/re_transformer.h>
#include <re/adt/adt.h>
#include <ucd/core/unicode_set.h>
namespace re { class RE; }

namespace kernel { class PipelineBuilder; class StreamSet; }
using PipelineBuilder = kernel::PipelineBuilder;
using StreamSet = kernel::StreamSet;



namespace csv {


//  DoubleQuoteEscape - transform REs for matching within quoted CSV fields
//
//  When matching CSV fields in quotes, any double quote character within
//  the represented string is escaped with a second double quote character.
//  The DoubleQuoteEscape tranformer modifies a regular expression to
//  match a pair of double quotes whenever a single one is desired
//  in the represented string value.

class DoubleQuoteEscape : public re::RE_Transformer {
public:
    DoubleQuoteEscape(UCD::codepoint_t dqChar = 0x22);
protected:
    re::CC * getDQ_CC();
    re::RE * getDoubleEscape();
    re::RE * transformCC (re::CC * cc) override;
    re::RE * transformAny (re::Any * a) override;
    re::RE * transformName (re::Name * name) override;
    re::RE * transformPropertyExpression (re::PropertyExpression * pe) override;
private:
    UCD::codepoint_t mDQ;
    re::CC * mDQ_CC;
    re::RE * mDoubleEscape;
};

}