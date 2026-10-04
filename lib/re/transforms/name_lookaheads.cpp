/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/transforms/name_lookaheads.h>

#include <llvm/Support/Casting.h>
#include <llvm/Support/ErrorHandling.h>
#include <re/analysis/re_analysis.h>
#include <re/adt/adt.h>
#include <re/printer/re_printer.h>
#include <kernel/core/kernel.h>

using namespace llvm;

namespace re {

RE * LookAheadNamer::transformAssertion (Assertion * a) {
    RE * x0 = a->getAsserted();
    RE * x = transform(x0);
    if ((a->getKind() == Assertion::Kind::LookAhead) && !isa<Name>(x)) {
        auto a_range = getLengthRange(x0, &mAlphabet);
        if ((&mAlphabet == &cc::Unicode) && isUTF8EncodedCharacter(x0)) {
            // One character, encoded in UTF-8 code units: compiled in place
            // with Unicode indexing, as grepOffset expects.
            a_range = std::make_pair(1, 1);
        }
        std::vector<LookaheadSegment> segments;
        if ((a_range.first != a_range.second) && !hasUniquePrefix(x) &&
                (&mAlphabet == &cc::Unicode) && parseLookaheadChain(x, &mAlphabet, segments)) {
            // A lookahead chain (e.g. B*C D*E), compiled directly as an external
            // (with one position per character).  The name identifies it as such.
            RE * a1 = (x == x0) ? a : makeAssertion(x, a->getKind(), a->getSense());
            return createName(LookaheadChainPrefix + kernel::Kernel::getStringHash(Printer_RE::PrintRE(a1)), a1);
        } else if (a_range.first != a_range.second) {
            RE * prefix, * suffix;
            std::tie(prefix, suffix) = ParseUniquePrefix(x);
            if (isEmptySeq(prefix) || isEmptySeq(suffix)) {
                llvm::report_fatal_error("Unsupported lookahead assertion");
            }
            std::string prefixName = Printer_RE::PrintRE(prefix);
            Name * pfx = makeName(prefixName, prefix);
            // Keep the assertion (and its sense): the name stands for a
            // zero-width lookahead whose asserted RE begins with the named prefix.
            RE * xfrmd = makeAssertion(makeSeq({pfx, suffix}), a->getKind(), a->getSense());
            return createName(Printer_RE::PrintRE(xfrmd), xfrmd);
        } else if (a_range.first > mMaxLookahead) {
            // Fixed length RE 
            RE * a1 = a;
            if (x != x0) {
                a1 = makeAssertion(x, a->getKind(), a->getSense());
            }
            std::string name = Printer_RE::PrintRE(a1);
            return createName(name, a1);
        }
    }
    if (x == x0) return a;
    return makeAssertion(x, a->getKind(), a->getSense());
}

bool isLookaheadChainName(const Name * n) {
    return n->getFullName().compare(0, LookaheadChainPrefix.size(), LookaheadChainPrefix) == 0;
}

 unsigned NamedLookAheadAmount(const Name * n, const cc::Alphabet & alpha) {
    RE * defn = n->getDefinition();
    if (defn == nullptr) {
        llvm::report_fatal_error("Undefined name");
    }
    if (isLookaheadChainName(n)) {
        // The external marks the first character after the lookahead point.
        return 1;
    }
    if (const Assertion * a = dyn_cast<Assertion>(defn)) {
        if (a->getKind() == Assertion::Kind::LookAhead) {
            const RE * asserted = a->getAsserted();
            auto a_range = getLengthRange(asserted, &alpha);
            if (a_range.first == a_range.second) {
                // fixed length RE
                return a_range.second;
            }
            if (const Seq * seq = dyn_cast<Seq>(asserted)) {
                // Expecting a unique prefix as the first element
                auto a_range = getLengthRange(seq->front(), &alpha);
                if (a_range.first == a_range.second) {
                    return a_range.second;
                }
            }
            llvm::report_fatal_error("Unsupported lookahead assertion");
        }
    }
    return 0;  // Not a lookahead
}

}
