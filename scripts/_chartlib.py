"""Shared figure engine for scripts/chart_corpus and scripts/chart_vs.

Everything that decides how a cjson bench figure LOOKS lives here — palette, surface,
row parser, axis styling, the two figure kinds. The front-end scripts only decide what
to group by and what to call it, so the two tools cannot drift apart in style.

COLOR. The palette is the validated eight-slot categorical set, in fixed slot order,
and each library is pinned to a slot BY NAME — a run that drops a contender must not
repaint the survivors. Verified with the dataviz validator in both modes: worst
adjacent CVD ΔE 9.1 light / 8.4 dark under protanopia+deuteranopia (target ≥ 8),
worst adjacent normal-vision ΔE 19.6 / 19.3 (floor ≥ 15).

On the LIGHT surface three slots sit under 3:1 contrast, which obliges a non-colour
relief: every bar carries a black outline, and the numbers themselves live in
benches/results/*.txt and the README table. The dark surfaces clear 3:1 outright, so
prefer a dark mode for anything embedded on a dark page.

Eight slots do not stretch to eleven contenders, because a slot is pinned to the
LIBRARY and several libraries field two entries (yyjson / yyjson-poolalc, simdjson-dom /
simdjson-ondemand, cjson / cjson-dom / cjson-ondemand). Across figures that is the point.
Within one figure it is a collision, so hatches_for() hands the later claimants of a
colour a distinct pattern — see its docstring.

SURFACES. Three, and the third is not cosmetic: `dark` is this project's own dark
surface (#1a1a19), `github` is GitHub's Primer dark canvas (#0d1117) so a PNG dropped
into README.md under `prefers-color-scheme: dark` sits flush with the page instead of
floating on a lighter rectangle. `github` reuses the validated dark slots unchanged —
#0d1117 is DARKER than #1a1a19, so every slot's contrast against the surface can only
improve, and the validation carries over without a re-run.
"""

import os
import re
import sys
from collections import OrderedDict

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError as exc:  # pragma: no cover
    sys.exit(f"{os.path.basename(sys.argv[0])}: needs matplotlib and numpy ({exc})")


# the bench prints "### <label>  (<n> B)" before each corpus's groups
BANNER = re.compile(r"^###\s+(.+?)\s+\(\s*(\d+)\s*B\s*\)\s*$")
# and the pre-fix glue where a wide number runs into the impl column with no space
GLUED = re.compile(r"^(.*?[A-Za-z_.])([0-9]+\.[0-9]+)$")

METRICS = OrderedDict(
    [
        ("gbps", ("GB/s", "throughput (GB/s) — higher is better", False)),
        ("cyc", ("cyc/op", "cycles per op — lower is better", True)),
        ("ins", ("ins/op", "instructions per op — lower is better", True)),
        ("ipc", ("IPC", "instructions per cycle — higher is better", False)),
        ("bmiss", ("bmiss%", "branch misprediction rate (%) — lower is better", True)),
    ]
)

# %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
# palette
#
# Eight validated slots in fixed order. Libraries are pinned to a slot BY NAME so
# colour follows the entity and never its rank — dropping a contender from a run must
# not recolour the ones that remain.

SLOTS_LIGHT = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100",
               "#e87ba4", "#008300", "#4a3aa7", "#e34948"]
SLOTS_DARK = ["#3987e5", "#d95926", "#199e70", "#c98500",
              "#d55181", "#008300", "#9085e9", "#e66767"]

IMPL_SLOT = {
    "cjson": 0,               # blue
    "cjson-reuse": 1,         # orange
    "cjson-ondemand": 1,
    "cjson-dom": 0,
    "cjson-handloop": 2,
    "cjson-fp": 1,
    "cjson-items": 0,
    "cjson-at-indexed": 3,
    "cjson-index-only": 2,
    "yyjson": 2,              # aqua
    "yyjson-poolalc": 2,
    "simdjson-dom": 3,        # yellow
    "simdjson-ondemand": 3,
    "simdjson": 3,
    "rapidjson": 4,           # magenta
    "glaze": 5,               # green
    "glaze-lazy": 5,
    "boost.json": 6,          # violet
    "nlohmann": 7,            # red
}

