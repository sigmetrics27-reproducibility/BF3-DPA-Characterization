#!/usr/bin/env python3
"""check write-rate matching and plot same-line overhead from saved samples"""
import argparse
import csv
import json
from pathlib import Path
from statistics import median


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("directory", type=Path)
    p.add_argument("--no-plot", action="store_true")
    args = p.parse_args()
    root = args.directory
    manifest = json.loads((root / "manifest.json").read_text())
    cases = []
    for entry in manifest["cases"]:
        if entry["status"] != "ok":
            raise ValueError(f"Failed configuration: {entry['case']}")
        with (root / entry["directory"] / "samples.csv").open() as f:
            raw = list(csv.DictReader(f))
        if len(raw) != manifest["warmup"] + manifest["samples"] or any(r["status"] != "ok" for r in raw):
            raise ValueError(f"Missing or invalid batches: {entry['case']}")
        measured = [r for r in raw if r["phase"] == "measure"]
        if len(measured) != manifest["samples"]:
            raise ValueError(f"Wrong measured batch count: {entry['case']}")
        rows = [float(r["cycles_per_read"]) for r in measured]
        rates = [float(r["writes_per_1000_cycles"]) for r in measured]
        missed = [int(r["missed"]) for r in measured]
        cases.append(dict(case=entry["case"], repeat=entry["repeat"], placement=entry["placement"],
                          layout=entry["layout"], period_cycles=entry["period_cycles"],
                          cycles_per_read=median(rows), writes_per_1000_cycles=median(rates),
                          median_missed=median(missed), samples=len(measured)))
    with (root / "case_summary.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, list(cases[0]), lineterminator="\n")
        writer.writeheader(); writer.writerows(cases)
    periods = sorted({r["period_cycles"] for r in cases})
    comparisons = []
    for repeat in range(1, manifest["repeats"] + 1):
        for placement in ("same-core", "cross-core"):
            for period in periods:
                group = [r for r in cases if r["repeat"] == repeat and r["placement"] == placement and r["period_cycles"] == period]
                if len(group) != 2 or {r["layout"] for r in group} != {"same-line", "separate-line"}:
                    raise ValueError(f"Missing paired case: round={repeat} {placement} {period}")
                same = next(r for r in group if r["layout"] == "same-line")
                separate = next(r for r in group if r["layout"] == "separate-line")
                target = 1000 / period
                rate_same, rate_separate = same["writes_per_1000_cycles"], separate["writes_per_1000_cycles"]
                mismatch = abs(rate_same - rate_separate) / ((rate_same + rate_separate) / 2)
                valid = (mismatch <= .05 and abs(rate_same / target - 1) <= .05 and
                         abs(rate_separate / target - 1) <= .05)
                comparisons.append(dict(repeat=repeat, placement=placement, period_cycles=period,
                    status="ok" if valid else "rate_mismatch", same_cycles_per_read=same["cycles_per_read"],
                    separate_cycles_per_read=separate["cycles_per_read"],
                    delta_cycles_per_read=same["cycles_per_read"] - separate["cycles_per_read"] if valid else "",
                    same_writes_per_1000_cycles=rate_same, separate_writes_per_1000_cycles=rate_separate,
                    rate_difference_percent=100 * mismatch, same_median_missed=same["median_missed"],
                    separate_median_missed=separate["median_missed"]))
    with (root / "comparisons.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, list(comparisons[0]), lineterminator="\n")
        writer.writeheader(); writer.writerows(comparisons)
    valid = [r for r in comparisons if r["status"] == "ok"]
    contrasts = []
    for repeat in range(1, manifest["repeats"] + 1):
        for period in periods:
            pair = [r for r in comparisons if r["repeat"] == repeat and r["period_cycles"] == period]
            same = next(r for r in pair if r["placement"] == "same-core")
            cross = next(r for r in pair if r["placement"] == "cross-core")
            rate_same = (same["same_writes_per_1000_cycles"] + same["separate_writes_per_1000_cycles"]) / 2
            rate_cross = (cross["same_writes_per_1000_cycles"] + cross["separate_writes_per_1000_cycles"]) / 2
            matched = abs(rate_same - rate_cross) / ((rate_same + rate_cross) / 2) <= .05
            ok = same["status"] == cross["status"] == "ok" and matched
            mean_read_cycles = (same["same_cycles_per_read"] + same["separate_cycles_per_read"] +
                                cross["same_cycles_per_read"] + cross["separate_cycles_per_read"]) / 4
            difference = cross["delta_cycles_per_read"] - same["delta_cycles_per_read"] if ok else ""
            contrasts.append(dict(repeat=repeat, period_cycles=period,
                status="ok" if ok else "rate_mismatch", observed_writes_per_1000_cycles=(rate_same + rate_cross) / 2,
                cross_minus_same_cycles_per_read=difference, mean_cycles_per_read=mean_read_cycles,
                cross_minus_same_percent=100 * difference / mean_read_cycles if ok else ""))
    with (root / "topology_contrast.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, list(contrasts[0]), lineterminator="\n")
        writer.writeheader(); writer.writerows(contrasts)
    valid_contrasts = [r for r in contrasts if r["status"] == "ok"]
    if not args.no_plot and valid:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, (ax, contrast_ax) = plt.subplots(2, 1, figsize=(6.5, 5.5), sharex=True,
                                               gridspec_kw={"height_ratios": [2, 1]})
        for placement, color, marker in (("same-core", "#277da1", "o"), ("cross-core", "#f3722c", "s")):
            x_medians, y_medians = [], []
            for period in reversed(periods):
                points = [r for r in valid if r["placement"] == placement and r["period_cycles"] == period]
                if not points: continue
                xs = [(r["same_writes_per_1000_cycles"] + r["separate_writes_per_1000_cycles"]) / 2 for r in points]
                ys = [r["delta_cycles_per_read"] for r in points]
                ax.scatter(xs, ys, color=color, marker=marker, s=22, alpha=.45)
                x_medians.append(median(xs)); y_medians.append(median(ys))
            ax.plot(x_medians, y_medians, color=color, marker=marker, label=placement.replace("-", " "))
        contrast_x, contrast_y = [], []
        for period in reversed(periods):
            points = [r for r in valid_contrasts if r["period_cycles"] == period]
            if not points: continue
            xs = [r["observed_writes_per_1000_cycles"] for r in points]
            ys = [r["cross_minus_same_percent"] for r in points]
            contrast_ax.scatter(xs, ys, color="#555555", s=22, alpha=.45)
            contrast_x.append(median(xs)); contrast_y.append(median(ys))
        if contrast_x: contrast_ax.plot(contrast_x, contrast_y, color="#555555", marker="D", ms=4)
        ax.axhline(0, color="black", lw=.7)
        contrast_ax.axhline(0, color="black", lw=.7)
        ax.set_ylabel("Same-line overhead (cycles/read)")
        contrast_ax.set_ylabel("Cross - same\n(% of mean read cost)")
        reference_costs = [r["mean_cycles_per_read"] for r in valid_contrasts]
        if reference_costs:
            contrast_ax.text(.02, .07,
                             f"Mean of 4 cases: {min(reference_costs):.1f}-{max(reference_costs):.1f} cycles/read",
                             transform=contrast_ax.transAxes, fontsize=8, color="#555555")
        contrast_ax.set_xlabel("Observed B writes / 1000 A cycles (target period below)")
        ticks = [median(r["writes_per_1000_cycles"] for r in cases if r["period_cycles"] == period)
                 for period in reversed(periods)]
        contrast_ax.set_xscale("log", base=2)
        contrast_ax.set_xticks(ticks, [f"{rate:.3f}\n({period} cyc)"
                                     for period, rate in zip(reversed(periods), ticks)])
        for axes in (ax, contrast_ax): axes.grid(axis="y", alpha=.2)
        ax.legend()
        fig.tight_layout()
        for suffix in ("png", "pdf"):
            fig.savefig(root / f"same_line_overhead.{suffix}", dpi=180)
        plt.close(fig)
    (root / "analysis.json").write_text(json.dumps(dict(valid_comparisons=len(valid),
        invalid_comparisons=len(comparisons) - len(valid), valid_topology_contrasts=len(valid_contrasts),
        invalid_topology_contrasts=len(contrasts) - len(valid_contrasts),
        metric="Median of batch cycles/read; lower panel is cross-minus-same overhead divided by the mean of all four placement/layout medians per independent run",
        caveat="Indirect evidence consistent with coherence maintenance, not invalidation counts"), indent=2) + "\n")
    print(f"{len(valid)}/{len(comparisons)} rate-matched comparisons; see {root / 'comparisons.csv'}")


if __name__ == "__main__":
    main()
