#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  Combinatorial testing of tconv (see README.md): t-way covering arrays
#  (covering.py) over tconv's options and classes of transforms and inputs,
#  each case checked against a reference run of the same transform, input,
#  scope and filter (in Unicode mode, with the default switches): options
#  that only choose how a transform is implemented must not change its
#  output.  The reference runs are also checked against the CLDR test data's
#  expected results and against ICU.
#
#  tconv_ct.py run      [--branch B] [--t 2] [--jobs 1]   continuous, on the commits of a branch
#  tconv_ct.py once     --tconv BIN [--budget 120] [--transform NAME]... [--set PARAM=VALUE]...
#  tconv_ct.py report                                      the state of the continuous runner
#  tconv_ct.py check    ID --tconv BIN                     rerun an anomaly (exit 1 if it persists)
#  tconv_ct.py bisect   ID [--good COMMIT] [--bad COMMIT] [--jobs N]
#
#  Common options: --state DIR (default ~/.parabix/tconv-ct), --cldr DIR
#  (default ~/cldr/common/testData/transforms).

import argparse
import hashlib
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from covering import covering_array  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_STATE = os.path.expanduser("~/.parabix/tconv-ct")
DEFAULT_CLDR = os.path.expanduser("~/cldr/common/testData/transforms")
RUN_TIMEOUT = 600

# The parameters and their values.  "class" (the class of the transform, by
# its plan) is added from the catalog.
PARAMS = [
    ("mode", ["mixed", "utf8-only", "unicode"]),          # --utf8-mode, --mixed-modes
    ("scope", ["text", "lines", "lines-sep"]),             # --lines, --region-separators
    ("filter", ["none", "half"]),                          # a global filter around the transform
    ("input", ["cldr", "random"]),
    ("length", ["tiny", "block", "multi", "large"]),       # ~4, ~140, ~600, ~6000 characters
    ("segsize", ["default", "small"]),                     # -segment-size
    ("capture_strings", ["0", "1"]),
    ("after_streams", ["0", "1"]),
    ("shape_groups", ["0", "1"]),
]
LENGTHS = {"tiny": (1, 8), "block": (100, 180), "multi": (450, 700), "large": (5000, 7000)}
UNSUPPORTED = re.compile(r"not supported|not implementable|not yet supported|unknown transform|no transform", re.I)


# ---------------------------------------------------------------- state

class State:
    def __init__(self, path):
        self.path = path
        os.makedirs(os.path.join(path, "anomalies"), exist_ok=True)
        self.file = os.path.join(path, "state.json")
        self.data = {"commits": [], "rotation": {}, "known": {}, "passes": 0, "cases": 0}
        if os.path.exists(self.file):
            with open(self.file) as f:
                self.data.update(json.load(f))

    def save(self):
        tmp = self.file + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.data, f, indent=1, sort_keys=True)
        os.replace(tmp, self.file)

    def log(self, record):
        with open(os.path.join(self.path, "results.jsonl"), "a") as f:
            f.write(json.dumps(record, sort_keys=True) + "\n")

    def anomalies(self):
        d = os.path.join(self.path, "anomalies")
        out = []
        for a in sorted(os.listdir(d)):
            p = os.path.join(d, a, "anomaly.json")
            if os.path.exists(p):
                with open(p) as f:
                    out.append(json.load(f))
        return out

    def save_anomaly(self, a):
        d = os.path.join(self.path, "anomalies", a["id"])
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "anomaly.json"), "w") as f:
            json.dump(a, f, indent=1, sort_keys=True, ensure_ascii=False)


# ---------------------------------------------------------------- catalog

