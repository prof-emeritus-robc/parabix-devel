"""Makes the charts of tokenizer-test/README.md (python3 make_figures.py [OUTDIR]; needs matplotlib).
Every number is copied from the results of that README (BPE table and pre-tokenizer numbers)."""
import sys, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

import os
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
INPUTS = ["val_4MB\n(web text)", "code_4MB\n(C/C++ source)", "multi_4MB\n(15 languages)"]
# Median ms (Parabix, tiktoken, HF 0.23.3) and the speed-ups, as in the README results table
# (Apple M3, run 2026-11-19, commit 4225b1fab, 3 rounds of 5 runs, median of the round medians).
# Speed-ups are the unrounded ratio of medians, not recomputed from the rounded times.
T1 = [(12.9, 137, 1098), (16.2, 238, 1162), (17.9, 217, 1339)]
T4 = [(3.86, 48.7, 347), (5.89, 85.5, 360), (5.89, 89.8, 392)]
SP1 = {1: [10.7, 14.8, 12.1], 2: [85.4, 71.9, 74.9]}   # 1 thread: vs tiktoken (1), vs HF (2)
SP4 = {1: [12.6, 14.5, 15.2], 2: [90.0, 61.2, 66.5]}   # 4 threads
# Thesis slate palette (main.tex: figInk, figLine, figMid, figBorder, figTitle, figSub).
# Series as in the thesis size-sweep figure: tokenizer figInk, tiktoken figLine, HF figMid.
COL_BY_THEME = {
    "light": {"Parabix": "#2F5B78", "tiktoken": "#7C8D99", "HF": "#8FA9BC"},
    "dark":  {"Parabix": "#7FB0D0", "tiktoken": "#9AA9B4", "HF": "#4F6B80"},   # lightened for a dark background
}
COL = COL_BY_THEME["light"]
# bench_pretokenizers.py, run 2026-11-19 (1 thread, 3 rounds of 5 calls, HF median / Parabix median,
# val, code, multi).
PRETOK = [("whitespace", 5.5, 6.1, 4.6), ("whitespacesplit", 4.7, 4.9, 4.4),
          ("digits", 2.3, 2.9, 2.2), ("punctuation", 8.7, 12.9, 6.9),
          ("simplewordboundaries", 12.6, 10.1, 10.3), ("chardelimiter", 1.4, 1.6, 1.4),
          ("bert", 13.1, 13.6, 11.9), ("sequence_whitespace_punctuation", 13.2, 13.5, 11.1),
          ("bytelevel (codepoint path)", 17.2, 19.2, 14.3), ("bytelevel (byte path)", 26.1, 27.8, 22.1)]

THEMES = {
    "light": dict(fg="#1E2A33", muted="#4F5F6B", grid="#C9D0D6", bg="#ffffff"),
    "dark":  dict(fg="#e6edf3", muted="#9198a1", grid="#30363d", bg="#0d1117"),
}

def style(ax, t):
    ax.set_facecolor(t["bg"])
    for s in ("top", "right"): ax.spines[s].set_visible(False)
    for s in ("left", "bottom"): ax.spines[s].set_color(t["grid"])
    ax.tick_params(colors=t["muted"], labelsize=9)
    ax.yaxis.label.set_color(t["muted"]); ax.xaxis.label.set_color(t["muted"])

def speed_fig(theme):
    global COL
    COL = COL_BY_THEME[theme]
    t = THEMES[theme]
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.4), facecolor=t["bg"], sharey=True)
    for ax, data, title in zip(axes, (T1, T4), ("1 thread", "4 threads")):
        style(ax, t)
        x = np.arange(3); w = 0.26
        for i, (name, key) in enumerate((("Parabix", 0), ("tiktoken", 1), ("HF", 2))):
            vals = [r[key] for r in data]
            bars = ax.bar(x + (i - 1) * w, vals, w, color=COL[name], label={"HF": "HuggingFace 0.23.3"}.get(name, name))
            for b, v in zip(bars, vals):
                ax.text(b.get_x() + b.get_width() / 2, v * 1.12, f"{v:,.1f}" if v < 100 else f"{v:,.0f}",
                        ha="center", va="bottom", fontsize=8, color=t["fg"])
        ax.set_yscale("log"); ax.set_ylim(1, 4000)
        ax.set_xticks(x); ax.set_xticklabels(INPUTS)
        ax.set_title(title, color=t["fg"], fontsize=12, fontweight="bold", loc="left")
        ax.yaxis.grid(True, color=t["grid"], lw=0.6); ax.set_axisbelow(True)
    axes[0].set_ylabel("ms per file, log scale (lower is better)")
    h, l = axes[0].get_legend_handles_labels()
    fig.legend(h, l, loc="upper center", ncol=3, frameon=False, labelcolor=t["fg"], fontsize=10)
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(f"{OUT}/speed-time-{theme}.png", dpi=170, facecolor=t["bg"]); plt.close(fig)

