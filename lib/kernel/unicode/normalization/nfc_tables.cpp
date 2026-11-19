/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  The NFC analysis of scripts/UCD/nfc_generator.py, ported to C++ (see
 *  doc/masked-nfc-plan.md).  Each function below corresponds to the method of
 *  NFC_generator of the same name, and is called in the same order as the
 *  generator's main.  The generator's dictionaries iterate in insertion order;
 *  where a result depends on that order, the order is kept explicitly
 *  (short_composable_order, long_composable_order).
 */

#include <kernel/unicode/normalization/nfc_tables.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <stdexcept>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>
#include <ucd/data/PropertyAliases.h>
#include <ucd/data/PropertyObjects.h>
#include <ucd/data/PropertyObjectTable.h>
#include <ucd/data/PropertyValueAliases.h>

using namespace UCD;
using codepoint_t = UCD::codepoint_t;

namespace {

unsigned u8_encoded_length(codepoint_t cp) {
    if (cp <= 0x7F) return 1;
    if (cp <= 0x7FF) return 2;
    if (cp <= 0xFFFF) return 3;
    return 4;
}

unsigned u8_length(const std::u32string & s) {
    unsigned n = 0;
    for (char32_t c : s) n += u8_encoded_length(c);
    return n;
}

unsigned ceil_log2(unsigned x) {
    return static_cast<unsigned>(std::ceil(std::log2(static_cast<double>(x))));
}

//  The UCD data the analysis uses.
struct UCD_Data {
    StringPropertyObject * dm;
    UnicodeSet canonical;           // dt = Can
    UnicodeSet excluded;            // CE (CompositionExclusions.txt)
    EnumeratedPropertyObject * cccObj;
    UCD_Data() {
        dm = llvm::cast<StringPropertyObject>(getPropertyObject(property_t::dm));
        auto * dt = llvm::cast<EnumeratedPropertyObject>(getPropertyObject(property_t::dt));
        canonical = dt->GetCodepointSet(DT_ns::Can);
        excluded = llvm::cast<BinaryPropertyObject>(getPropertyObject(property_t::CE))->GetCodepointSet(Binary_ns::Y);
        cccObj = llvm::cast<EnumeratedPropertyObject>(getPropertyObject(property_t::ccc));
    }
    std::u32string decomp(codepoint_t cp) const {return dm->GetU32StringValue(cp);}
    //  The numeric combining class.
    unsigned ccc(codepoint_t cp) const {return CCC_ns::enum_val[cccObj->GetEnumerationValue(cp)];}
    bool starter(codepoint_t cp) const {return ccc(cp) == 0;}