def read_cldr(cldr, name):
    rows = []
    with open(os.path.join(cldr, name + ".txt"), encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or "\t" not in line:
                continue
            parts = line.rstrip("\n").split("\t")
            rows.append((parts[0], parts[1]))
    return rows


def classify(plan):
    """The class of a transform by its plan (--plan --lines), or None if not implementable."""
    if re.search(r"^\S.*: not implementable", plan, re.M) or "unknown" in plan.split("\n")[0]:
        return None
    m = re.search(r"^\s*mode: (.*)$", plan, re.M)
    if m is None:
        mode = "icu" if "applied by ICU" in plan else "unicode"
    elif m.group(1).startswith("UTF-8"):
        mode = "utf8"
    else:
        reasons = sorted(set(re.findall(r"Unicode \(([^)]*)\)", m.group(1))))
        mode = "mixed[" + "; ".join(reasons) + "]"
    parts = [mode]
    rs = re.search(r"region scope \(--lines\): (.*)$", plan, re.M)
    if rs and "not supported" in rs.group(1):
        parts.append("nolines")
    elif rs and "bounded" in rs.group(1):
        parts.append("bounded")
    if re.search(r"^  filter ", plan, re.M):
        parts.append("filter")
    return "+".join(parts)


def build_catalog(tconv, cldr):
    catalog = {}
    for fn in sorted(os.listdir(cldr)):
        if not fn.endswith(".txt"):
            continue
        name = fn[:-4]
        r = subprocess.run([tconv, "--plan", "--lines", name], capture_output=True, timeout=120)
        plan = r.stdout.decode("utf-8", "replace") + r.stderr.decode("utf-8", "replace")
        cls = classify(plan) if r.returncode == 0 else None
        if cls:
            catalog[name] = cls
    return catalog


def build_catalog_for(tconv, names):
    catalog = {}
    for name in names:
        r = subprocess.run([tconv, "--plan", "--lines", name], capture_output=True, timeout=120)
        cls = classify(r.stdout.decode("utf-8", "replace")) if r.returncode == 0 else None
        if cls:
            catalog[name] = cls
    return catalog


def classes_of(catalog):
    out = {}
    for name, cls in sorted(catalog.items()):
        out.setdefault(cls, []).append(name)
    return out


def constraint(row):
    cls = row.get("class")
    if cls and "nolines" in cls and row.get("scope") in ("lines", "lines-sep"):
        return False
    return True


# ---------------------------------------------------------------- cases

def case_id(case):
    key = json.dumps({k: case[k] for k in sorted(case) if k not in ("id",)}, sort_keys=True)
    return hashlib.sha1(key.encode()).hexdigest()[:10]


def make_input(case, cldr):
    """The input text of a case, and the CLDR rows it comes from (or None), deterministic by its seed."""
    rnd = random.Random(case["seed"])
    rows = read_cldr(cldr, case["transform"])
    alphabet = sorted(set("".join(s for s, _ in rows)) - {"\n", "\t"}) or list("abc")
    reserved = alphabet[-1] if len(alphabet) > 2 else None     # kept out of random text, for a separator
    lo, hi = LENGTHS[case["length"]]
    target = rnd.randint(lo, hi)
    if case["input"] == "cldr" and rows:
        start = rnd.randrange(len(rows))
        chosen, total = [], 0
        for k in range(len(rows)):
            s, e = rows[(start + k) % len(rows)]
            chosen.append((s, e))
            total += len(s) + 1
            if total >= target:
                break
        text = "".join(s + "\n" for s, _ in chosen)
        return text, chosen, alphabet
    letters = [c for c in alphabet if c != reserved] + [" "]
    lines, total = [], 0
    while total < target:
        n = rnd.randint(1, min(24, max(1, target - total)))
        lines.append("".join(rnd.choice(letters) for _ in range(n)))
        total += n + 1
    text = "\n".join(lines) + ("\n" if rnd.random() < 0.5 else "")
    return text, None, alphabet


def separator(text, alphabet):
    present = set(text)
    for c in reversed(alphabet):
        if c not in present and c != " " and ord(c) > 0x20:
            return c
    return "\x01"


def filter_rules(case, text, alphabet):
    rnd = random.Random(case["seed"] * 7 + 1)
    chars = sorted(c for c in set(text) | set(alphabet) if c not in "\n\t" and ord(c) <= 0xFFFF)
    keep = [c for c in chars if rnd.random() < 0.5] or chars[:1]
    return ":: [" + "".join("\\u%04X" % ord(c) for c in keep) + "] ;\n:: " + case["transform"] + " ;\n"


def tconv_args(case, objcache, salt=None, reference=False, icu=False, compile_threads=1):
    #  (One thread for running, and by default for compiling the kernels: the
    #  continuous runner keeps to one core.)
    args = ["-thread-num=1", "-compile-threads=%d" % compile_threads]
    if objcache:
        args.append("-object-cache-dir=" + objcache)
    if salt:
        args.append("-object-cache-salt=" + salt)
    if icu:
        args.append("--use-ICU=.*")
    elif reference:
        args.append("--utf8-mode=0")
    else:
        args += {"mixed": ["--utf8-mode=1", "--mixed-modes=1"], "utf8-only": ["--utf8-mode=1", "--mixed-modes=0"],
                 "unicode": ["--utf8-mode=0"]}[case["mode"]]
        if case["segsize"] == "small":
            args.append("-segment-size=1")
        args += ["--capture-strings=" + case["capture_strings"], "--after-streams=" + case["after_streams"],
                 "--apply-shape-groups=" + case["shape_groups"]]
    if case["scope"] != "text":
        args.append("--lines")
    return args


def run_tconv(tconv, args, case, text, rules, tmp, sep=None):
    path = os.path.join(tmp, "input.txt")
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    cmd = [tconv] + args
    if sep is not None:
        cmd.append("--region-separators=%X" % ord(sep))
    if rules:
        rp = os.path.join(tmp, "rules.txt")
        with open(rp, "w", encoding="utf-8") as f:
            f.write(rules)
        cmd.append("--rules-file=" + rp)
    else:
        cmd.append(case["transform"])
    cmd.append(path)
    t0 = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return "timeout", "", "", time.time() - t0, cmd
    out = r.stdout.decode("utf-8", "replace")
    err = r.stderr.decode("utf-8", "replace")
    if r.returncode == 0:
        status = "ok"
    elif r.returncode > 0 and UNSUPPORTED.search(err):
        status = "unsupported"
    else:
        status = "error"
    return status, out, err[-2000:], time.time() - t0, cmd


class Checker:
    """Runs cases with one tconv binary, with the reference runs cached."""

    def __init__(self, tconv, cldr, objcache=None, salt=None, compile_threads=1):
        self.tconv, self.cldr, self.objcache, self.salt = tconv, cldr, objcache, salt
        self.compile_threads = compile_threads
        self.refs = {}
        self.tmp = tempfile.mkdtemp(prefix="tconv-ct-")

    def close(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def args(self, case, **kw):
        return tconv_args(case, self.objcache, self.salt, compile_threads=self.compile_threads, **kw)

    def prepared(self, case, text=None):
        gen, rows, alphabet = make_input(case, self.cldr)
        if text is None:
            text = gen
        else:
            rows = None
        sep = None
        if case["scope"] == "lines-sep":
            text = text.rstrip("\n")
            sep = separator(text, alphabet)
        rules = filter_rules(case, text, alphabet) if case["filter"] == "half" else None
        return text, rows, sep, rules

    def reference(self, case, text, rules):
        scope = "text" if case["scope"] == "text" else "lines"
        key = (case["transform"], scope, rules, text)
        if key not in self.refs:
            rc = dict(case, scope=scope)
            res = run_tconv(self.tconv, self.args(rc, reference=True), rc, text, rules, self.tmp)
            if res[0] == "unsupported":
                # (The transform only in UTF-8 mode: the default configuration.)
                res = run_tconv(self.tconv, [a for a in self.args(rc, reference=True)
                                             if a != "--utf8-mode=0"], rc, text, rules, self.tmp)
            self.refs[key] = res
        return self.refs[key]

    def check(self, case, text=None):
        """(status, detail): ok, unsupported, or an anomaly kind (error, timeout, disagree)."""
        text, rows, sep, rules = self.prepared(case, text)
        if sep is not None:
            pieces = text.split("\n")
            test_text = sep.join(pieces)
        else:
            test_text = text
        res = run_tconv(self.tconv, self.args(case), case, test_text, rules, self.tmp, sep)
        status, out, err, secs, cmd = res
        detail = {"secs": round(secs, 2), "cmd": cmd}
        if status in ("error", "timeout"):
            detail["stderr"] = err
            return status, detail
        ref = self.reference(case, text, rules)
        if ref[0] in ("error", "timeout"):
            detail["stderr"] = ref[2]
            detail["reference_cmd"] = ref[4]
            return "reference-" + ref[0], detail
        if status == "unsupported" or ref[0] == "unsupported":
            detail["stderr"] = err if status == "unsupported" else ref[2]
            return "unsupported", detail
        want = ref[1]
        if sep is not None:
            parts = want.split("\n")
            want = sep.join(parts) if len(parts) == len(pieces) else None
        if out != want:
            i = next((k for k in range(min(len(out), len(want or ""))) if out[k] != (want or "")[k]), 0)
            detail.update({"at": i, "got": out[max(0, i - 20):i + 20], "want": (want or "")[max(0, i - 20):i + 20],
                           "reference_cmd": ref[4]})
            return "disagree", detail
        return "ok", detail

    def oracles(self, case):
        """Mismatches of the reference run against the CLDR expected results and ICU: lists of source lines."""
        text, rows, sep, rules = self.prepared(case)
        found = {}
        if case["scope"] == "text" or rules:
            return found
        ref = self.reference(case, text, rules)
        if ref[0] != "ok":
            return found
        got = ref[1].split("\n")
        src = text.split("\n")
        if rows is not None:
            found["expected"] = [s for k, (s, e) in enumerate(rows) if k >= len(got) or got[k] != e]
        icu = run_tconv(self.tconv, self.args(dict(case, scope="lines"), icu=True),
                        case, text, None, self.tmp)
        if icu[0] == "ok":
            ig = icu[1].split("\n")
            found["icu"] = [src[k] for k in range(min(len(src), len(ig), len(got))) if ig[k] != got[k] and src[k]]
        return found


# ---------------------------------------------------------------- shrinking

def shrink(checker, case, kind, text, budget=180):
    """A smaller input with the same anomaly (ddmin over lines, then characters)."""
    deadline = time.time() + budget

    def still(t):
        if time.time() > deadline or not t:
            return False
        return checker.check(case, t)[0] == kind

    for unit in ("lines", "chars"):
        items = text.split("\n") if unit == "lines" else list(text)
        join = (lambda xs: "\n".join(xs)) if unit == "lines" else (lambda xs: "".join(xs))
        n = 2
        while len(items) >= 2 and time.time() < deadline:
            size = max(1, len(items) // n)
            reduced = False
            for start in range(0, len(items), size):
                trial = items[:start] + items[start + size:]
                if trial and still(join(trial)):
                    items, n, reduced = trial, max(n - 1, 2), True
                    break
            if not reduced:
                if size == 1:
                    break
                n = min(len(items), n * 2)
        text = join(items)
    return text


# ---------------------------------------------------------------- anomalies

def record_anomaly(state, checker, case, kind, detail, commit, text, do_shrink=True):
    a_id = hashlib.sha1((kind + case["id"]).encode()).hexdigest()[:10]
    existing = {a["id"]: a for a in state.anomalies()}
    if a_id in existing:
        return existing[a_id], False
    shrunk = None
    if do_shrink and kind in ("disagree", "error"):
        shrunk = shrink(checker, case, kind, text)
    a = {"id": a_id, "kind": kind, "case": case, "detail": detail, "first_seen": commit,
         "previous_commit": state.data["commits"][-2] if len(state.data["commits"]) > 1 else None,
         "status": "open", "time": time.strftime("%Y-%m-%d %H:%M:%S")}
    state.save_anomaly(a)
    d = os.path.join(state.path, "anomalies", a_id)
    with open(os.path.join(d, "input.txt"), "w", encoding="utf-8") as f:
        f.write(text)
    if shrunk is not None:
        with open(os.path.join(d, "shrunk.txt"), "w", encoding="utf-8") as f:
            f.write(shrunk)
    return a, True


def oracle_anomalies(state, case, found, commit, baseline):
    """New mismatches against the CLDR expected results (by line) and ICU (by transform)."""
    known = state.data["known"].setdefault(case["transform"], {"expected": [], "icu": False})
    new = []
    for line in found.get("expected", []):
        if line not in known["expected"]:
            known["expected"].append(line)
            if not baseline:
                new.append(("expected", line))
    if found.get("icu"):
        if not known["icu"] and not baseline:
            new.append(("icu", found["icu"][0]))
        known["icu"] = True
    return new


# ---------------------------------------------------------------- commands

def git(*args, cwd=REPO):
    return subprocess.run(["git"] + list(args), cwd=cwd, capture_output=True, text=True, check=True).stdout.strip()


def fingerprint(tconv):
    """A hash of tconv and the libraries it loads from its build (the object cache's keys follow them)."""
    h = hashlib.sha1()
    lib = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(tconv))), "lib")
    files = [tconv] + sorted(os.path.join(d, f) for d, _, fs in os.walk(lib) for f in fs
                             if f.endswith((".dylib", ".so")) or ".so." in f)
    for path in files:
        if os.path.isfile(path):
            with open(path, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
    return h.hexdigest()


def cmake_settings(reference_build):
    settings = {"CMAKE_BUILD_TYPE": "Release"}
    cache = os.path.join(reference_build, "CMakeCache.txt")
    if os.path.exists(cache):
        for line in open(cache):
            m = re.match(r"(CMAKE_PREFIX_PATH|LLVM_DIR|ENABLE_ICU|CMAKE_BUILD_TYPE)(?::[A-Z]+)?=(.*)", line.strip())
            if m:
                settings[m.group(1)] = m.group(2)
    settings["PARABIX_BUILD_TESTS"] = "OFF"
    return settings


def build(worktree, build_dir, commit, jobs, reference_build, log):
    if not os.path.exists(worktree):
        git("worktree", "add", "--detach", worktree, commit)
    else:
        git("checkout", "--detach", "--quiet", commit, cwd=worktree)
    if not os.path.exists(os.path.join(build_dir, "CMakeCache.txt")):
        os.makedirs(build_dir, exist_ok=True)
        cmd = ["cmake", "-S", worktree, "-B", build_dir] + ["-D%s=%s" % kv for kv in cmake_settings(reference_build).items()]
        subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=True)
    r = subprocess.run(["nice", "-n", "10", "make", "-j%d" % jobs, "tconv"], cwd=build_dir, stdout=log, stderr=subprocess.STDOUT)
    return r.returncode == 0


