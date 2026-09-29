#include <re/adt/re_re.h>
#include <re/adt/adt.h>

using namespace llvm;

namespace re {

bool matchesEmptyString(const RE * re) {
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * re : *alt) {
            if (matchesEmptyString(re)) {
                return true;
            }
        }
        return false;
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * re : *seq) {
            if (!matchesEmptyString(re)) {
                return false;
            }
        }
        return true;
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        return (rep->getLB() == 0) || matchesEmptyString(rep->getRE());
    } else if (isa<Start>(re)) {
        return true;
    } else if (isa<End>(re)) {
        return true;
    } else if (const Assertion * a = dyn_cast<Assertion>(re)) {
        return a->getSense() == Assertion::Sense::Negative;
    } else if (const Diff * diff = dyn_cast<Diff>(re)) {
        return matchesEmptyString(diff->getLH()) && !matchesEmptyString(diff->getRH());
    } else if (const Intersect * e = dyn_cast<Intersect>(re)) {
        return matchesEmptyString(e->getLH()) && matchesEmptyString(e->getRH());
    } else if (isa<Any>(re)) {
        return false;
    } else if (isa<CC>(re)) {
        return false;
    } else if (const Group * g = dyn_cast<Group>(re)) {
        return matchesEmptyString(g->getRE());
    } else if (const Name * n = dyn_cast<Name>(re)) {
        return matchesEmptyString(n->getDefinition());
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        return matchesEmptyString(c->getCapturedRE());
    } else if (isa<Range, PropertyExpression, Reference, Permute, Interleavable>(re)) {
        return false;
    }
    UnexpectedRE("matchesEmptyString", re);
}

[[noreturn]] void UnsupportedRE(const std::string & errmsg) {
    llvm::report_fatal_error(llvm::StringRef(errmsg));
}

const char * getClassTypeName(RE::ClassTypeId t) {
    using T = RE::ClassTypeId;
    // No default: -Wswitch flags any RE type missing here.
    switch (t) {
        case T::Alt: return "Alt";
        case T::Any: return "Any";
        case T::Assertion: return "Assertion";
        case T::CC: return "CC";
        case T::Range: return "Range";
        case T::Diff: return "Diff";
        case T::End: return "End";
        case T::Intersect: return "Intersect";
        case T::Name: return "Name";
        case T::PropertyExpression: return "PropertyExpression";
        case T::Capture: return "Capture";
        case T::Reference: return "Reference";
        case T::Group: return "Group";
        case T::Rep: return "Rep";
        case T::Seq: return "Seq";
        case T::Start: return "Start";
        case T::Permute: return "Permute";
        case T::Interleavable: return "Interleavable";
    }
    llvm_unreachable("Unknown RE type");
}

[[noreturn]] void UnexpectedRE(const char * routine, const RE * re) {
    UnsupportedRE(std::string(routine) + ": unexpected RE type " + getClassTypeName(re->getClassTypeId()));
}

}