    //  A reference NFC (UAX #15: the full canonical decomposition, the canonical
    //  ordering, and the canonical composition algorithm), for the overridable
    //  primaries (see Analysis::determine_overridable_primaries).
    static constexpr codepoint_t SBase = 0xAC00, LBase = 0x1100, VBase = 0x1161, TBase = 0x11A7;
    static constexpr unsigned LCount = 19, VCount = 21, TCount = 28, NCount = VCount * TCount, SCount = LCount * NCount;
    std::map<std::pair<codepoint_t, codepoint_t>, codepoint_t> primaryComposites;
    void initComposition() {
        for (codepoint_t cp : dm->GetExplicitCps()) {
            if (!canonical.contains(cp) || excluded.contains(cp)) continue;
            const std::u32string d = decomp(cp);
            if (d.size() != 2 || !starter(cp) || !starter(d[0])) continue;
            primaryComposites[{d[0], d[1]}] = cp;
        }
    }
    void fullDecomposition(codepoint_t cp, std::u32string & out) const {
        if (cp >= SBase && cp < SBase + SCount) {
            const unsigned s = cp - SBase;
            out += static_cast<char32_t>(LBase + s / NCount);
            out += static_cast<char32_t>(VBase + (s % NCount) / TCount);
            if (s % TCount) out += static_cast<char32_t>(TBase + s % TCount);
            return;
        }
        if (canonical.contains(cp)) {
            for (char32_t c : decomp(cp)) fullDecomposition(c, out);
            return;
        }
        out += static_cast<char32_t>(cp);
    }
    std::u32string nfc(const std::u32string & s) const {
        std::u32string cps;
        for (char32_t c : s) fullDecomposition(c, cps);
        for (size_t i = 0; i < cps.size(); ) {
            if (starter(cps[i])) {i++; continue;}
            size_t j = i;
            while (j < cps.size() && !starter(cps[j])) j++;
            std::stable_sort(cps.begin() + i, cps.begin() + j, [&](char32_t a, char32_t b) {return ccc(a) < ccc(b);});
            i = j;
        }
        std::u32string out;
        int starterPos = -1;
        int lastCCC = -1;      // of the last character appended after the starter
        for (char32_t c : cps) {
            const unsigned cc = ccc(c);
            if (starterPos >= 0) {
                const char32_t L = out[starterPos];
                const bool adjacent = static_cast<int>(out.size()) - 1 == starterPos;
                const bool blocked = !adjacent && (lastCCC == 0 || lastCCC >= static_cast<int>(cc));
                if (!blocked) {
                    if (adjacent && L >= LBase && L < LBase + LCount && c >= VBase && c < VBase + VCount) {
                        out[starterPos] = SBase + ((L - LBase) * VCount + (c - VBase)) * TCount;
                        continue;
                    }
                    if (adjacent && L >= SBase && L < SBase + SCount && (L - SBase) % TCount == 0 && c > TBase && c < TBase + TCount) {
                        out[starterPos] = L + (c - TBase);
                        continue;
                    }
                    const auto f = primaryComposites.find({static_cast<codepoint_t>(L), static_cast<codepoint_t>(c)});
                    if (f != primaryComposites.end()) {
                        out[starterPos] = f->second;
                        continue;
                    }
                }
            }
            if (cc == 0) {
                starterPos = static_cast<int>(out.size());
                lastCCC = -1;
            } else {
                lastCCC = static_cast<int>(cc);
            }
            out += c;
        }
        return out;
    }
};

class Analysis {
public:
    Analysis(NFC_Tables & t) : T(t) {}
    void run() {
        create_mappings();
        add_overridable_seconds();
        determine_overridable_primaries();
        create_max_insert_map();
        allocate_ccc_passes();
        allocate_chained_short_passes();
        implement_overridable_primaries_as_long_composables();
        create_conditional_mark_codes();
        generate_candidate_classes();
        determine_pass_applications();
    }
private:
    bool excluded_composite(codepoint_t cp) const {
        if (!U.canonical.contains(cp)) return false;
        const std::u32string decomp = U.decomp(cp);
        if (decomp.size() == 1) return false;
        if (U.excluded.contains(cp)) return true;
        if (!U.starter(cp)) return true;
        return !U.starter(decomp[0]);
    }
    void addShort(codepoint_t cp1, codepoint_t cp2, codepoint_t precomposed) {
        if (T.short_composable_map.find(cp1) == T.short_composable_map.end()) T.short_composable_order.push_back(cp1);
        T.short_composable_map[cp1][cp2] = precomposed;
    }
    void addLong(codepoint_t mark, codepoint_t starter, const std::u32string & s) {
        if (T.long_composable_map.find(mark) == T.long_composable_map.end()) T.long_composable_order.push_back(mark);
        T.long_composable_map[mark][starter] = s;
    }
    bool isShortKey(codepoint_t cp) const {return T.short_composable_map.count(cp) != 0;}