def cases_for(state, catalog, t, seed, selected=None, settings=None):
    classes = classes_of(catalog)
    params = [("class", sorted(classes))] + PARAMS
    if settings:
        params = [(n, [settings[n]] if n in settings else v) for n, v in params]
    rows = covering_array(params, t, constraint, seed)
    random.Random(seed).shuffle(rows)
    rot = state.data["rotation"]
    cases = []
    for k, row in enumerate(rows):
        members = classes[row["class"]]
        if selected:
            members = [m for m in members if m in selected] or None
            if members is None:
                continue
        i = rot.get(row["class"], 0)
        rot[row["class"]] = i + 1
        case = dict(row, transform=members[i % len(members)], seed=seed * 100003 + k)
        case["id"] = case_id(case)
        cases.append(case)
    return cases


def cmd_run(a):
    state = State(a.state)
    worktree = a.worktree or os.path.join(os.path.dirname(REPO), os.path.basename(REPO) + "-ct")
    build_dir = os.path.join(worktree, "build-ct")
    objcache = os.path.join(a.state, "objcache")
    branch = a.branch or git("branch", "--show-current")
    log = open(os.path.join(a.state, "build.log"), "a")
    tconv = a.tconv or os.path.join(build_dir, "bin", "tconv")
    print("tconv_ct: following %s; worktree %s; state %s" % (branch, worktree, a.state), flush=True)
    checker = None
    while True:
        commit = git("rev-parse", "--short", branch)
        commits = state.data["commits"]
        if not commits or commits[-1] != commit:
            print("[%s] building %s" % (time.strftime("%H:%M:%S"), commit), flush=True)
            if not a.tconv and not build(worktree, build_dir, commit, a.jobs, a.reference_build, log):
                state.log({"commit": commit, "status": "build-failed"})
                time.sleep(300)
                continue
            commits.append(commit)
            #  (A commit that leaves tconv and its libraries unchanged, e.g. of
            #  documentation or tests, keeps the object cache, the catalog and
            #  the anomalies' status: the objects are keyed by the binaries.)
            fp = fingerprint(tconv)
            changed = a.tconv or fp != state.data.get("fingerprint") or "catalog" not in state.data
            state.data["fingerprint"] = fp
            state.data.setdefault("baseline_fingerprint", fp)
            if not changed:
                print("  tconv unchanged", flush=True)
                state.save()
                continue
            shutil.rmtree(objcache, ignore_errors=True)
            state.data["catalog"] = build_catalog(tconv, a.cldr) if not a.only else \
                {n: c for n, c in build_catalog_for(tconv, a.only).items()}
            state.save()
            checker = Checker(tconv, a.cldr, objcache)
            # The open anomalies first: fixed, or still there.
            for an in state.anomalies():
                if an["status"] != "open" or an["kind"] in ("expected", "icu"):
                    continue
                st, _ = checker.check(an["case"])
                if st != an["kind"]:
                    an["status"], an["fixed_at"] = "fixed", commit
                    state.save_anomaly(an)
                    print("  fixed at %s: %s %s" % (commit, an["id"], an["kind"]), flush=True)
        if checker is None:
            checker = Checker(tconv, a.cldr, objcache)
        #  (The CLDR and ICU mismatches of the first build tested are the baseline.)
        baseline = state.data.get("fingerprint") == state.data.get("baseline_fingerprint")
        state.data["passes"] += 1
        seed = state.data["passes"]
        t = a.t if state.data["passes"] <= a.pairwise_passes else a.t + 1
        cases = cases_for(state, state.data["catalog"], t, seed)
        state.save()
        print("[%s] %s pass %d: %d cases (t=%d)" % (time.strftime("%H:%M:%S"), commit, seed, len(cases), t), flush=True)
        for case in cases:
            if git("rev-parse", "--short", branch) != commit:
                break
            st, detail = checker.check(case)
            state.data["cases"] += 1
            state.log({"commit": commit, "case": case, "status": st, "secs": detail.get("secs")})
            if st not in ("ok", "unsupported"):
                text = checker.prepared(case)[0]
                an, new = record_anomaly(state, checker, case, st, detail, commit, text)
                if new:
                    print("  ANOMALY %s %s %s %s" % (an["id"], st, case["transform"],
                                                     {k: case[k] for k in dict(PARAMS)}), flush=True)
            for kind, line in oracle_anomalies(state, case, checker.oracles(case), commit, baseline):
                oc = dict(case, oracle_line=line)
                oc["id"] = case_id(oc)
                an, new = record_anomaly(state, checker, oc, kind, {"line": line}, commit, line, do_shrink=False)
                if new:
                    print("  ANOMALY %s %s %s: %r" % (an["id"], kind, case["transform"], line), flush=True)
            state.save()
        checker.refs.clear()


