"""Screen shared simulation profiles against Table I and verify on fresh seeds."""

import argparse
import csv
import itertools
import random
import statistics
import subprocess
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

from table_i_layouts import Pair, Point, west_weighted


# Table I cycles in Ours, Batch order.
PAPER = {
    (9, 100): (92.6, 112.6), (9, 90): (104.4, 114.2),
    (9, 70): (108.4, 110.6), (9, 50): (109.6, 110.2),
    (15, 100): (173.6, 176.4), (15, 90): (175.4, 175.6),
    (15, 70): (169.8, 189.2), (15, 50): (169.2, 182.8),
    (25, 100): (208.6, 271.6), (25, 90): (203.8, 215.8),
    (25, 70): (175.0, 233.6), (25, 50): (187.8, 218.6),
}
WEST_COUNTS = {
    "west12": {9: (4, 3, 2), 15: (7, 4, 4), 25: (12, 7, 6)},
    "west9": {9: (3, 3, 3), 15: (5, 5, 5), 25: (9, 8, 8)},
}
SCREEN_SEEDS = (3101, 3102)
HELDOUT_SEEDS = (6201, 6202, 6203, 6204, 6205)
MODES = ("immediate", "static_batch")


def inlet_pairs(family: str, groups: int, edge: int) -> list[Pair]:
    if family == "default":
        return []  # Let the simulator provide its original inlet layout.
    pairs = west_weighted(WEST_COUNTS[family][groups], edge)
    if family == "west12":
        left = [i for i, (a, _) in enumerate(pairs) if a[0] == 0]
        first, second = left[0], left[round(8 * (len(left) - 1) / 11)]
    else:
        top = [i for i, (a, _) in enumerate(pairs) if a[1] == 0]
        bottom = [i for i, (a, _) in enumerate(pairs) if a[1] == edge]
        index = 5 * (len(bottom) - 1) // 7
        first, second = top[index], bottom[index]
    a, b = pairs[first]
    c, d = pairs[second]
    pairs[first] = (a, d) if a < d else (d, a)
    pairs[second] = (c, b) if c < b else (b, c)
    return pairs


@dataclass(frozen=True)
class Profile:
    family: str
    output: str
    waste: str
    mix: int
    detect: int

    @property
    def name(self) -> str:
        return f"{self.family}_o{self.output}_w{self.waste}_m{self.mix}_d{self.detect}"

    def ports(self, edge: int) -> tuple[Point, Point]:
        output_y = 0 if self.output == "rt" else edge
        waste = {"lt": (0, 0), "lb": (0, edge), "rr": (edge, edge - output_y)}
        return (edge, output_y), waste[self.waste]

    def fields(self, groups: int, pattern: str) -> tuple:
        edge = 29 if groups == 9 else 59
        output, waste = self.ports(edge)
        inlets = inlet_pairs(self.family, groups, edge)
        coordinates = [n for pair in inlets for point in pair for n in point]
        return (groups, *output, self.mix, self.detect, pattern, "waste", *waste, *coordinates)


REFERENCE = Profile("default", "rt", "lb", 3, 2)


def generate_cases(path: Path, profiles: list[Profile], seeds: tuple[int, ...]) -> dict:
    patterns = {}
    for groups, percent in PAPER:
        for seed in seeds:
            rng = random.Random(seed)
            patterns[groups, percent, seed] = "".join(
                "O" if rng.randrange(100) < percent else "W" for _ in range(groups)
            )
    cases = {}
    with path.open("w", encoding="utf-8") as stream:
        stream.write("# name groups output_x output_y mix detect branches waste x y [inlet x y ...]\n")
        for profile in profiles:
            for (groups, percent, seed), pattern in patterns.items():
                name = f"{profile.name}_g{groups}_p{percent}_r{seed}"
                fields = profile.fields(groups, pattern)
                stream.write(" ".join(map(str, (name, *fields))) + "\n")
                cases[name] = (profile, groups, percent, seed)
    return cases


def run_trials(exe: Path, directory: Path, stage: str, profiles: list[Profile],
               seeds: tuple[int, ...]) -> dict:
    inputs = directory / f"{stage}_cases.txt"
    csv_path = directory / f"{stage}_runs.csv"
    cases = generate_cases(inputs, profiles, seeds)
    csv_path.unlink(missing_ok=True)
    result = subprocess.run([str(exe), "--calibrate", str(inputs), str(csv_path), "1"])
    if result.returncode not in (0, 1):
        result.check_returncode()
    trials = {}
    with csv_path.open(newline="", encoding="utf-8-sig") as stream:
        for row in csv.DictReader(stream):
            profile, groups, percent, seed = cases[row["profile"]]
            mode = row["mode"]
            key = (profile, groups, percent, seed, mode)
            status = row["status"]
            if key in trials or mode not in MODES or status not in ("complete", "timeout", "failed"):
                raise ValueError(f"Invalid trial row: {row}")
            edge = 29 if groups == 9 else 59
            output, waste = profile.ports(edge)
            columns = ("groups", "chip_side", "mix_cycles", "detect_cycles",
                       "output_x", "output_y", "waste_x", "waste_y", "repeat")
            expected = (groups, edge + 1, profile.mix, profile.detect, *output, *waste, 1)
            if tuple(int(row[column]) for column in columns) != expected:
                raise ValueError(f"Trial settings do not match: {row}")
            if status == "complete" and (
                int(row["route_requests"]) != groups * 4
                or int(row["route_arrivals"]) != groups * 4
                or int(row["steps"]) < 1
            ):
                raise ValueError(f"Invalid complete trial: {row}")
            trials[key] = status, int(row["steps"]) if status == "complete" else None
    if len(trials) != len(cases) * len(MODES):
        raise ValueError(f"{stage} output is missing trials")
    return trials