    void create_mappings() {
        std::vector<codepoint_t> cps = U.dm->GetExplicitCps();
        std::sort(cps.begin(), cps.end());
        for (codepoint_t precomposed : cps) {
            if (!U.canonical.contains(precomposed)) continue;
            std::u32string decomp = U.decomp(precomposed);
            codepoint_t cp1 = decomp[0];
            if (decomp.size() == 1) {
                T.singleton_map[precomposed] = cp1;
            } else if (decomp.size() == 2) {
                if (!U.starter(precomposed) || !U.starter(cp1)) T.non_starter_uset.insert(precomposed);
                const codepoint_t cp2 = decomp[1];
                if (excluded_composite(precomposed)) {
                    while (excluded_composite(cp1)) {
                        decomp = U.decomp(cp1) + decomp.substr(1);
                        cp1 = decomp[0];
                    }
                    T.excluded_composite_map[precomposed] = decomp;
                } else {
                    const unsigned ccc = U.ccc(cp2);
                    T.by_ccc_and_second[ccc][cp2][cp1] = precomposed;
                    if (ccc == 0) {
                        //  Decomposition to two consecutive starters.
                        T.composable_seconds.insert(cp2);
                        if (cp1 == cp2) T.self_composables[cp1] = precomposed;
                        addShort(cp1, cp2, precomposed);
                    } else {
                        addLong(cp2, cp1, std::u32string(1, static_cast<char32_t>(precomposed)));
                    }
                }
            } else {
                throw std::runtime_error("NFC_Tables: unexpected decomposition length");
            }
        }
        for (const auto & s : T.singleton_map) {
            if (excluded_composite(s.second)) T.excluded_composite_map[s.first] = T.excluded_composite_map.at(s.second);
        }
        //  (The set grows while it is tested, in the generator's order.)
        for (codepoint_t cp1 : T.short_composable_order) {
            if (!T.composable_seconds.contains(cp1)) continue;
            for (const auto & e : T.short_composable_map[cp1]) T.composable_seconds.insert(e.second);
        }
    }

    //  A BC ==> AB C
    void add_overridable_seconds() {
        auto & seconds0 = T.by_ccc_and_second[0];
        for (codepoint_t B : T.short_composable_order) {
            for (const auto & bc : T.short_composable_map[B]) {
                const codepoint_t C = bc.first, BC = bc.second;
                const auto f = seconds0.find(B);
                if (f == seconds0.end()) continue;
                for (const auto & a : f->second) {
                    const codepoint_t A = a.first, AB = a.second;
                    if (T.short_composable_map.at(A).count(BC)) continue;     // an entry already
                    auto & entry = T.short_overridable_map[A];
                    if (isShortKey(AB) && T.short_composable_map.at(AB).count(C)) {
                        entry[BC] = std::u32string(1, static_cast<char32_t>(T.short_composable_map.at(AB).at(C)));
                    } else {
                        entry[BC] = std::u32string{static_cast<char32_t>(AB), static_cast<char32_t>(C)};
                        //  A BCD => AB CD
                        if (isShortKey(BC)) {
                            for (const auto & d : T.short_composable_map.at(BC)) {
                                const codepoint_t D = d.first, BCD = d.second;
                                if (!isShortKey(C) || !T.short_composable_map.at(C).count(D)) {
                                    throw std::runtime_error("NFC_Tables: unexpected expansion");
                                }
                                entry[BCD] = std::u32string{static_cast<char32_t>(AB), static_cast<char32_t>(T.short_composable_map.at(C).at(D))};
                            }
                        }
                    }
                }
            }
        }
    }