def speedup_fig(theme):
    global COL
    COL = COL_BY_THEME[theme]
    t = THEMES[theme]
    fig, axes = plt.subplots(1, 2, figsize=(11, 3.8), facecolor=t["bg"])
    for ax, ref, ttl in ((axes[0], 1, "Speed-up over tiktoken 0.14.0"), (axes[1], 2, "Speed-up over HuggingFace tokenizers 0.23.3")):
        style(ax, t)
        x = np.arange(3); w = 0.36
        for i, (sp, lab, a) in enumerate(((SP1, "1 thread", 1.0), (SP4, "4 threads", 0.55))):
            vals = sp[ref]
            bars = ax.bar(x + (i - .5) * w, vals, w, color=COL["Parabix"], alpha=a, label=lab)
            for b, v in zip(bars, vals):
                ax.text(b.get_x() + b.get_width() / 2, v + max(vals) * 0.015, f"{v:.1f}×",
                        ha="center", va="bottom", fontsize=9, color=t["fg"], fontweight="bold")
        ax.set_xticks(x); ax.set_xticklabels(INPUTS)
        ax.set_title(ttl, color=t["fg"], fontsize=11, fontweight="bold", loc="left")
        ax.yaxis.grid(True, color=t["grid"], lw=0.6); ax.set_axisbelow(True)
        ax.set_ylim(0, max(max(sp[ref]) for sp in (SP1, SP4)) * 1.15)
    axes[0].set_ylabel("× faster (ratio of medians)")
    h, l = axes[0].get_legend_handles_labels()
    fig.legend(h, l, loc="upper center", ncol=2, frameon=False, labelcolor=t["fg"], fontsize=10)
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(f"{OUT}/speedup-{theme}.png", dpi=170, facecolor=t["bg"]); plt.close(fig)

def pretok_fig(theme):
    global COL
    COL = COL_BY_THEME[theme]
    t = THEMES[theme]
    fig, ax = plt.subplots(figsize=(9, 5.4), facecolor=t["bg"]); style(ax, t)
    names = [p[0] for p in PRETOK]; y = np.arange(len(names)); h = 0.26
    shades = [(1.0, "val_4MB"), (0.7, "code_4MB"), (0.45, "multi_4MB")]
    for i, (a_, lab) in enumerate(shades):
        vals = [p[1 + i] for p in PRETOK]
        bars = ax.barh(y + (i - 1) * h, vals, h, color=COL["Parabix"], alpha=a_, label=lab)
        for bb, v in zip(bars, vals):
            ax.text(v + 0.25, bb.get_y() + bb.get_height() / 2, f"{v:.1f}", va="center", fontsize=7, color=t["muted"])
    ax.set_yticks(y); ax.set_yticklabels(names, fontsize=9, color=t["fg"]); ax.invert_yaxis()
    ax.set_xlabel("× faster than HuggingFace 0.23.3 (ratio of medians, 1 thread)")
    ax.xaxis.grid(True, color=t["grid"], lw=0.6); ax.set_axisbelow(True); ax.set_xlim(0, 31)
    ax.legend(frameon=False, labelcolor=t["fg"], fontsize=9, loc="center right")
    fig.tight_layout(); fig.savefig(f"{OUT}/pretokenizers-{theme}.png", dpi=170, facecolor=t["bg"]); plt.close(fig)

for th in THEMES:
    speed_fig(th); speedup_fig(th); pretok_fig(th)
print("speed-ups used:", SP1, SP4)
