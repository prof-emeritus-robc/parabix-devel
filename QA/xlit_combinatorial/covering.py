#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  t-way covering arrays by the IPOG strategy (Lei et al., "IPOG: A General
#  Strategy for T-Way Software Testing", 2007), as NIST's ACTS builds them:
#  every combination of values of any t parameters appears in some row.
#
#  The parameters are grown one at a time: the first t are covered by all
#  their combinations, and each further parameter is added to the existing
#  rows (horizontal growth, each row taking the value that covers the most
#  uncovered combinations), then to new rows for the combinations still
#  uncovered (vertical growth, filling the unset entries of rows where it
#  can).  A constraint, a function of a partial row (a dict of the
#  parameters set so far) that returns False when the row is invalid, keeps
#  invalid combinations out.
#
#  covering.py --self-test

import itertools
import random
import sys


def covering_array(params, t=2, constraint=None, seed=0):
    """params: list of (name, [values]).  Returns a list of dicts (rows)."""
    rnd = random.Random(seed)
    names = [n for n, _ in params]
    domains = {n: list(v) for n, v in params}
    t = min(t, len(names))
    ok = constraint or (lambda row: True)

    def valid(row):
        return ok({k: v for k, v in row.items() if v is not None})

    # The first t parameters: all their valid combinations.
    first = names[:t]
    rows = []
    for combo in itertools.product(*(domains[n] for n in first)):
        row = dict(zip(first, combo))
        if valid(row):
            rows.append(row)

    for i in range(t, len(names)):
        p = names[i]
        earlier = names[:i]
        # The uncovered combinations: p's value with the values of t-1 earlier parameters.
        uncovered = set()
        for others in itertools.combinations(earlier, t - 1):
            for combo in itertools.product(*(domains[n] for n in others)):
                for v in domains[p]:
                    key = tuple(sorted(zip(others + (p,), combo + (v,))))
                    if valid(dict(key)):
                        uncovered.add(key)

        def covered_by(row, v):
            r = dict(row)
            r[p] = v
            n = 0
            for others in itertools.combinations(earlier, t - 1):
                if any(r.get(o) is None for o in others):
                    continue
                key = tuple(sorted([(o, r[o]) for o in others] + [(p, v)]))
                if key in uncovered:
                    n += 1
            return n

        # Horizontal growth.
        for row in rows:
            best, best_n = None, -1
            values = domains[p][:]
            rnd.shuffle(values)
            for v in values:
                r = dict(row)
                r[p] = v
                if not valid(r):
                    continue
                n = covered_by(row, v)
                if n > best_n:
                    best, best_n = v, n
            row[p] = best
            if best is not None:
                for others in itertools.combinations(earlier, t - 1):
                    if any(row.get(o) is None for o in others):
                        continue
                    uncovered.discard(tuple(sorted([(o, row[o]) for o in others] + [(p, best)])))

        # Vertical growth.
        for key in sorted(uncovered, key=repr):
            want = dict(key)
            placed = False
            for row in rows:
                if all(row.get(k) is None or row.get(k) == v for k, v in want.items()):
                    r = dict(row)
                    r.update(want)
                    if valid(r):
                        row.update(want)
                        placed = True
                        break
            if not placed:
                row = {n: None for n in names[:i + 1]}
                row.update(want)
                rows.append(row)

    # The entries left unset: any valid value.
    for row in rows:
        for n in names:
            if row.get(n) is None:
                values = domains[n][:]
                rnd.shuffle(values)
                for v in values:
                    r = dict(row)
                    r[n] = v
                    if valid(r):
                        row[n] = v
                        break
                else:
                    row[n] = values[0]
    return [r for r in rows if ok(r)]


def coverage(rows, params, t=2, constraint=None):
    """The t-way combinations missing from the rows (valid ones only)."""
    ok = constraint or (lambda row: True)
    names = [n for n, _ in params]
    domains = dict(params)
    missing = []
    for ps in itertools.combinations(names, t):
        for combo in itertools.product(*(domains[n] for n in ps)):
            want = dict(zip(ps, combo))
            if not ok(want):
                continue
            if not any(all(r[k] == v for k, v in want.items()) for r in rows):
                missing.append(want)
    return missing


def self_test():
    failures = 0
    params = [("a", [0, 1, 2, 3]), ("b", [0, 1, 2]), ("c", [0, 1]), ("d", [0, 1, 2]), ("e", [0, 1]), ("f", list(range(5)))]
    no_a3_b2 = lambda r: not (r.get("a") == 3 and r.get("b") == 2)
    for t in (2, 3):
        for constraint in (None, no_a3_b2):
            rows = covering_array(params, t, constraint, seed=1)
            missing = coverage(rows, params, t, constraint)
            bad = [r for r in rows if constraint and not constraint(r)]
            full = 1
            for _, v in params:
                full *= len(v)
            print("t=%d constraint=%s: %d rows (exhaustive %d), %d missing, %d invalid"
                  % (t, constraint is not None, len(rows), full, len(missing), len(bad)))
            failures += bool(missing) + bool(bad)
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    print(__doc__ if __doc__ else "covering.py --self-test")
