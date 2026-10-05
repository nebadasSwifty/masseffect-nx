#!/usr/bin/env python3
"""Build and validate the English -> Russian function address map for Mass Effect 1.

Usage:
  python3 tools/rus_address_map.py [--build] [--validate] [--out rus_address_map.json]
"""
import glob
import json
import os
import re
import sys
import time
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENG_GEN = os.path.abspath(os.path.join(ROOT, "..", "masseffect-nx", "app", "generated", "default"))
RUS_GEN = os.path.join(ROOT, "app", "generated", "default")
MAP_PATH = os.path.join(ROOT, "rus_address_map.json")

ASM_RE = re.compile(r"^\t//\s+([a-z][a-z0-9._]*)(?:\s+(.*))?$")
REG_RE = re.compile(r"\b([rfv]\d+|cr\d+|xer|ctr|lr)\b")
FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\((sub_[0-9A-Fa-f]{8})\)")
CALL_RE = re.compile(r"\b(?:__imp__)?(sub_[0-9A-F]{8}|[A-Z][A-Za-z0-9_]+)\(ctx, base\)")

# Fixed anchors where generated C++ was modified by patches or split differently
KNOWN_ANCHORS = {
    "SUB_82238FE8": "SUB_82238DD8",  # Scaleform GFx viewport
    "SUB_827C07F0": "SUB_827C10E8",  # HUD world to screen
    "SUB_82ACA550": "SUB_82BE2330",  # CRT longjmp
    "SUB_8260D894": "SUB_8260D924",  # r11 tail fragment
}


def scan_functions(gen_dir):
    funcs = {}
    files = sorted(glob.glob(os.path.join(gen_dir, "masseffect_recomp.*.cpp")))
    t0 = time.time()
    for f in files:
        cur_fn = None
        cur_sig = []
        cur_calls = []
        with open(f, errors="replace") as fp:
            for line in fp:
                if line.startswith("DEFINE_REX_FUNC("):
                    m = FUNC_RE.match(line)
                    if m:
                        if cur_fn:
                            funcs[cur_fn] = {"sig": tuple(cur_sig), "calls": cur_calls}
                        cur_fn = m.group(1).upper()
                        cur_sig = []
                        cur_calls = []
                    continue
                if line.startswith("}") and cur_fn:
                    funcs[cur_fn] = {"sig": tuple(cur_sig), "calls": cur_calls}
                    cur_fn = None
                    continue
                if cur_fn:
                    if line.startswith("\t// "):
                        m = ASM_RE.match(line)
                        if m:
                            mnem = m.group(1)
                            # strip parenthetical patch comments if any
                            rest = re.sub(r"\(.*?\)", "", m.group(2) or "")
                            regs = tuple(REG_RE.findall(rest))
                            cur_sig.append((mnem, regs))
                    for c in CALL_RE.findall(line):
                        c = c.upper()
                        if c != cur_fn and c not in cur_calls:
                            cur_calls.append(c)
        if cur_fn:
            funcs[cur_fn] = {"sig": tuple(cur_sig), "calls": cur_calls}
    print(f"Loaded {len(funcs)} functions from {gen_dir} in {time.time()-t0:.2f}s")
    return funcs


def get_callers(funcs):
    callers = defaultdict(set)
    for fn, data in funcs.items():
        for c in data["calls"]:
            callers[c].add(fn)
    return callers