    //  Overridable primaries: a starter P with a following mark m (that composes
    //  with some starter) whose NFC is not P m, nor the composite of P and m: m
    //  has a lower combining class than a mark of P's decomposition, so that
    //  NFC reorders and recomposes.  Each is P + m -> NFC(P m), from the
    //  reference NFC: a starter and the marks that remain, in canonical order.
    //  (The generator constructed them from the compositions of P's starter and
    //  its marks, one level deep: its results differ where P's starter is itself
    //  a composite whose mark m overrides, e.g. U+1EE1 U+0328, whose NFC is
    //  U+01EB U+031B U+0303.)
    void determine_overridable_primaries() {
        U.initComposition();
        std::vector<codepoint_t> marks = T.long_composable_order;
        std::vector<codepoint_t> cps = U.dm->GetExplicitCps();
        std::sort(cps.begin(), cps.end());
        for (codepoint_t P : cps) {
            if (!U.canonical.contains(P) || !U.starter(P)) continue;
            const std::u32string self(1, static_cast<char32_t>(P));
            if (U.nfc(self) != self) continue;        // not in NFC
            std::u32string d;
            U.fullDecomposition(P, d);
            unsigned maxCCC = 0;
            for (size_t i = 1; i < d.size(); i++) maxCCC = std::max(maxCCC, U.ccc(d[i]));
            if (maxCCC == 0) continue;
            for (codepoint_t m : marks) {
                if (U.ccc(m) >= maxCCC) continue;
                const std::u32string input{static_cast<char32_t>(P), static_cast<char32_t>(m)};
                const std::u32string R = U.nfc(input);
                if (R == input) continue;
                const auto & withM = T.long_composable_map.at(m);
                const auto f = withM.find(P);
                if (f != withM.end() && f->second == R) continue;    // an ordinary composition
                T.overridable_primaries[P][m] = R;
            }
        }
    }

    //  The longest UTF-8 encoding of a composite that a starter may become by the
    //  compositions that follow it (each written in place of the starter), with
    //  the marks that an override leaves after it.
    unsigned maxReach(codepoint_t s, std::map<codepoint_t, unsigned> & memo, unsigned depth = 0) {
        const auto f = memo.find(s);
        if (f != memo.end()) return f->second;
        if (depth > 16) throw std::runtime_error("NFC_Tables: a cycle of compositions");
        unsigned best = u8_encoded_length(s);
        for (const auto & m : T.long_composable_map) {
            const auto e = m.second.find(s);
            if (e != m.second.end() && e->second.size() == 1) best = std::max(best, maxReach(e->second[0], memo, depth + 1));
        }
        const auto o = T.overridable_primaries.find(s);
        if (o != T.overridable_primaries.end()) {
            for (const auto & r : o->second) {
                best = std::max(best, maxReach(r.second[0], memo, depth + 1) + u8_length(r.second.substr(1)));
            }
        }
        memo[s] = best;
        return best;
    }

    void update_max_insert_map(codepoint_t cp, int insert_len) {
        const auto f = T.max_insert_map.find(cp);
        if (f != T.max_insert_map.end()) {
            if (insert_len > static_cast<int>(f->second)) f->second = insert_len;
        } else if (insert_len > 0) {
            T.max_insert_map[cp] = insert_len;
        }
    }

