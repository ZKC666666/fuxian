"""Choose layouts by paired step gap, then generate shared timing variants."""

import csv
import sys
from pathlib import Path


def timing_cases(layout_path: Path, result_path: Path) -> list[str]:
    source = {line.split()[0]: line.split()
              for line in layout_path.read_text(encoding="utf-8").splitlines()
              if line.strip() and not line.lstrip().startswith("#")}
    pairs = {}
    with result_path.open(newline="", encoding="utf-8-sig") as results:
        for row in csv.DictReader(results):
            if row["status"] == "complete":
                pairs.setdefault(row["profile"], {})[row["mode"]] = int(row["steps"])

    candidates = []
    for name, modes in pairs.items():
        if set(modes) == {"immediate", "static_batch"}:
            online = modes["immediate"]
            static = modes["static_batch"]
            offset = max(0, round(((208.6 - online) + (271.6 - static)) / 2))
            candidates.append((abs(static - online - 63), offset, name))

    cases = []
    for _, offset, name in sorted(candidates)[:5]:
        original = source[name]
        for shift in sorted({max(0, offset + delta) for delta in (-6, -3, 0, 3, 6)}):
            for extra_mix in sorted({0, shift // 2, shift}):
                mix, detect = 3 + extra_mix, 2 + shift - extra_mix
                fields = original.copy()
                fields[0] = f"{name}_m{mix}_d{detect}"
                fields[4:6] = [str(mix), str(detect)]
                cases.append(" ".join(fields))
    return cases


if __name__ == "__main__":
    layouts, results, output = map(Path, sys.argv[1:])
    cases = timing_cases(layouts, results)
    output.write_text("# Shared timing variants for paired layouts\n" +
                      "\n".join(cases) + "\n", encoding="utf-8")
    print(f"{len(cases)} timing variants")
