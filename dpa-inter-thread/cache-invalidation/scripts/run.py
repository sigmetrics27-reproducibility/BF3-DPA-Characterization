#!/usr/bin/env python3
"""run sharing configurations serially in randomized order"""
import argparse
import hashlib
import json
from pathlib import Path
import random
import subprocess
import time

from topology import ROOT, TOOL, capture

PERIODS = (256, 512, 1024, 2048, 4096)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output-dir", type=Path, default=ROOT / "results/frequency-sweep")
    p.add_argument("--binary", type=Path, default=ROOT / "build/dpa_cache_invalidation")
    p.add_argument("--device-name", default="mlx5_0")
    p.add_argument("--reads", type=int, default=1000000)
    p.add_argument("--warmup", type=int, default=5)
    p.add_argument("--samples", type=int, default=30)
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--seed", type=int, default=290)
    p.add_argument("--cross-eus", default="16", help="comma-separated B EUs on different cores")
    p.add_argument("--periods", default=",".join(map(str, PERIODS)), help="comma-separated write periods")
    args = p.parse_args()
    if args.reads < 1 or args.warmup < 0 or args.samples < 1 or args.repeats < 1:
        p.error("reads, samples and repeats must be positive; warmup must be nonnegative")
    try:
        cross_eus = [int(value) for value in args.cross_eus.split(",")]
        periods = [int(value) for value in args.periods.split(",")]
    except ValueError:
        p.error("cross-eus and periods must be comma-separated integers")
    if (not cross_eus or not periods or len(cross_eus) != len(set(cross_eus)) or
            len(periods) != len(set(periods)) or any(period < 1 for period in periods)):
        p.error("cross-eus and periods must be nonempty and contain unique positive values")
    binary = args.binary.resolve()
    output = args.output_dir.resolve()
    if output.exists() and any(output.iterdir()):
        p.error(f"output directory is not empty: {output}; choose a fresh directory")
    rows = capture(output, args.device_name)
    by_eu = {r["eu_id"]: r for r in rows}
    if any(eu not in by_eu for eu in (0, 1, *cross_eus)) or by_eu[0]["core_id"] != by_eu[1]["core_id"]:
        raise ValueError("EU 0/1 must exist on the same core, per captured topology")
    cross_cores = [by_eu[eu]["core_id"] for eu in cross_eus]
    if by_eu[0]["core_id"] in cross_cores or len(cross_cores) != len(set(cross_cores)):
        raise ValueError("Each cross EU must be on a different core from A and the other cross EUs")
    manifest = {"binary": str(binary), "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "device": args.device_name, "seed": args.seed, "reads": args.reads,
                "warmup": args.warmup, "samples": args.samples, "repeats": args.repeats,
                "periods": periods, "cross_eus": cross_eus, "cases": []}
    rng = random.Random(args.seed)
    for repeat in range(args.repeats):
        configs = [(eu_b, layout, period)
                   for eu_b in (1, *cross_eus)
                   for layout in ("same-line", "separate-line")
                   for period in periods]
        rng.shuffle(configs)
        for eu_b, layout, period in configs:
            placement = "same-core" if eu_b == 1 else "cross-core"
            case = (f"round{repeat + 1}_{placement}_{layout}_{period}" if cross_eus == [16] else
                    f"round{repeat + 1}_b{eu_b}_{layout}_{period}")
            folder = output / case
            folder.mkdir(parents=True, exist_ok=True)
            argv = [str(binary), "--device-name", args.device_name,
                    "--topology-file", str(output / "topology.csv"), "--eu-list", f"0,{eu_b}",
                    "--layout", layout, "--period-cycles", str(period), "--reads", str(args.reads),
                    "--warmup", str(args.warmup), "--samples", str(args.samples)]
            start = time.monotonic()
            try:
                result = subprocess.run(argv, text=True, capture_output=True, timeout=300)
                status = "ok" if result.returncode == 0 else f"exit_{result.returncode}"
                stdout, stderr = result.stdout, result.stderr
            except subprocess.TimeoutExpired as e:
                status = "host_timeout"
                stdout = e.stdout.decode() if isinstance(e.stdout, bytes) else e.stdout or ""
                stderr = e.stderr.decode() if isinstance(e.stderr, bytes) else e.stderr or ""
            (folder / "samples.csv").write_text(stdout)
            (folder / "stderr.txt").write_text(stderr)
            entry = dict(case=case, repeat=repeat + 1, placement=placement, layout=layout,
                         period_cycles=period, eu_list=f"0,{eu_b}", core_b=by_eu[eu_b]["core_id"], argv=argv,
                         status=status, seconds=time.monotonic() - start, directory=case)
            manifest["cases"].append(entry)
            (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            print(case, status, f"{entry['seconds']:.1f}s", flush=True)
            if status != "ok":
                raise RuntimeError(f"{case} failed; inspect {folder / 'stderr.txt'}")
    status = subprocess.run([TOOL, "info", "status", "--dpa_device", args.device_name],
                            text=True, capture_output=True, timeout=30)
    (output / "status-after.txt").write_text(status.stdout + status.stderr)
    manifest["device_status_after_exit"] = status.returncode
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Saved {len(manifest['cases'])} configurations in {output}")


if __name__ == "__main__":
    main()