    void create_max_insert_map() {
        for (const auto & s : T.short_composable_map) {
            for (const auto & e : s.second) {
                const int ldiff = static_cast<int>(u8_encoded_length(e.second)) - static_cast<int>(u8_encoded_length(s.first));
                if (ldiff > 0) update_max_insert_map(s.first, ldiff);
            }
        }
        for (const auto & m : T.long_composable_map) {
            for (const auto & s : m.second) {
                const int ldiff = static_cast<int>(u8_length(s.second)) - static_cast<int>(u8_encoded_length(s.first));
                if (ldiff > 0) update_max_insert_map(s.first, ldiff);
            }
        }
        for (const auto & e : T.excluded_composite_map) {
            const int ldiff = static_cast<int>(u8_length(e.second)) - static_cast<int>(u8_encoded_length(e.first));
            if (ldiff > 0) update_max_insert_map(e.first, ldiff);
        }
        //  A starter of long compositions: room for the longest composite it may
        //  become, written in place of it (in UTF-8 mode).  The generator gave it
        //  room for one composition, the following compositions growing into the
        //  bytes of the marks deleted; but a mark not yet composed (of a higher
        //  combining class, before the one composed) may lie between, as in
        //  c U+0301 U+0327, whose ç then grows into U+0301 as U+1E09.
        std::map<codepoint_t, unsigned> reach;
        for (const auto & m : T.long_composable_map) {
            for (const auto & st : m.second) {
                const int ldiff = static_cast<int>(maxReach(st.first, reach)) - static_cast<int>(u8_encoded_length(st.first));
                if (ldiff > 0) update_max_insert_map(st.first, ldiff);
            }
        }
        //  An override writes its starter, then (right-aligned in the region of
        //  P and its insertion) the marks that remain: room for the composites
        //  its starter may become in the passes that follow (see maxReach and
        //  the kernels' overrideTarget).
        std::map<codepoint_t, unsigned> memo;
        for (const auto & o : T.overridable_primaries) {
            for (const auto & m : o.second) {
                const unsigned needed = maxReach(m.second[0], memo) + u8_length(m.second.substr(1));
                const int ldiff = static_cast<int>(needed) - static_cast<int>(u8_encoded_length(o.first));
                if (ldiff > 0) update_max_insert_map(o.first, ldiff);
            }
        }
        for (const auto & s : T.singleton_map) {
            int ldiff = static_cast<int>(u8_encoded_length(s.second)) - static_cast<int>(u8_encoded_length(s.first));
            const auto f = T.max_insert_map.find(s.second);
            if (f != T.max_insert_map.end()) ldiff += f->second;
            if (ldiff > 0) update_max_insert_map(s.first, ldiff);
        }
    }

    //  A starter with precompositions with marks of several combining classes
    //  needs them in separate passes, so that the mark of the lowest class is
    //  chosen.
    void allocate_ccc_passes() {
        std::map<codepoint_t, std::set<unsigned>> ccc_set_by_starter;
        std::map<unsigned, std::set<unsigned>> ccc_conflict_map;
        for (codepoint_t mark : T.long_composable_order) {
            const unsigned ccc = U.ccc(mark);
            ccc_conflict_map[ccc];
            for (const auto & s : T.long_composable_map.at(mark)) {
                auto f = ccc_set_by_starter.find(s.first);
                if (f == ccc_set_by_starter.end()) {
                    ccc_set_by_starter[s.first] = {ccc};
                } else if (!f->second.count(ccc)) {
                    for (unsigned other : f->second) {
                        if (other < ccc) ccc_conflict_map[ccc].insert(other); else ccc_conflict_map[other].insert(ccc);
                    }
                    f->second.insert(ccc);
                }
            }
        }
        for (const auto & c : ccc_conflict_map) {
            unsigned min_pass = 0;
            for (unsigned other : c.second) {
                if (T.ccc_pass_allocation.at(other) >= min_pass) {
                    min_pass = T.ccc_pass_allocation.at(other) + 1;
                    if (min_pass >= T.pass_count) T.pass_count = min_pass + 1;
                }
            }
            while (T.pass_cccs.size() <= min_pass) T.pass_cccs.emplace_back();
            T.pass_cccs[min_pass].push_back(c.first);
            T.ccc_pass_allocation[c.first] = min_pass;
        }
    }

