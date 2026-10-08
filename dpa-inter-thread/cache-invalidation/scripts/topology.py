#!/usr/bin/env python3
"""capture the management tool's explicit EU map without changing the device"""
import argparse
import csv
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TOOL = "/opt/mellanox/doca/tools/dpaeumgmt"


def capture(output, device="mlx5_0"):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    command = [TOOL, "info", "chip", "--dpa_device", device]
    text = subprocess.check_output(command, text=True, timeout=30)
    (output / "chip.yaml").write_text(text)
    matches = re.findall(r"- core_id: (\d+)\s+eu_id: (\d+)\s+eu_index: (\d+)", text)
    rows = [dict(eu_id=int(eu), core_id=int(core), slot=int(slot)) for core, eu, slot in matches]
    if not rows or len({r["eu_id"] for r in rows}) != len(rows):
        raise ValueError("Cannot extract a unique EU map from dpaeumgmt output")
    rows.sort(key=lambda r: (r["core_id"], r["slot"]))
    with (output / "topology.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, ["eu_id", "core_id", "slot"], lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    commands = {
        "firmware": ["ibv_devinfo", "-d", device],
        "sdk": ["pkg-config", "--modversion", "doca-common", "doca-dpa", "libflexio"],
        "device_compiler": ["/opt/mellanox/doca/tools/dpa-clang", "--version"],
        "host_compiler": ["cc", "--version"],
        "partitions": [TOOL, "partition", "query", "--dpa_device", device],
        "status": [TOOL, "info", "status", "--dpa_device", device],
    }
    metadata = {"device": device, "topology_command": command, "eus": len(rows),
                "cores": sorted({r["core_id"] for r in rows}), "line_bytes_assumed": 64,
                "slot_bytes": 256, "commands": {}}
    for name, args in commands.items():
        p = subprocess.run(args, text=True, capture_output=True, timeout=30)
        metadata["commands"][name] = dict(argv=args, returncode=p.returncode,
                                          stdout=p.stdout, stderr=p.stderr)
    compile_commands = ROOT / "build/compile_commands.json"
    if compile_commands.exists():
        metadata["host_compile_commands"] = json.loads(compile_commands.read_text())
    metadata["device_build_commands"] = [
        line.strip() for path in (ROOT / "build/CMakeFiles").glob("*_device.dir/build.make")
        for line in path.read_text().splitlines() if "/dpacc " in line]
    (output / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    return rows


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--device-name", default="mlx5_0")
    p.add_argument("--output-dir", type=Path, default=ROOT / "results/preflight")
    args = p.parse_args()
    rows = capture(args.output_dir, args.device_name)
    print(f"Saved {len(rows)} EUs to {args.output_dir / 'topology.csv'}")