def build_map():
    eng = scan_functions(ENG_GEN)
    rus = scan_functions(RUS_GEN)

    eng_to_rus = {}
    rus_to_eng = {}

    # Seed known anchors
    for e, r in KNOWN_ANCHORS.items():
        if e in eng and r in rus:
            eng_to_rus[e] = r
            rus_to_eng[r] = e

    eng_by_sig = defaultdict(list)
    for fn, data in eng.items():
        eng_by_sig[data["sig"]].append(fn)

    rus_by_sig = defaultdict(list)
    for fn, data in rus.items():
        rus_by_sig[data["sig"]].append(fn)

    # Pass 0: unique signature
    for sig, eng_fns in eng_by_sig.items():
        rus_fns = rus_by_sig.get(sig, [])
        if len(eng_fns) == 1 and len(rus_fns) == 1:
            e = eng_fns[0]
            r = rus_fns[0]
            if e not in eng_to_rus and r not in rus_to_eng:
                eng_to_rus[e] = r
                rus_to_eng[r] = e

    print(f"Pass 0 (unique signatures + anchors): {len(eng_to_rus)} matched")

    eng_callers = get_callers(eng)
    rus_callers = get_callers(rus)

    eng_sorted = sorted(eng.keys(), key=lambda x: int(x[4:], 16))
    rus_sorted = sorted(rus.keys(), key=lambda x: int(x[4:], 16))
    rus_idx = {fn: i for i, fn in enumerate(rus_sorted)}

    for round_num in range(1, 20):
        prev = len(eng_to_rus)

        # Call-graph tie breaking
        for sig, eng_fns in eng_by_sig.items():
            unmatched_e = [e for e in eng_fns if e not in eng_to_rus]
            if not unmatched_e:
                continue
            rus_fns = [r for r in rus_by_sig.get(sig, []) if r not in rus_to_eng]
            if not rus_fns:
                continue

            if len(unmatched_e) == 1 and len(rus_fns) == 1:
                e, r = unmatched_e[0], rus_fns[0]
                eng_to_rus[e] = r
                rus_to_eng[r] = e
                continue

            for e in list(unmatched_e):
                e_calls = {eng_to_rus[c] for c in eng[e]["calls"] if c in eng_to_rus}
                e_callers = {eng_to_rus[c] for c in eng_callers[e] if c in eng_to_rus}

                best_r = None
                best_score = 0
                tie = False
                for r in rus_fns:
                    r_calls = set(rus[r]["calls"])
                    r_callers = set(rus_callers[r])
                    score = len(e_calls & r_calls) * 2 + len(e_callers & r_callers) * 3
                    if score > best_score:
                        best_score = score
                        best_r = r
                        tie = False
                    elif score == best_score and score > 0:
                        tie = True

                if best_r and not tie and best_score >= 2:
                    r = best_r
                    r_calls_rev = {rus_to_eng[c] for c in rus[r]["calls"] if c in rus_to_eng}
                    r_callers_rev = {rus_to_eng[c] for c in rus_callers[r] if c in rus_to_eng}
                    rev_best = None
                    rev_score = 0
                    rev_tie = False
                    for e_cand in unmatched_e:
                        s = len(set(eng[e_cand]["calls"]) & r_calls_rev) * 2 + len(set(eng_callers[e_cand]) & r_callers_rev) * 3
                        if s > rev_score:
                            rev_score = s
                            rev_best = e_cand
                            rev_tie = False
                        elif s == rev_score and s > 0:
                            rev_tie = True
                    if rev_best == e and not rev_tie:
                        eng_to_rus[e] = r
                        rus_to_eng[r] = e
                        unmatched_e.remove(e)
                        rus_fns.remove(r)

        # Neighbour propagation
        for i, e in enumerate(eng_sorted):
            if e in eng_to_rus:
                continue
            p = i - 1
            while p >= 0 and eng_sorted[p] not in eng_to_rus:
                p -= 1
            s = i + 1
            while s < len(eng_sorted) and eng_sorted[s] not in eng_to_rus:
                s += 1
            if p >= 0 and s < len(eng_sorted):
                e_p, e_s = eng_sorted[p], eng_sorted[s]
                r_p, r_s = eng_to_rus[e_p], eng_to_rus[e_s]
                idx_rp = rus_idx[r_p]
                idx_rs = rus_idx[r_s]
                if (s - p) == (idx_rs - idx_rp) and (idx_rs - idx_rp) > 0:
                    offset = i - p
                    r_cand = rus_sorted[idx_rp + offset]
                    if r_cand not in rus_to_eng:
                        if eng[e]["sig"] == rus[r_cand]["sig"]:
                            eng_to_rus[e] = r_cand
                            rus_to_eng[r_cand] = e

        delta = len(eng_to_rus) - prev
        print(f"Round {round_num}: {len(eng_to_rus)} matched (+{delta})")
        if delta == 0:
            break

    # Format result: lower case keys and values sub_82xxxxxx -> sub_82xxxxxx
    result = {k.lower(): v.lower() for k, v in sorted(eng_to_rus.items())}
    with open(MAP_PATH, "w") as fp:
        json.dump(result, fp, indent=2)
    print(f"Saved {len(result)} mapped functions to {MAP_PATH}")
    return result


def validate_map(address_map=None):
    if address_map is None:
        if not os.path.exists(MAP_PATH):
            sys.exit(f"Map not found at {MAP_PATH}; run with --build first.")
        address_map = json.load(open(MAP_PATH))

    # Convert keys to uppercase for matching
    addr_map_upper = {k.upper(): v.upper() for k, v in address_map.items()}

    # Check 154 hooked functions
    import direct_calls
    hooked = sorted(direct_calls.hooked())
    print(f"\nValidating {len(hooked)} hooked functions from direct_calls.py...")
    missing_hooked = []
    for h in hooked:
        h_up = h.upper()
        if h_up not in addr_map_upper:
            missing_hooked.append(h)
    if missing_hooked:
        print(f"WARNING: {len(missing_hooked)} hooked functions unmapped: {missing_hooked[:10]}")
    else:
        print(f"SUCCESS: All {len(hooked)} hooked functions mapped!")

    return len(missing_hooked) == 0


if __name__ == "__main__":
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    if "--build" in sys.argv or not os.path.exists(MAP_PATH):
        m = build_map()
        validate_map(m)
    elif "--validate" in sys.argv:
        validate_map()
    else:
        m = build_map()
        validate_map(m)
