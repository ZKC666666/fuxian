"""Compare the Table II experiment CSV with the paper's mean control steps."""

import argparse
import csv
import statistics
from collections import defaultdict
from pathlib import Path


PATHS = ("Short", "Medium-1", "Medium-2", "Long")
# Each pair is Baseline, Ours from the paper.
PAPER = {
    "N2_C1_M4": [(92.0, 98.0), (131.4, 134.2), (136.2, 103.4), (226.2, 167.0)],
    "N2_C2_M3": [(93.6, 92.0), (132.6, 101.2), (141.4, 109.8), (212.0, 197.8)],
    "N3_C1_M3": [(106.0, 96.2), (142.4, 113.6), (145.6, 119.4), (214.0, 173.0)],
    "N3_C1_M5": [(135.8, 129.8), (187.0, 143.2), (195.4, 154.0), (375.6, 281.0)],
    "N4_C2_M6": [(164.0, 164.0), (220.6, 175.0), (220.4, 169.6), (472.25, 355.67)],
    "N5_C2_M7": [(186.6, 178.4), (242.0, 219.4), (256.0, 208.6), (551.8, 470.33)],
}
MODES = ("batch_plan", "immediate")


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("trials", type=Path)
    cli.add_argument("comparison", type=Path)
    args = cli.parse_args()
    scenarios = defaultdict(dict)
    with args.trials.open(newline="", encoding="utf-8-sig") as stream:
        for row in csv.DictReader(stream):
            trial = (row["mode"], row["seed"])
            block = scenarios[row["config"], row["path"]]
            if trial in block or row["status"] not in ("complete", "timeout", "failed"):
                raise ValueError(f"Unexpected trial {row}")
            block[trial] = row
    full = {(name, path) for name in PAPER for path in PATHS}
    pilot = {("N2_C1_M4", path) for path in ("Short", "Long")}
    if scenarios.keys() != full and scenarios.keys() != pilot:
        raise ValueError("Expected a complete pilot or a full set of scenarios")

    with args.comparison.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("config", "path", "chip_side", "sites", "paper_batch", "paper_ours",
                         "sim_batch_plan", "sim_immediate", "paper_gain", "sim_gain",
                         "complete_batch", "complete_immediate"))
        for (config, path), trials in scenarios.items():
            seeds = [str(s) for s in range(6201, 6206)]
            if trials.keys() != {(mode, seed) for mode in MODES for seed in seeds}:
                raise ValueError(f"Unexpected or missing runs for {config} {path}")
            for seed in seeds:
                if trials[MODES[0], seed]["outcomes"] != trials[MODES[1], seed]["outcomes"]:
                    raise ValueError(f"Unpaired input for {config} {path} {seed}")
            complete = [[int(trials[mode, seed]["steps"]) for seed in seeds
                         if trials[mode, seed]["status"] == "complete"] for mode in MODES]
            batch, immediate = [statistics.mean(group) if len(group) == 5 else None for group in complete]
            sim_gain = None if batch is None or immediate is None else round(batch - immediate, 2)
            paper_batch, paper_ours = PAPER[config][PATHS.index(path)]
            first = trials[MODES[0], seeds[0]]
            writer.writerow((config, path, first["chip_side"], first["sites"], paper_batch,
                             paper_ours, batch, immediate, round(paper_batch - paper_ours, 2),
                             sim_gain, *map(len, complete)))


if __name__ == "__main__":
    main()