    void allocate_chained_short_passes() {
        using RuleMap = std::map<codepoint_t, std::map<codepoint_t, std::u32string>>;
        using Deps = std::map<codepoint_t, std::vector<std::pair<codepoint_t, codepoint_t>>>;
        bool self_composable_pass_found = false;
        Deps dependence_map;
        RuleMap all_rule_map;
        for (codepoint_t X : T.short_composable_order) {
            all_rule_map[X];
            for (const auto & e : T.short_composable_map.at(X)) {
                const codepoint_t Y = e.first, precomposed = e.second;
                if (X == Y) continue;       // skip self-composables
                all_rule_map[X][Y] = std::u32string(1, static_cast<char32_t>(precomposed));
                if (isShortKey(Y)) dependence_map[Y].emplace_back(X, Y);
                if (isShortKey(precomposed)) dependence_map[precomposed].emplace_back(X, Y);
            }
        }
        for (const auto & o : T.short_overridable_map) {
            const codepoint_t X = o.first;
            for (const auto & e : o.second) {
                const codepoint_t Y = e.first;
                all_rule_map[X][Y] = e.second;
                if (isShortKey(Y)) dependence_map[Y].emplace_back(X, Y);
                const codepoint_t final_cp = e.second.back();
                if (isShortKey(final_cp)) dependence_map[final_cp].emplace_back(X, Y);
            }
        }
        //  All rules without a prior dependence are allocated to pass 0.
        unsigned pass_no = 0;
        RuleMap remaining_rule_map;
        T.short_pass_map[0];
        for (const auto & r : all_rule_map) {
            if (dependence_map.count(r.first)) remaining_rule_map[r.first] = r.second; else T.short_pass_map[0][r.first] = r.second;
        }
        T.short_pass_count = 1;
        while (!remaining_rule_map.empty()) {
            const unsigned prior_pass = pass_no;
            pass_no++;
            Deps remaining_dependence_map;
            for (const auto & r : remaining_rule_map) {
                for (const auto & uv : dependence_map.at(r.first)) {
                    const auto & prior = T.short_pass_map[prior_pass];
                    const auto f = prior.find(uv.first);
                    if (f == prior.end() || !f->second.count(uv.second)) remaining_dependence_map[r.first].push_back(uv);
                }
            }
            const RuleMap rule_map = remaining_rule_map;
            dependence_map = remaining_dependence_map;
            auto & pass = T.short_pass_map[pass_no];
            remaining_rule_map.clear();
            bool self_composable_pass = false;
            if (!self_composable_pass_found) {
                self_composable_pass = true;
                for (const auto & sc : T.self_composables) {
                    const auto f = dependence_map.find(sc.first);
                    if (f == dependence_map.end() || f->second != std::vector<std::pair<codepoint_t, codepoint_t>>{{sc.first, sc.second}}) {
                        self_composable_pass = false;
                    }
                }
                if (self_composable_pass) {
                    T.self_composable_pass_no = static_cast<int>(pass_no);
                    for (const auto & r : rule_map) {
                        const codepoint_t X = r.first;
                        const auto sc = T.self_composables.find(X);
                        if (sc != T.self_composables.end()) {
                            const codepoint_t XX = sc->second;
                            pass[X][XX] = std::u32string{static_cast<char32_t>(XX), static_cast<char32_t>(X)};
                            pass[X][X] = std::u32string(1, static_cast<char32_t>(XX));
                            for (const auto & y : r.second) {
                                if (y.first != X && y.first != XX) remaining_rule_map[X][y.first] = y.second;
                            }
                        } else {
                            remaining_rule_map[X] = r.second;
                        }
                    }
                    self_composable_pass_found = true;
                }
            }
            if (!self_composable_pass) {
                for (const auto & r : rule_map) {
                    if (dependence_map.count(r.first)) remaining_rule_map[r.first] = r.second; else pass[r.first] = r.second;
                }
                if (pass.empty()) remaining_rule_map.clear();
            }
            T.short_pass_count = pass_no + 1;
        }
    }

    void implement_overridable_primaries_as_long_composables() {
        for (const auto & o : T.overridable_primaries) {
            for (const auto & m : o.second) addLong(m.first, o.first, m.second);
        }
    }

