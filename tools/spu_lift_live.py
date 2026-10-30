#!/usr/bin/env python3
"""Register liveness over a lifted SPU image (spu_lifter.py --regions output).

A native block (--native-block ADDR-END=SYMBOL) may leave a register as it was
before the block, instead of as the lifted code would, only when nothing reads
that register afterwards before writing it again. This answers that question
for one point of the lift, over the whole image: which registers may be read
before they are written from there on.

    python spu_lift_live.py <spu_recomp.c> 0x8F60            # at a label
    python spu_lift_live.py <spu_recomp.c> exit:0x9058 2-79  # after a loop's back edge
    python spu_lift_live.py <spu_recomp.c> @2794 3           # at a line of the file

The second argument is where: 0xADDR is the statement after that label, @LINE
the first statement at or after that line, exit:0xSTART the statement after
the back edge to the label START (a loop's exit). The third, optional, is a
set of registers (2-79, or 14,16,21): each of them that is live is printed
with the path to its first read. With exit:, the registers the loop body
writes that are live at its exit are printed too.

The flow it follows: falls through and gotos inside a region, region gotos
(SPU_RGOTO), trampolines (ctx->pc = ...; return), the bi-switch tables
(--bi-switch: the switch's cases are the indirect branch's targets), direct
calls and returns. A direct call reads what its callee's entry has live and
leaves r0 and r2-r79 undefined, as the SPU ABI lets a callee do; a return
has RET_LIVE live (default r1, r3 and r80-r127, the ABI's; set the RET_LIVE
environment variable to change it, e.g. RET_LIVE=1,80-127). An indirect jump
or call with no table makes every register live. A native block's hook line
(if (spu_hook_X(ctx)) goto loc_END;) counts as the lifted range it replaces,
which its contract makes it equal to, so the lifted code after it is what is
followed.

The ABI rule at returns is the one assumption: where a result depends on it
(a register live only because a return has it live), check the callers'
continuations with @LINE on the line after each call.
"""
import os
import re
import sys
from collections import deque

R_REG = re.compile(r"ctx->gpr\[(\d+)\]")
R_FULLW = re.compile(r"^ctx->gpr\[(\d+)\] = (.*);$")
R_LABEL = re.compile(r"^loc_([0-9A-F]{8}):$")
R_RGOTO = re.compile(r"SPU_RGOTO\(ctx, 0x([0-9A-F]+)u,")
R_TRAMP = re.compile(r"ctx->pc = 0x([0-9A-F]+); g_spu_trampoline_fn = \w+; return;")
R_CALL = re.compile(r"spu_link\(0x[0-9A-F]+\); \{ int32_t _si = \(int32_t\)ctx->image_id; "
                    r"ctx->host_depth\+\+; \w+_spu_func_([0-9A-F]{8})\(ctx\);")
R_ICALL = re.compile(r"spu_link\(0x[0-9A-F]+\);.*spu_indirect_branch\(ctx\);")
R_IJUMP = re.compile(r"ctx->pc = ctx->gpr\[(\d+)\]\._u32\[0\]; g_spu_trampoline_fn = spu_indirect_branch; return;")
R_HOOK = re.compile(r"^if \(spu_hook_\w+\(ctx\)\) goto loc_[0-9A-F]{8};$")
R_CASE = re.compile(r"^case 0x([0-9A-F]+)u: SPU_RGOTO\(ctx, 0x([0-9A-F]+)u,")

ALL = frozenset(range(128))
CALL_KILL = frozenset({0} | set(range(2, 80)))


def regset(s):
    out = set()
    for part in s.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-")
            out.update(range(int(a), int(b) + 1))
        else:
            out.add(int(part))
    return out


def parse(lines):
    """The region functions' statements as [line, kind, reads, write, targets].

    kind: op (falls through), jump (to its targets only), cjump (its targets
    and falls through), call (its callee, then falls through), ret, cret (a
    conditional return), all (unknown: every register live), halt (no
    successor), switch (its case targets, and falls through to the indirect
    jump after it)."""
    stmts, label_at, region_of = [], {}, []
    in_region = first_label = False
    pending_switch = None
    region_start = 0
    for ln, raw in enumerate(lines, 1):
        s = raw.strip()
        if raw.startswith("static void ") and "_spu_region_" in raw:
            in_region, first_label, region_start = True, False, ln
            continue
        if not in_region:
            continue
        if raw == "}":
            in_region = False
            continue
        m = R_LABEL.match(s)
        if m:
            first_label = True
            label_at[int(m.group(1), 16)] = len(stmts)
            continue
        if not first_label or not s or s.startswith("/*") or s in ("}", "{") or R_HOOK.match(s):
            continue                  # the entry switch, comments, hook lines
        mc = R_CASE.match(s)
        if mc:
            if pending_switch is not None:
                stmts[pending_switch][4].append(int(mc.group(2), 16))
            continue
        if s.startswith("default:"):
            continue
        reads = set(int(x) for x in R_REG.findall(s))
        if "switch (ctx->gpr[" in s:
            stmts.append([ln, "switch", reads, None, []])
            region_of.append(region_start)
            pending_switch = len(stmts) - 1
            continue
        write, kind, targets = None, "op", []
        w = R_FULLW.match(s)
        if R_CALL.search(s):
            kind, targets, reads, write = "call", [int(R_CALL.search(s).group(1), 16)], set(), 0
        elif R_ICALL.search(s):
            kind = "all"
        elif R_IJUMP.search(s):
            if pending_switch is not None and int(R_IJUMP.search(s).group(1)) in stmts[pending_switch][2]:
                kind = "halt"         # the switch's cases are every target
            else:
                kind = "all"
        elif "spu_halt(ctx)" in s and s.startswith("if ("):
            kind = "cjump"            # a halt: no successor on its taken side
        elif "SPU_RET(ctx)" in s:
            kind = "cret" if s.startswith("if (") else "ret"
            reads |= {0}
        elif R_RGOTO.search(s) or R_TRAMP.search(s):
            t = R_RGOTO.search(s) or R_TRAMP.search(s)
            targets = [int(t.group(1), 16)]
            kind = "cjump" if s.startswith("if (") else "jump"
        elif s.startswith("goto loc_"):
            kind, targets = "jump", [int(s[9:17], 16)]
        elif w:
            write = int(w.group(1))
            reads = set(int(x) for x in R_REG.findall(w.group(2)))
        elif "return;" in s:
            kind = "all"
        pending_switch = None
        stmts.append([ln, kind, reads, write, targets])
        region_of.append(region_start)
    return stmts, label_at, region_of