# github/* are Primer tokens: bgColor-default, fgColor-default, fgColor-muted,
# borderColor-default, and canvas-inset for the bar outline.
SURFACE = {"light": "#fcfcfb", "dark": "#1a1a19", "github": "#0d1117"}
INK = {"light": ("#0b0b0b", "#52514e"), "dark": ("#ffffff", "#c3c2b7"),
       "github": ("#f0f6fc", "#9198a1")}
GRID = {"light": "#dedcd6", "dark": "#3a3a37", "github": "#3d444d"}
OUTLINE = {"light": "#000000", "dark": "#0b0b0b", "github": "#010409"}
SLOTS = {"light": SLOTS_LIGHT, "dark": SLOTS_DARK, "github": SLOTS_DARK}

# a light figure keeps the bare name; every dark surface earns a suffix, so one output
# directory can hold all three renderings of the same figure
SUFFIX = {"light": "", "dark": ".dark", "github": ".github"}
MODES = tuple(SUFFIX)


def is_cjson(impl):
    # the same 5-char prefix rule mb::is_cjson_impl uses, so the highlighting here and
    # the verdict line the bench prints can never disagree
    return impl[:5] == "cjson"


def colour_for(impl, mode, fallback_order):
    slots = SLOTS[mode]
    if impl in IMPL_SLOT:
        return slots[IMPL_SLOT[impl] % len(slots)]
    # an unpinned name still gets a stable slot: sorted position, not draw order
    return slots[(len(IMPL_SLOT) + fallback_order.index(impl)) % len(slots)]


def hatch_for(impl):
    # secondary encoding on our own bars — identity survives greyscale, a print-out,
    # and the CVD floor band without relying on hue alone
    if impl in ("cjson", "cjson-dom", "cjson-items", "cjson-handloop"):
        return "///"
    if is_cjson(impl):
        return "\\\\\\"
    return None


# neutral patterns first: these are handed to the SECOND claimant of a colour, and
# starting with cjson's own /// and \\\ would blur the one thing hatching is for
_HATCH_LADDER = ("...", "xxx", "---", "+++", "///", "\\\\\\")


def hatches_for(series, mode, fallback):
    """impl -> hatch, disambiguated within one figure.

    Colour is pinned to the LIBRARY, not the variant, so two series in one figure can
    land on the same slot: yyjson vs yyjson-poolalc, simdjson-dom vs simdjson-ondemand,
    cjson vs cjson-dom. Across figures that is exactly right — a library keeps its
    colour. Within one figure it is a collision, and it is what a `--by corpus` write_vs
    figure hits, where write/minify's yyjson sits beside write/minify-into's
    yyjson-poolalc in the same aqua. So any colour claimed more than once here hands the
    later claimants a distinct pattern; the first keeps whatever hatch_for pinned.
    """
    out = {s: hatch_for(s) for s in series}
    by_colour = OrderedDict()
    for s in series:
        by_colour.setdefault(colour_for(s, mode, fallback), []).append(s)

    for group in by_colour.values():
        if len(group) < 2:
            continue
        taken = {out[group[0]]} - {None}
        for s in group[1:]:
            want = out[s]
            if want is None or want in taken:
                want = next((h for h in _HATCH_LADDER if h not in taken), None)
            out[s] = want
            if want:
                taken.add(want)
    return out