def cmd_once(a):
    state = State(a.state or tempfile.mkdtemp(prefix="tconv-ct-once-"))
    catalog = build_catalog_for(a.tconv, a.transform) if a.transform else build_catalog(a.tconv, a.cldr)
    settings = dict(s.split("=", 1) for s in a.set)
    checker = Checker(a.tconv, a.cldr, salt=a.salt, compile_threads=a.compile_threads)
    start = time.time()
    deadline = start + a.budget
    cases = cases_for(state, catalog, a.t, a.seed, set(a.transform) if a.transform else None, settings)
    done = bad = 0
    for case in cases:
        #  (A case is started only if one of the average time so far fits.)
        if time.time() + ((time.time() - start) / done if done else 0) > deadline:
            break
        st, detail = checker.check(case)
        done += 1
        if st not in ("ok", "unsupported"):
            bad += 1
            print("%s %s %s %s\n  %s" % (st, case["transform"], {k: case[k] for k in dict(PARAMS)},
                                         " ".join(detail.get("cmd", [])), {k: v for k, v in detail.items() if k != "cmd"}))
    print("%d of %d cases run in %.0f s, %d anomalies" % (done, len(cases), time.time() - start, bad))
    checker.close()
    return 1 if bad else 0


def cmd_report(a):
    state = State(a.state)
    d = state.data
    print("commits tested: %s" % ", ".join(d["commits"][-10:]))
    print("passes %d, cases %d" % (d["passes"], d["cases"]))
    for an in state.anomalies():
        c = an["case"]
        print("%s %-8s %-9s %s first %s (previous tested %s)%s" % (
            an["id"], an["status"], an["kind"], c["transform"], an["first_seen"], an["previous_commit"],
            " fixed at " + an["fixed_at"] if an.get("fixed_at") else ""))
        if a.verbose:
            print("   ", {k: c[k] for k in dict(PARAMS) if k in c}, json.dumps(an["detail"], ensure_ascii=False)[:400])