    void create_conditional_mark_codes() {
        for (unsigned p = 0; p < T.pass_count; p++) T.max_conditional_code_bits[p] = 1;
        //  pass -> prefix code -> marks, in the generator's order.
        std::map<unsigned, std::map<unsigned, std::vector<codepoint_t>>> pass_data;
        for (codepoint_t mark : T.long_composable_order) {
            const unsigned pass_code = T.ccc_pass_allocation.at(U.ccc(mark));
            std::set<unsigned> pfx_codes;
            for (const auto & s : T.long_composable_map.at(mark)) pfx_codes.insert(NFC_Tables::prefixCode(s.first));
            for (unsigned pfx : pfx_codes) pass_data[pass_code][pfx].push_back(mark);
        }
        for (unsigned p = 0; p < T.pass_count; p++) {
            for (const auto & pd : pass_data[p]) {
                const std::vector<codepoint_t> & mark_list = pd.second;
                if (mark_list.size() == 1) continue;   // an unconditional mark for this starter range
                auto & codes = T.conditional_mark_codes[p][pd.first];
                const unsigned bits = ceil_log2(static_cast<unsigned>(mark_list.size()) + 1);    // code 0 reserved
                const unsigned mask = (1u << bits) - 1;
                std::set<unsigned> allocated{0};
                std::vector<codepoint_t> unassigned;
                for (codepoint_t mark : mark_list) {
                    const unsigned potential = mark & mask;
                    if (allocated.count(potential)) {
                        unassigned.push_back(mark);
                    } else {
                        codes[mark] = potential;
                        allocated.insert(potential);
                    }
                }
                for (codepoint_t mark : unassigned) {
                    for (unsigned code = 1; code < (1u << bits); code++) {
                        if (allocated.count(code)) continue;
                        codes[mark] = code;
                        allocated.insert(code);
                        break;
                    }
                }
                if (T.max_conditional_code_bits[p] < bits) T.max_conditional_code_bits[p] = bits;
            }
        }
    }

    void generate_candidate_classes() {
        //  Non-starter decompositions are either marks or generate marks.
        UnicodeSet candidate = T.non_starter_uset + T.composable_seconds;
        candidate = candidate + (UnicodeSet(0, 0x10FFFF) - U.cccObj->GetCodepointSet(CCC_ns::NR));
        for (const auto & sc : T.self_composables) candidate.insert(sc.second);
        //  The Hangul V and T composables.
        candidate = candidate + UnicodeSet(0x1161, 0x1161 + 21 - 1) + UnicodeSet(0x11A7, 0x11A7 + 28 - 1);
        T.candidate_class = candidate;
        UnicodeSet expansion;
        for (const auto & s : T.singleton_map) expansion.insert(s.first);
        for (const auto & e : T.excluded_composite_map) expansion.insert(e.first);
        T.expansion_required_class = expansion;
    }

    //  The applications of each long-composition pass: the longest chain of
    //  compositions with marks of its combining classes, starting from a starter
    //  that is not itself the result of such a composition.
    void determine_pass_applications() {
        T.pass_applications.assign(T.pass_count, 1);
        for (unsigned p = 0; p < T.pass_count; p++) {
            //  starter -> the single-character results with marks of the pass.
            std::map<codepoint_t, std::set<codepoint_t>> next;
            for (const auto & m : T.long_composable_map) {
                const auto f = T.ccc_pass_allocation.find(U.ccc(m.first));
                if (f == T.ccc_pass_allocation.end() || f->second != p) continue;
                for (const auto & s : m.second) {
                    if (s.second.size() == 1) next[s.first].insert(s.second[0]);
                }
            }
            std::map<codepoint_t, unsigned> depth;
            std::function<unsigned (codepoint_t, unsigned)> longest = [&](codepoint_t cp, unsigned guard) -> unsigned {
                const auto d = depth.find(cp);
                if (d != depth.end()) return d->second;
                if (guard > 64) throw std::runtime_error("NFC_Tables: a cycle of compositions");
                unsigned best = 0;
                const auto n = next.find(cp);
                if (n != next.end()) {
                    for (codepoint_t r : n->second) best = std::max(best, 1 + longest(r, guard + 1));
                }
                depth[cp] = best;
                return best;
            };
            unsigned applications = 1;
            for (const auto & n : next) applications = std::max(applications, longest(n.first, 0));
            T.pass_applications[p] = applications;
        }
    }

    NFC_Tables & T;
    UCD_Data U;
};

//  The text form of the dump.
std::string hexSeq(const std::u32string & s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (i) r += " ";
        char buf[16];
        snprintf(buf, sizeof(buf), "%x", static_cast<unsigned>(s[i]));
        r += buf;
    }
    return r;
}