def parse(text):
    """-> [ {corpus, size, op, impl, gbps, cyc, ins, ipc, bmiss} ]

    Accepts the 8-field layout (with GB/s) and the older 7- and 6-field ones, matching
    scripts/ab. Only the 8-field rows carry a real gbps.

    `corpus` comes from the "### label (n B)" banner when the bench emits one and falls
    back to "<n>B" when it does not — the stored parse_vs/write_vs baselines predate
    the banner, and they still have to chart.
    """
    rows = []
    corpus = None
    for ln in text.splitlines():
        m = BANNER.match(ln)
        if m:
            corpus = m.group(1)
            continue
        if ln.startswith("  ->") or ln.startswith("  !!") or ln.startswith("#"):
            continue

        f = ln.split()
        if len(f) in (5, 6, 7) and len(f) > 2:
            g = GLUED.match(f[2])
            if g:
                f = [f[0], f[1], g.group(1), g.group(2)] + f[3:]
        if len(f) not in (6, 7, 8):
            continue
        try:
            size = int(f[0])
            if len(f) == 8:
                gbps, cyc, ins, ipc, bm = (float(f[3]), float(f[4]), float(f[5]),
                                           float(f[6]), float(f[7]))
            elif len(f) == 7:
                gbps = float("nan")
                cyc, ins, ipc, bm = float(f[3]), float(f[4]), float(f[5]), float(f[6])
            else:
                gbps = float("nan")
                cyc, ipc, bm = float(f[3]), float(f[4]), float(f[5])
                ins = cyc * ipc
        except ValueError:
            continue

        rows.append(
            dict(corpus=corpus or f"{size}B", size=size, op=f[1], impl=f[2],
                 gbps=gbps, cyc=cyc, ins=ins, ipc=ipc, bmiss=bm)
        )
    return rows


def style_axes(ax, mode):
    primary, secondary = INK[mode]
    ax.set_facecolor(SURFACE[mode])
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID[mode])
        ax.spines[side].set_linewidth(1.0)
    ax.tick_params(colors=secondary, labelsize=8.5, length=3, width=1.0)
    ax.yaxis.label.set_color(secondary)
    ax.xaxis.label.set_color(secondary)
    ax.title.set_color(primary)


def fmt_value(v, metric):
    if metric in ("gbps", "ipc", "bmiss"):
        return f"{v:.2f}"
    # a 1 GB parse retires ~2.3e10 instructions, so the ladder has to reach G or the
    # labels come out as "22968.2M"
    if v >= 1e9:
        return f"{v / 1e9:.1f}G"
    if v >= 1e6:
        return f"{v / 1e6:.1f}M"
    if v >= 1e3:
        return f"{v / 1e3:.0f}K"
    return f"{v:.0f}"


def fmt_size(n):
    # DECIMAL units, to match the GB/s axis — that metric is bytes per nanosecond, so a
    # binary MiB in the title beside it would have the reader dividing by 1024 in one
    # place and 1000 in the other
    if n >= 1_000_000_000:
        return f"{n / 1e9:.2f} GB"
    if n >= 1_000_000:
        return f"{n / 1e6:.1f} MB"
    return f"{n / 1e3:.0f} kB"


# %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
# per-segment grouped bars

