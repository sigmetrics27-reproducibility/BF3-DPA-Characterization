import argparse
import csv
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--device-name", default="mlx5_0")
    parser.add_argument("--output", type=Path, default=Path("build/topology.csv"))
    args = parser.parse_args()

    output = subprocess.check_output(
        ["/opt/mellanox/doca/tools/dpaeumgmt", "info", "chip", "--dpa_device", args.device_name],
        text=True,
        timeout=30,
    )
    rows = [
        (int(eu), int(core), int(slot))
        for core, eu, slot in re.findall(
            r"- core_id: (\d+)\s+eu_id: (\d+)\s+eu_index: (\d+)", output
        )
    ]
    if not rows or len({eu for eu, _, _ in rows}) != len(rows):
        raise ValueError("Could not read a unique EU map")
    rows.sort(key=lambda row: (row[1], row[2]))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(("eu_id", "core_id", "slot"))
        writer.writerows(rows)
    print(f"Saved {len(rows)} EUs across {len({core for _, core, _ in rows})} cores to {args.output}")


if __name__ == "__main__":
    main()
