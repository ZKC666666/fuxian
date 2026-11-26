"""Generate the 25-group calibration layouts with fixed 3/2 durations."""

import dataclasses
import sys
from pathlib import Path


Point = tuple[int, int]
Pair = tuple[Point, Point]
EDGE = 59
WASTE = (0, EDGE)
ALL_OUTPUT = "O" * 25


def boundary_pair(edge: int, center: int, extent: int = EDGE) -> Pair:
    return (
        ((0, center - 1), (0, center + 1)),
        ((extent, center - 1), (extent, center + 1)),
        ((center - 1, 0), (center + 1, 0)),
        ((center - 1, extent), (center + 1, extent)),
    )[edge]


def balanced() -> list[Pair]:
    pairs = []
    for group in range(25):
        edge = group % 4
        count = (28 - edge) // 4
        slot = (2 * (group // 4) + 1) * 9 // (2 * count)
        pairs.append(boundary_pair(edge, 4 + 6 * slot))
    return pairs


def west_weighted(counts: tuple[int, int, int], extent: int = EDGE) -> list[Pair]:
    slots = (extent - 7) // 4 + 1
    sides = []
    for edge, count in zip((0, 2, 3), counts):
        centers = [4 + 4 * ((2 * i + 1) * slots // (2 * count)) for i in range(count)]
        sides.append([boundary_pair(edge, center, extent) for center in centers])
    return [side[i] for i in range(max(counts)) for side in sides if i < len(side)]


@dataclasses.dataclass(frozen=True)
class Layout:
    name: str
    pairs: list[Pair]
    outlet_y: int

    def legal(self) -> bool:
        ports = [(EDGE, self.outlet_y), WASTE, *(p for pair in self.pairs for p in pair)]
        return all(max(abs(x - u), abs(y - v)) >= 2
                   for i, (x, y) in enumerate(ports) for u, v in ports[:i])

    def line(self) -> str:
        coordinates = " ".join(str(value) for pair in self.pairs for p in pair for value in p)
        return f"{self.name} 25 {EDGE} {self.outlet_y} 3 2 {ALL_OUTPUT} {coordinates}"


def layouts() -> list[Layout]:
    result = []
    families = {
        "balanced": balanced(),
        "west12": west_weighted((12, 7, 6)),
        "west9": west_weighted((9, 8, 8)),
    }
    distant_pairs = (
        ((0, 57), (59, 2)),
        ((0, 2), (59, 57)),
        ((2, 59), (57, 0)),
        ((2, 0), (57, 59)),
        ((0, 46), (59, 2)),
        ((0, 13), (59, 57)),
    )
    for label, original in families.items():
        for outlet in (0, 29, 59):
            result.append(Layout(f"{label}_out{outlet}", original, outlet))
            for index in (0, 4, 12, 20, 24):
                for pair_id, replacement in enumerate(distant_pairs):
                    pairs = original.copy()
                    pairs[index] = replacement
                    result.append(Layout(f"{label}_out{outlet}_g{index + 1}_p{pair_id + 1}",
                                         pairs, outlet))
        for left in (0, 8, 16, 24):
            for right in (1, 9, 17, 21):
                pairs = original.copy()
                a, b = original[left]
                c, d = original[right]
                pairs[left] = tuple(sorted((a, d)))
                pairs[right] = tuple(sorted((c, b)))
                for outlet in (0, 59):
                    result.append(Layout(f"{label}_out{outlet}_swap{left + 1}_{right + 1}",
                                         pairs, outlet))
    return [layout for layout in result if layout.legal()]


if __name__ == "__main__":
    cases = layouts()
    Path(sys.argv[1]).write_text("# Fixed durations for the layout screen\n" +
                                 "\n".join(layout.line() for layout in cases) + "\n",
                                 encoding="utf-8")
    print(f"{len(cases)} admissible layouts")