def draw(groups, key_name, series_name, metric, outdir, prefix, mode, note,
         title_fmt="cjson corpus benchmark — {group}   ({metric})"):
    label, ylabel, _lower_better = METRICS[metric]
    primary, secondary = INK[mode]
    written = []

    for gname, entries in groups.items():
        keys = list(OrderedDict.fromkeys(e[key_name] for e in entries))
        series = list(OrderedDict.fromkeys(e[series_name] for e in entries))
        if not keys or not series:
            continue

        lut = {(e[key_name], e[series_name]): e[metric] for e in entries}
        vals = [v for v in lut.values() if v == v and v > 0]
        if not vals:
            continue

        # a 300x spread (65 KB vs 26 MB on cyc/op) makes a linear axis useless — every
        # other bar becomes invisible
        logy = max(vals) / min(vals) > 100.0

        n = len(series)
        x = np.arange(len(keys), dtype=float)
        width = 0.84 / n
        fallback = sorted(series)

        # width tracks the number of GROUPS, not group x series — scaling by both makes
        # an eleven-corpus figure absurdly wide and unreadable at any zoom
        fig_w = max(9.0, min(24.0, 1.15 * len(keys) + 3.0))
        fig, ax = plt.subplots(figsize=(fig_w, 5.6), facecolor=SURFACE[mode])
        style_axes(ax, mode)
        hatches = hatches_for(series, mode, fallback)

        for i, s in enumerate(series):
            ys = [lut.get((k, s), float("nan")) for k in keys]
            off = (i - (n - 1) / 2.0) * width
            ax.bar(x + off, ys, width * 0.92, label=s,
                   color=colour_for(s, mode, fallback),
                   edgecolor=OUTLINE[mode], linewidth=0.7,
                   hatch=hatches[s], zorder=3)

        if logy:
            ax.set_yscale("log")
            ax.set_ylabel(ylabel + "   [log scale]", fontsize=9)
        else:
            ax.set_ylabel(ylabel, fontsize=9)

        ax.set_xticks(x)
        ax.set_xticklabels(keys, rotation=30, ha="right", fontsize=8.5)
        ax.set_title(title_fmt.format(group=gname, metric=label), fontsize=11.5, pad=10)
        ax.grid(axis="y", color=GRID[mode], linewidth=1.0, alpha=1.0, zorder=0)
        ax.set_axisbelow(True)
        leg = ax.legend(fontsize=8, ncol=min(len(series), 4), loc="best",
                        framealpha=0.92, facecolor=SURFACE[mode],
                        edgecolor=GRID[mode])
        for t in leg.get_texts():
            t.set_color(primary)
        if note:
            fig.text(0.005, 0.005, note, fontsize=6.5, color=secondary, va="bottom")
        fig.tight_layout()

        safe = re.sub(r"[^A-Za-z0-9._-]+", "_", gname)
        path = os.path.join(outdir, f"{prefix}{safe}.{metric}{SUFFIX[mode]}.png")
        fig.savefig(path, dpi=150, facecolor=SURFACE[mode])
        plt.close(fig)
        written.append(path)
    return written


# %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
# the headline figure

# the box the numbers came off, and the one asymmetry a reader has to know about
SUBTITLE = ("AMD Ryzen 7 3700U · GCC 16.1.1 · taskset -c 0 · medians of 7 · "
            "cjson-reuse borrows a warm scratch; every other row allocates per op")
# The two right-hand panels count cycles:u / instructions:u — USER SPACE ONLY — while
# GB/s is wall clock. An allocation-heavy row therefore looks better in cyc/op than it
# does in GB/s, because its page-fault and mmap time lands in the kernel where the
# counters do not follow. That is why the panels can rank differently, and the
# throughput panel is the one that reflects elapsed time.
FOOTNOTE = ("cyc/op and ins/op are user-space counters; GB/s is wall clock — rows that "
            "allocate per op pay kernel time the counters do not see, so the panels can "
            "rank differently")


