#!/usr/bin/env python3
"""compare same-line overhead across B-core placements from a run.py sweep"""
import argparse
import csv
import json
from pathlib import Path
from statistics import median


def write_csv(path, rows):
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, rows[0].keys(), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("directory", type=Path)
    args = p.parse_args()
    root = args.directory
    manifest = json.loads((root / "manifest.json").read_text())
    topology = {int(row["eu_id"]): int(row["core_id"])
                for row in csv.DictReader((root / "topology.csv").open())}
    cases = []
    for entry in manifest["cases"]:
        if entry["status"] != "ok":
            raise ValueError(f"Failed configuration: {entry['case']}")
        eu_a, eu_b = map(int, entry["eu_list"].split(","))
        if eu_a != 0 or topology[eu_b] != entry["core_b"]:
            raise ValueError(f"Incorrect EU/core mapping: {entry['case']}")
        with (root / entry["directory"] / "samples.csv").open() as f:
            rows = list(csv.DictReader(f))
        measured = [row for row in rows if row["phase"] == "measure"]
        if (len(rows) != manifest["warmup"] + manifest["samples"] or
                len(measured) != manifest["samples"] or
                any(row["status"] != "ok" or int(row["eu_b"]) != eu_b or
                    int(row["core_b"]) != topology[eu_b] for row in rows)):
            raise ValueError(f"Invalid samples: {entry['case']}")
        cases.append(dict(repeat=entry["repeat"], eu_b=eu_b, core_b=topology[eu_b],
                          layout=entry["layout"], period_cycles=entry["period_cycles"],
                          cycles_per_read=median(float(row["cycles_per_read"]) for row in measured),
                          writes_per_1000_cycles=median(float(row["writes_per_1000_cycles"]) for row in measured)))
    expected = manifest["repeats"] * len(manifest["periods"]) * 2 * (1 + len(manifest["cross_eus"]))
    if len(cases) != expected:
        raise ValueError(f"Expected {expected} cases, found {len(cases)}")
    write_csv(root / "case_summary.csv", cases)

    pairs = []
    for repeat in range(1, manifest["repeats"] + 1):
        for period in manifest["periods"]:
            for eu_b in (1, *manifest["cross_eus"]):
                group = [row for row in cases if row["repeat"] == repeat and
                         row["period_cycles"] == period and row["eu_b"] == eu_b]
                if len(group) != 2 or {row["layout"] for row in group} != {"same-line", "separate-line"}:
                    raise ValueError(f"Missing layout pair: round={repeat} EU={eu_b} period={period}")
                same = next(row for row in group if row["layout"] == "same-line")
                separate = next(row for row in group if row["layout"] == "separate-line")
                rate_same, rate_separate = same["writes_per_1000_cycles"], separate["writes_per_1000_cycles"]
                rate_average = (rate_same + rate_separate) / 2
                target = 1000 / period
                valid = (abs(rate_same - rate_separate) / rate_average <= .05 and
                         abs(rate_same / target - 1) <= .05 and
                         abs(rate_separate / target - 1) <= .05)
                pairs.append(dict(repeat=repeat, period_cycles=period, eu_b=eu_b,
                                  core_b=topology[eu_b], status="ok" if valid else "rate_mismatch",
                                  same_cycles_per_read=same["cycles_per_read"],
                                  separate_cycles_per_read=separate["cycles_per_read"],
                                  same_line_overhead=same["cycles_per_read"] - separate["cycles_per_read"] if valid else "",
                                  observed_writes_per_1000_cycles=rate_average))
    write_csv(root / "paired_summary.csv", pairs)

    contrasts = []
    for repeat in range(1, manifest["repeats"] + 1):
        for period in manifest["periods"]:
            base = next(row for row in pairs if row["repeat"] == repeat and
                        row["period_cycles"] == period and row["eu_b"] == 1)
            for eu_b in manifest["cross_eus"]:
                cross = next(row for row in pairs if row["repeat"] == repeat and
                             row["period_cycles"] == period and row["eu_b"] == eu_b)
                rate_base = base["observed_writes_per_1000_cycles"]
                rate_cross = cross["observed_writes_per_1000_cycles"]
                matched = abs(rate_base - rate_cross) / ((rate_base + rate_cross) / 2) <= .05
                valid = base["status"] == cross["status"] == "ok" and matched
                mean_cost = (base["same_cycles_per_read"] + base["separate_cycles_per_read"] +
                             cross["same_cycles_per_read"] + cross["separate_cycles_per_read"]) / 4
                difference = cross["same_line_overhead"] - base["same_line_overhead"] if valid else ""
                contrasts.append(dict(repeat=repeat, period_cycles=period, eu_b=eu_b,
                                      core_b=topology[eu_b], status="ok" if valid else "rate_mismatch",
                                      cross_minus_same_cycles_per_read=difference,
                                      mean_cycles_per_read=mean_cost,
                                      cross_minus_same_percent=100 * difference / mean_cost if valid else ""))
    write_csv(root / "topology_contrast.csv", contrasts)

    valid = [row for row in contrasts if row["status"] == "ok"]
    if valid:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(figsize=(6.5, 3.8))
        colors = plt.get_cmap("tab10")
        for index, period in enumerate(sorted(manifest["periods"], reverse=True)):
            color = colors(index % 10)
            points = [row for row in valid if row["period_cycles"] == period]
            cores = sorted({row["core_b"] for row in points})
            ax.scatter([row["core_b"] for row in points],
                       [row["cross_minus_same_percent"] for row in points],
                       color=color, s=22, alpha=.35)
            ax.plot(cores, [median(row["cross_minus_same_percent"] for row in points
                                   if row["core_b"] == core) for core in cores],
                    color=color, marker="o", label=f"B period {period} cycles")
        ax.axhline(0, color="black", lw=.7)
        ax.set_xticks(sorted({row["core_b"] for row in valid}))
        ax.set_xlabel("B core ID (A on core 0; same-core control: B on core 0)")
        ax.set_ylabel("Cross-core - same-core overhead\n(% of mean read cost)")
        ax.grid(axis="y", alpha=.2)
        ax.legend()
        fig.tight_layout()
        for suffix in ("png", "pdf"):
            fig.savefig(root / f"core_contrast.{suffix}", dpi=180)
        plt.close(fig)
    print(f"{len(valid)}/{len(contrasts)} rate-matched topology contrasts")


if __name__ == "__main__":
    main()
