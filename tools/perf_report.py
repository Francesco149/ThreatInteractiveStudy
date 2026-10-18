"""Turns benchmark CSVs (bench/*_<name>.csv, written by `ti_engine --bench NAME`) into markdown
tables for docs/PERF.md: the median GPU frame time of every configuration on every view, the mean
over views, the difference to the first configuration, and the passes that benchmark is about.
An `--bench all` run is split back into its benchmarks by configuration label. A `smoke` run
(the default configuration) also gets a per-pass table against TI's chart budgets, read from
ti_budget_ms() in src/bench.cpp.

    python -I tools/perf_report.py [bench_dir] > report.md

Uses the newest CSV for each benchmark name; a "-suffix" (renamed runs: smoke-nobc, prepass-stats)
makes a separate name.
"""
import csv
import glob
import os
import re
import sys
from collections import OrderedDict, defaultdict

# The passes each benchmark is about (profiler scope names; "Parent/Child" for nested scopes).
FOCUS = {
    "prepass": ["Depth pre-pass", "Base pass"],
    "order": ["Base pass"],
    "clears": ["Clears"],
    "lights": ["Local lights"],
    "msaa": ["Base pass", "Sun + ambient", "Local lights", "Compose", "Anti-aliasing"],
    "depth": ["Base pass", "Sun + ambient", "Local lights"],
    "aa": ["Tone map", "Anti-aliasing"],
    "shadows": ["Shadows", "Sun + ambient"],
    "localshadows": ["Shadows/Local lights", "Local lights"],
    "post": ["Tone map", "Exposure", "Bloom"],
    "gi": ["GTAO", "Sun + ambient", "GI relight"],
    "motionblur": ["Motion blur"],
    "sss": ["Subsurface scattering"],
    "vfx": ["Transparents + VFX"],
    "lod": ["Base pass", "Shadows"],
}

# Configuration label prefix -> benchmark, to split an `--bench all` run (labels in src/bench.cpp).
SECTIONS = [
    ("prepass=", "prepass"), ("order=", "order"), ("clears=", "clears"), ("lights=", "lights"),
    ("msaa=", "msaa"), ("depth=", "depth"), ("aa=", "aa"), ("shadows=", "shadows"),
    ("local shadows=", "localshadows"), ("tonemap=", "post"), ("auto exposure", "post"),
    ("bloom off", "post"), ("auto white balance", "post"), ("gi=", "gi"), ("mb=", "motionblur"),
    ("sss=", "sss"), ("vfx=", "vfx"), ("lod=", "lod"),
]


PS = " [PS invocations]"  # suffix of the --stats metrics in the CSVs (src/bench.cpp)


def section_of(config):
    for prefix, name in SECTIONS:
        if config.startswith(prefix):
            return name
    return "other"


def load(path):
    rows = list(csv.DictReader(open(path, encoding="utf-8")))
    data = defaultdict(dict)  # (config, view) -> metric -> ms
    configs, views = OrderedDict(), OrderedDict()
    for r in rows:
        key = (r["config"], r["view"])
        data[key][r["metric"]] = float(r["ms"])
        configs[r["config"]] = 1
        views[r["view"]] = 1
    return data, list(configs), list(views)


def mean_over_views(data, config, views, metric):
    vals = [data[(config, v)].get(metric) for v in views if data[(config, v)].get(metric) is not None]
    return sum(vals) / len(vals) if vals else None