class Liveness:
    def __init__(self, lines, ret_live):
        self.stmts, self.label_at, self.region_of = parse(lines)
        self.ret_live = ret_live
        n = len(self.stmts)
        self.live = [frozenset()] * n
        self.rounds = 0
        changed = True
        while changed:
            changed = False
            self.rounds += 1
            for i in range(n - 1, -1, -1):
                new = self.transfer(i)
                if new != self.live[i]:
                    self.live[i] = new
                    changed = True

    def entry_live(self, addr):
        i = self.label_at.get(addr)
        return ALL if i is None else self.live[i]

    def succs(self, i):
        _, kind, _, _, targets = self.stmts[i]
        out = []
        if kind in ("op", "cjump", "call", "cret", "switch"):
            if i + 1 < len(self.stmts) and self.region_of[i + 1] == self.region_of[i]:
                out.append(i + 1)
        if kind in ("jump", "cjump", "switch"):
            out.extend(self.label_at.get(t, "ALL") for t in targets)
        return out

    def transfer(self, i):
        _, kind, reads, write, targets = self.stmts[i]
        if kind == "all":
            return ALL
        if kind == "halt":
            return frozenset(reads)
        if kind == "ret":
            return self.ret_live | reads
        out = set()
        for j in self.succs(i):
            out |= ALL if j == "ALL" else self.live[j]
        if kind == "cret":
            out |= self.ret_live
        if kind == "call":            # the link writes r0 before the callee runs
            return frozenset(((out - CALL_KILL) | self.entry_live(targets[0])) - {0})
        if write is not None:
            out.discard(write)
        return frozenset(out | reads)

    def where(self, w):
        if w.startswith("@"):
            want = int(w[1:])
            for i, st in enumerate(self.stmts):
                if st[0] >= want:
                    return i
            sys.exit("no statement at or after line %d" % want)
        if w.startswith("exit:"):
            a = int(w[5:], 16)
            for i in range(self.label_at[a], len(self.stmts)):
                if self.stmts[i][1] == "cjump" and a in self.stmts[i][4]:
                    return i + 1
            sys.exit("no back edge to 0x%X" % a)
        return self.label_at[int(w, 16)]

    def path_to_read(self, i0, r):
        """Line numbers from i0 to a read of r with no write of r before it."""
        prev = {i0: None}
        q = deque([i0])

        def back(i):
            p = []
            while i is not None:
                p.append(self.stmts[i][0])
                i = prev[i]
            return list(reversed(p))
        while q:
            i = q.popleft()
            _, kind, reads, write, targets = self.stmts[i]
            if r in reads or kind == "all" or (kind in ("ret", "cret") and r in self.ret_live) or \
               (kind == "call" and r in self.entry_live(targets[0])):
                return back(i), "read" if kind == "op" else kind
            if write == r or (kind == "call" and r in CALL_KILL):
                continue
            for j in self.succs(i):
                if j == "ALL":
                    return back(i), "a jump to an address with no label"
                if j not in prev:
                    prev[j] = i
                    q.append(j)
        return None, None


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    lines = open(sys.argv[1], encoding="utf-8").read().split("\n")
    lv = Liveness(lines, frozenset(regset(os.environ.get("RET_LIVE", "1,3,80-127"))))
    i0 = lv.where(sys.argv[2])
    at = lv.live[i0]
    print("%d statements, %d labels, %d rounds; at line %d (%s): live %s" % (
        len(lv.stmts), len(lv.label_at), lv.rounds, lv.stmts[i0][0], sys.argv[2],
        ",".join("r%d" % r for r in sorted(at))))
    want = regset(sys.argv[3]) if len(sys.argv) > 3 else set(ALL)
    for r in sorted(want & at):
        p, why = lv.path_to_read(i0, r)
        if p is None:
            print("  r%d: live (no path found)" % r)
        else:
            print("  r%d: %s at line %d after %d statements: %s" % (
                r, why, p[-1], len(p), lines[p[-1] - 1].strip()[:110]))
    if sys.argv[2].startswith("exit:"):
        start = lv.label_at[int(sys.argv[2][5:], 16)]
        body = set(lv.stmts[i][3] for i in range(start, i0) if lv.stmts[i][3] is not None)
        print("  the loop body writes %s" % ",".join("r%d" % r for r in sorted(body)))
        print("  written by it and live at the exit: %s" % (
            ",".join("r%d" % r for r in sorted(body & at)) or "none"))


if __name__ == "__main__":
    main()