std::string hexRanges(const UnicodeSet & set) {
    std::string r;
    for (const auto & range : set) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s%x-%x", r.empty() ? "" : " ", static_cast<unsigned>(range.first), static_cast<unsigned>(range.second));
        r += buf;
    }
    return r;
}

}

unsigned NFC_Tables::prefixCode(codepoint_t cp) {
    unsigned initial;
    switch (u8_encoded_length(cp)) {
        case 1: initial = cp; break;
        case 2: initial = 0xC0 | (cp >> 6); break;
        case 3: initial = 0xE0 | (cp >> 12); break;
        default: initial = 0xF0 | (cp >> 18); break;
    }
    if (initial <= 0x7F) return 0;
    if (initial <= 0xC3) return 0xC2;
    if (initial <= 0xDF) return initial & 0xFC;
    return initial;
}

const NFC_Tables & NFC_Tables::get() {
    static const NFC_Tables tables = [] {
        NFC_Tables t;
        Analysis(t).run();
        return t;
    }();
    return tables;
}

void NFC_Tables::dump(llvm::raw_ostream & out) const {
    auto hex = [](unsigned v) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%x", v);
        return std::string(buf);
    };
    for (const auto & s : singleton_map) out << "singleton " << hex(s.first) << " " << hex(s.second) << "\n";
    for (const auto & e : excluded_composite_map) out << "excluded " << hex(e.first) << " : " << hexSeq(e.second) << "\n";
    for (const auto & s : short_composable_map) {
        for (const auto & e : s.second) out << "short " << hex(s.first) << " " << hex(e.first) << " " << hex(e.second) << "\n";
    }
    for (const auto & s : self_composables) out << "self " << hex(s.first) << " " << hex(s.second) << "\n";
    for (const auto & m : long_composable_map) {
        for (const auto & s : m.second) out << "long " << hex(m.first) << " " << hex(s.first) << " : " << hexSeq(s.second) << "\n";
    }
    out << "nonstarter " << hexRanges(non_starter_uset) << "\n";
    out << "seconds " << hexRanges(composable_seconds) << "\n";
    for (const auto & a : short_overridable_map) {
        for (const auto & e : a.second) out << "shortoverride " << hex(a.first) << " " << hex(e.first) << " : " << hexSeq(e.second) << "\n";
    }
    for (const auto & o : overridable_primaries) {
        for (const auto & m : o.second) out << "override " << hex(o.first) << " " << hex(m.first) << " : " << hexSeq(m.second) << "\n";
    }
    for (const auto & m : max_insert_map) out << "maxinsert " << hex(m.first) << " " << m.second << "\n";
    for (const auto & c : ccc_pass_allocation) out << "cccpass " << c.first << " " << c.second << "\n";
    out << "passcount " << pass_count << "\n";
    for (const auto & p : short_pass_map) {
        for (const auto & x : p.second) {
            for (const auto & y : x.second) {
                out << "shortpass " << p.first << " " << hex(x.first) << " " << hex(y.first) << " : " << hexSeq(y.second) << "\n";
            }
        }
    }
    out << "selfpass " << self_composable_pass_no << "\n";
    out << "shortpasscount " << short_pass_count << "\n";
    for (const auto & p : conditional_mark_codes) {
        for (const auto & f : p.second) {
            for (const auto & m : f.second) {
                out << "markcode " << p.first << " " << hex(f.first) << " " << hex(m.first) << " " << m.second << "\n";
            }
        }
    }
    for (const auto & b : max_conditional_code_bits) out << "markbits " << b.first << " " << b.second << "\n";
    out << "candidate " << hexRanges(candidate_class) << "\n";
    out << "expansion " << hexRanges(expansion_required_class) << "\n";
    for (unsigned p = 0; p < pass_applications.size(); p++) out << "applications " << p << " " << pass_applications[p] << "\n";
}