def report(name, source, data, configs, views):
    out = [f"### `{name}`  ({source})", ""]
    out.append("Median GPU frame time (ms) per view; mean over views; difference to the first row.")
    out.append("")
    out.append("| config | " + " | ".join(views) + " | **mean** | diff |")
    out.append("|---|" + "---|" * (len(views) + 2))
    base = None
    for c in configs:
        vals = [data[(c, v)].get("GPU frame (median)", float("nan")) for v in views]
        mean = sum(vals) / len(vals)
        if base is None:
            base = mean
        diff = mean - base
        out.append(f"| {c} | " + " | ".join(f"{x:.3f}" for x in vals) + f" | **{mean:.3f}** | {diff:+.3f} |")
    focus = FOCUS.get(name.split("-")[0], [])   # "prepass-stats" -> prepass
    if focus:
        out.append("")
        out.append("Mean over views of the passes this benchmark is about (ms):")
        out.append("")
        out.append("| config | " + " | ".join(focus) + " |")
        out.append("|---|" + "---|" * len(focus))
        for c in configs:
            cells = []
            for p in focus:
                m = mean_over_views(data, c, views, p)
                cells.append(f"{m:.3f}" if m is not None else "-")
            out.append(f"| {c} | " + " | ".join(cells) + " |")
        # Runs with --stats also carry pixel-shader invocations per pass.
        if any(mean_over_views(data, c, views, p + PS) is not None for c in configs for p in focus):
            out.append("")
            out.append("Mean over views of the pixel-shader invocations of those passes (millions):")
            out.append("")
            out.append("| config | " + " | ".join(focus) + " |")
            out.append("|---|" + "---|" * len(focus))
            for c in configs:
                cells = []
                for p in focus:
                    m = mean_over_views(data, c, views, p + PS)
                    cells.append(f"{m / 1e6:.2f}" if m is not None else "-")
                out.append(f"| {c} | " + " | ".join(cells) + " |")
    out.append("")
    return "\n".join(out)


def ti_budgets():
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "bench.cpp"), encoding="utf-8").read()
    return {m.group(1): float(m.group(2)) for m in re.finditer(r'\{"([^"]+)", ([0-9.]+)f\}', src)}


def budget_report(source, data, configs, views):
    """Per-pass mean and worst view of the default configuration against TI's chart budgets."""
    budgets = ti_budgets()
    c = configs[0]
    passes = [m for m in data[(c, views[0])] if "/" not in m and PS not in m and not m.startswith(("GPU frame", "CPU"))]
    out = [f"### Default configuration vs TI's chart budgets  ({source})", ""]
    out.append("Mean over views and worst view (ms) of every top-level pass; budgets from the video's chart "
               "(1080p, RTX 3060).")
    out.append("")
    out.append("| pass | mean | worst view | TI budget | |")
    out.append("|---|---|---|---|---|")
    for p in passes:
        m = mean_over_views(data, c, views, p)
        worst_v = max(views, key=lambda v: data[(c, v)].get(p, -1))
        worst = data[(c, worst_v)].get(p, 0)
        b = budgets.get(p)
        verdict = "" if b is None else ("within" if m <= b else "OVER")
        out.append(f"| {p} | {m:.3f} | {worst:.3f} ({worst_v}) | {'' if b is None else f'{b:.2f}'} | {verdict} |")
    med = mean_over_views(data, c, views, "GPU frame (median)")
    total_b = sum(budgets.values())
    out.append(f"| **GPU frame (median)** | **{med:.3f}** | | {total_b:.2f} (sum of the mapped budgets) | |")
    out.append("")
    return "\n".join(out)


def main():
    bench_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "bench")
    newest = {}
    for path in sorted(glob.glob(os.path.join(bench_dir, "*.csv"))):
        name = os.path.basename(path)[16:-4]  # strip "YYYYmmdd_HHMMSS_" and ".csv"
        newest[name] = path  # sorted by timestamp: the last one wins
    for name in sorted(newest):
        path = newest[name]
        data, configs, views = load(path)
        source = os.path.basename(path)
        if name == "all":
            sections = OrderedDict()
            for c in configs:
                sections.setdefault(section_of(c), []).append(c)
            for sec, cs in sections.items():
                if sec in newest and newest[sec] > path:
                    continue  # a newer standalone run of this benchmark replaces the section
                print(report(sec, source, data, cs, views))
        else:
            if name == "smoke" or name.startswith("smoke"):
                print(budget_report(source, data, configs, views))
            print(report(name, source, data, configs, views))


if __name__ == "__main__":
    main()