def summarize_profile(profile: Profile, trials: dict, seeds: tuple[int, ...]) -> tuple:
    counts = Counter()
    conditions = []
    errors, gap_errors, normalized_gap_errors = [], [], []
    for (groups, percent), (paper_dynamic, paper_batch) in PAPER.items():
        values = [[trials[profile, groups, percent, seed, mode] for seed in seeds]
                  for mode in MODES]
        completed = [[steps for status, steps in mode if status == "complete"] for mode in values]
        counts.update(status for mode in values for status, _ in mode)
        dynamic, batch = (statistics.mean(steps) if len(steps) == len(seeds) else None
                          for steps in completed)
        paper_gap = paper_batch - paper_dynamic
        gap = None
        if dynamic is not None and batch is not None:
            gap = batch - dynamic
            errors.extend((abs(dynamic - paper_dynamic) / paper_dynamic,
                           abs(batch - paper_batch) / paper_batch))
            gap_errors.append(abs(gap - paper_gap))
            normalized_gap_errors.append(abs(gap - paper_gap) / paper_batch)
        conditions.append((profile.name, groups, percent, paper_dynamic, paper_batch, paper_gap,
                           dynamic, batch, gap, *(len(steps) for steps in completed)))
    metrics = (None,) * 4
    if len(gap_errors) == len(PAPER):
        mean_error = statistics.mean(errors)
        score = mean_error + statistics.mean(normalized_gap_errors)
        metrics = (score, mean_error, max(errors), statistics.mean(gap_errors))
    return (*metrics, counts["complete"], counts["timeout"], counts["failed"]), conditions


def write_csv(path: Path, header: tuple[str, ...], rows) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(header)
        writer.writerows(tuple(round(value, 6) if isinstance(value, float) else value
                               for value in row) for row in rows)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path, help="Release build of dmf_paper_parallel_experiments")
    parser.add_argument("directory", type=Path, help="Output directory for calibration records")
    args = parser.parse_args()
    exe, directory = args.exe.resolve(strict=True), args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    profiles = [Profile(family, output, waste, mix, detect)
                for family, output, waste, (mix, detect) in itertools.product(
                    ("default", *WEST_COUNTS), ("rt", "rb"), ("lt", "lb", "rr"),
                    ((3, 2), (10, 18), (29, 8)))]

    trials = run_trials(exe, directory, "screen", profiles, SCREEN_SEEDS)
    ranked = [(profile, summarize_profile(profile, trials, SCREEN_SEEDS)[0])
              for profile in profiles]
    ranked.sort(key=lambda item: (float("inf") if item[1][0] is None else item[1][0],
                                  item[0].name))
    score_columns = ("score", "mean_relative_step_error", "worst_relative_step_error",
                     "mean_abs_gap_error", "complete", "timeout", "failed")
    write_csv(directory / "screen_ranking.csv", ("profile", *score_columns),
              ((profile.name, *metrics) for profile, metrics in ranked))
    winner, metrics = ranked[0]
    if metrics[0] is None:
        raise SystemExit("No screening profile completed every condition in both modes.")

    print(f"Screen winner: {winner.name}, score={metrics[0]:.6f}", flush=True)
    heldout_profiles = [winner] if winner == REFERENCE else [winner, REFERENCE]
    heldout = run_trials(exe, directory, "heldout", heldout_profiles, HELDOUT_SEEDS)
    summaries, conditions = [], []
    for profile in heldout_profiles:
        metrics, rows = summarize_profile(profile, heldout, HELDOUT_SEEDS)
        summaries.append((profile.name, *metrics))
        conditions.extend(rows)
    write_csv(directory / "heldout_summary.csv", ("profile", *score_columns), summaries)
    write_csv(directory / "heldout_conditions.csv",
              ("profile", "groups", "output_percent", "paper_ours", "paper_batch", "paper_gap",
               "immediate", "static_batch", "sim_gap", "immediate_completed", "batch_completed"),
              conditions)


if __name__ == "__main__":
    main()