def find_anomaly(state, a_id):
    for an in state.anomalies():
        if an["id"] == a_id:
            return an
    sys.exit("no anomaly " + a_id)


def cmd_check(a):
    state = State(a.state)
    an = find_anomaly(state, a_id=a.id)
    d = os.path.join(state.path, "anomalies", an["id"])
    text = None
    for fn in ("shrunk.txt", "input.txt"):
        if os.path.exists(os.path.join(d, fn)) and an["kind"] not in ("expected", "icu"):
            text = open(os.path.join(d, fn), encoding="utf-8").read()
            break
    checker = Checker(a.tconv, a.cldr, salt=a.salt)
    if an["kind"] in ("expected", "icu"):
        found = checker.oracles(an["case"])
        persists = an["case"].get("oracle_line") in found.get(an["kind"], [])
        st = an["kind"] if persists else "ok"
    else:
        st, detail = checker.check(an["case"], text)
        print(json.dumps(detail, ensure_ascii=False)[:1000])
    checker.close()
    print(st)
    if st == "unsupported":
        return 125
    return 1 if st == an["kind"] else 0


def cmd_bisect(a):
    state = State(a.state)
    an = find_anomaly(state, a.id)
    good = a.good or an["previous_commit"]
    bad = a.bad or an["first_seen"]
    if not good:
        sys.exit("no known good commit: give --good")
    wt = os.path.join(os.path.dirname(REPO), os.path.basename(REPO) + "-ct-bisect")
    if not os.path.exists(wt):
        git("worktree", "add", "--detach", wt, bad)
    build_dir = os.path.join(wt, "build-ct")
    script = os.path.join(a.state, "bisect-step.sh")
    with open(script, "w") as f:
        f.write("#!/bin/sh\n")
        f.write("if [ ! -f '%s/CMakeCache.txt' ]; then cmake -S '%s' -B '%s' %s >/dev/null || exit 125; fi\n" % (
            build_dir, wt, build_dir, " ".join("-D%s=%s" % kv for kv in cmake_settings(a.reference_build).items())))
        f.write("make -C '%s' -j%d tconv >/dev/null 2>&1 || exit 125\n" % (build_dir, a.jobs))
        f.write("exec python3 '%s' check %s --state '%s' --cldr '%s' --tconv '%s/bin/tconv' --salt bisect\n" % (
            os.path.abspath(__file__), an["id"], a.state, a.cldr, build_dir))
    os.chmod(script, 0o755)
    git("bisect", "start", bad, good, cwd=wt)
    r = subprocess.run(["git", "bisect", "run", script], cwd=wt)
    git("bisect", "reset", cwd=wt)
    return r.returncode