def draw_headline(rows, corpus, op, outdir, mode, prefix="",
                  suptitle_fmt="cjson vs the field — full DOM {op} of {corpus}.json  ({size})",
                  subtitle=SUBTITLE, footnote=FOOTNOTE):
    """Three panels side by side on one PNG: GB/s, cyc/op, ins/op for one corpus.

    Three measures of wildly different scale — so three panels sharing a categorical
    axis, never one plot with three y-scales. Horizontal bars, because eight library
    names read straight across instead of rotated 30 degrees under a column.
    """
    sel = [r for r in rows if r["corpus"] == corpus and r["op"] == op]
    if not sel:
        return None

    size = sel[0]["size"]
    primary, secondary = INK[mode]

    # one order for all three panels so a library can be tracked across them; sorted by
    # throughput so the headline reads top-down. Colour is pinned by name, so the sort
    # never repaints anything.
    sel.sort(key=lambda r: (r["gbps"] if r["gbps"] == r["gbps"] else -1), reverse=True)
    impls = [r["impl"] for r in sel]
    fallback = sorted(impls)
    hatches = hatches_for(impls, mode, fallback)
    y = np.arange(len(impls), dtype=float)[::-1]

    panels = ["gbps", "cyc", "ins"]
    # The header and footer are FIXED INCHES, not fractions: a four-bar figure is half
    # the height of an eight-bar one, and a fractional title band collides with the
    # subtitle at the short end.
    fig_h = 0.52 * len(impls) + 2.9
    fig, axes = plt.subplots(1, 3, figsize=(15.0, fig_h), facecolor=SURFACE[mode])

    for ax, metric in zip(axes, panels):
        label, ylabel, lower_better = METRICS[metric]
        style_axes(ax, mode)
        vals = [r[metric] for r in sel]

        ax.barh(y, vals, 0.72,
                color=[colour_for(i, mode, fallback) for i in impls],
                edgecolor=OUTLINE[mode], linewidth=0.8,
                hatch=None, zorder=3)
        # hatch has to go on per-bar, barh takes one value for the whole container
        for bar, impl in zip(ax.patches, impls):
            h = hatches[impl]
            if h:
                bar.set_hatch(h)

        span = max(vals) / max(min(v for v in vals if v > 0), 1e-9)
        if span > 100.0:
            ax.set_xscale("log")
            ax.set_xlabel(ylabel + "   [log]", fontsize=9)
        else:
            ax.set_xlabel(ylabel, fontsize=9)
            ax.set_xlim(0, max(vals) * 1.22)
            # matplotlib's shared "1e6" offset in the corner is redundant when every
            # bar already carries a K/M label, and it reads as part of the last value
            ax.ticklabel_format(axis="x", style="plain")
            # bind metric as a default arg: the lambda is called at RENDER time, long
            # after this loop has moved on, and a late-bound `metric` would format every
            # panel with the last one (GB/s ticks coming out as 0,0,1,1,2)
            ax.xaxis.set_major_formatter(
                matplotlib.ticker.FuncFormatter(lambda v, _p, m=metric: fmt_value(v, m)))

        # eight marks per panel and the whole job of a headline is to carry the
        # numbers, so every bar is labelled here — this is also the contrast relief
        # the light surface owes for its three sub-3:1 slots
        for yy, v in zip(y, vals):
            ax.annotate(fmt_value(v, metric), (v, yy), xytext=(4, 0),
                        textcoords="offset points", va="center", ha="left",
                        fontsize=8, color=primary, zorder=4)

        ax.set_yticks(y)
        ax.set_yticklabels(impls if ax is axes[0] else [""] * len(impls), fontsize=9)
        if ax is axes[0]:
            for t in ax.get_yticklabels():
                t.set_color(primary)
        ax.set_title(label, fontsize=11, pad=8)
        ax.grid(axis="x", color=GRID[mode], linewidth=1.0, zorder=0)
        ax.set_axisbelow(True)
        ax.tick_params(axis="y", length=0)

    fig.suptitle(suptitle_fmt.format(op=op, corpus=corpus, size=fmt_size(size)),
                 fontsize=13.5, color=primary, y=1.0 - 0.32 / fig_h)
    fig.text(0.5, 1.0 - 0.66 / fig_h, subtitle, fontsize=8, color=secondary, ha="center")
    fig.text(0.5, 0.16 / fig_h, footnote, fontsize=7.5, color=secondary, ha="center")
    fig.tight_layout(rect=(0, 0.34 / fig_h, 1, 1.0 - 0.86 / fig_h))

    path = os.path.join(outdir, f"{prefix}headline-{corpus}{SUFFIX[mode]}.png")
    fig.savefig(path, dpi=150, facecolor=SURFACE[mode])
    plt.close(fig)
    return path