def main():
    p = argparse.ArgumentParser(description="Combinatorial testing of tconv")
    sub = p.add_subparsers(dest="command", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--state", help="(default %s; for once, a temporary directory)" % DEFAULT_STATE)
    common.add_argument("--cldr", default=DEFAULT_CLDR)
    common.add_argument("--reference-build", default=os.path.join(REPO, "build22"),
                        help="build whose CMake settings (LLVM, ICU) the runner's builds copy")
    r = sub.add_parser("run", parents=[common])
    r.add_argument("--branch")
    r.add_argument("--worktree")
    r.add_argument("--t", type=int, default=2)
    r.add_argument("--pairwise-passes", type=int, default=3, help="passes at strength t before t+1")
    r.add_argument("--jobs", type=int, default=1)
    r.add_argument("--tconv", help="(testing the runner) a tconv binary to use instead of building the commits")
    r.add_argument("--only", action="append", default=[], help="(testing the runner) only these transforms")
    o = sub.add_parser("once", parents=[common])
    o.add_argument("--tconv", required=True)
    o.add_argument("--budget", type=float, default=120)
    o.add_argument("--transform", action="append", default=[])
    o.add_argument("--set", action="append", default=[], help="PARAM=VALUE: fix a parameter")
    o.add_argument("--t", type=int, default=2)
    o.add_argument("--seed", type=int, default=1)
    o.add_argument("--salt", default="ct")
    o.add_argument("--compile-threads", type=int, default=4)
    rp = sub.add_parser("report", parents=[common])
    rp.add_argument("-v", "--verbose", action="store_true")
    c = sub.add_parser("check", parents=[common])
    c.add_argument("id")
    c.add_argument("--tconv", required=True)
    c.add_argument("--salt", default="ct")
    b = sub.add_parser("bisect", parents=[common])
    b.add_argument("id")
    b.add_argument("--good")
    b.add_argument("--bad")
    b.add_argument("--jobs", type=int, default=1)
    a = p.parse_args()
    #  (The parsers share the --state action, so its default is set here.)
    if a.state:
        a.state = os.path.abspath(a.state)
    elif a.command != "once":
        a.state = DEFAULT_STATE
    return {"run": cmd_run, "once": cmd_once, "report": cmd_report, "check": cmd_check, "bisect": cmd_bisect}[a.command](a)


if __name__ == "__main__":
    sys.exit(main() or 0)
