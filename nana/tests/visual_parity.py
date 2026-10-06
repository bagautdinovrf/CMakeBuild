"""Compare real Nana captures with the matching FLTK render-preview matrix.

Run after one complete UI build and preview pass. No UI is mocked, no image is
resized, and no user settings are read. Pillow is required only for this audit.
Whole-image differences are reported for inspection; structural checks use the
actual reference pixels so font rasterization differences are not called layout
failures.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

SCALES = (100, 125, 150, 200)
THEMES = ("light", "dark")
VIEWS = ("panel", "build-and-run", "states", "journal", "settings")
PALETTE = {
    "light": {"surface": (255, 255, 255), "line": (223, 228, 235), "accent": (18, 101, 211)},
    "dark": {"surface": (32, 35, 41), "line": (60, 67, 78), "accent": (118, 173, 255)},
}


def stem(view: str, theme: str, scale: int) -> str:
    if view == "settings":
        return f"settings-{theme}-{scale}"
    suffix = "" if view == "panel" else f"-{view}"
    return f"panel-{theme}{suffix}-{scale}"


def locate(directory: Path, name: str, reference: bool = False) -> Path | None:
    # Native renderer outputs take precedence over optional converted previews,
    # which can survive from an older run with the same filename.
    for extension in ((".ppm", ".bmp", ".png") if reference else (".bmp", ".png", ".ppm")):
        path = directory / (name + extension)
        if path.is_file():
            return path
    return None


def rect(box: tuple[int, int, int, int], scale: int) -> tuple[int, int, int, int]:
    return tuple(int(value * scale / 100) for value in box)


def color_fraction(image: Image.Image, box: tuple[int, int, int, int], color: tuple[int, int, int]) -> float:
    pixels = list(image.crop(box).getdata())
    return sum(pixel == color for pixel in pixels) / max(1, len(pixels))


def color_bounds(image: Image.Image, box: tuple[int, int, int, int], color: tuple[int, int, int]) -> list[int] | None:
    cropped = image.crop(box)
    points = [(index % cropped.width, index // cropped.width)
              for index, pixel in enumerate(cropped.getdata()) if pixel == color]
    if not points:
        return None
    xs, ys = zip(*points)
    return [min(xs) + box[0], min(ys) + box[1], max(xs) + box[0] + 1, max(ys) + box[1] + 1]


def compare(reference_path: Path, candidate_path: Path, view: str, theme: str, scale: int) -> dict:
    with Image.open(reference_path) as loaded:
        reference = loaded.convert("RGB")
    with Image.open(candidate_path) as loaded:
        candidate = loaded.convert("RGB")
    result = {"reference": str(reference_path), "nana": str(candidate_path),
              "size_reference": list(reference.size), "size_nana": list(candidate.size), "issues": []}
    if candidate.size != reference.size:
        result["issues"].append("Client bitmap dimensions differ; compare the same logical size and DPI.")
        return result

    difference = ImageChops.difference(reference, candidate)
    result["rms_rgb"] = round(math.sqrt(sum(value * value for value in ImageStat.Stat(difference).rms) / 3), 4)
    result["exact_pixel_fraction"] = round(sum(pixel == (0, 0, 0) for pixel in difference.getdata()) / (candidate.width * candidate.height), 6)
    palette = PALETTE[theme]
    # At 200% the real FLTK settings header divider covers rows 85-86 and
    # enters this band. Calibrate its Surface coverage against the matching
    # reference pixels instead of assuming that every band pixel is Surface.
    band = rect((2, 39, 495 if view != "settings" else 500, 43), scale)
    result["surface_reference_fraction"] = color_fraction(reference, band, palette["surface"])
    result["surface_fraction"] = color_fraction(candidate, band, palette["surface"])
    if result["surface_fraction"] != result["surface_reference_fraction"]:
        result["issues"].append("The background differs from the FLTK surface palette.")

    logo_box = rect((14, 14, 31, 30), scale)
    expected_logo = color_bounds(reference, logo_box, palette["accent"])
    actual_logo = color_bounds(candidate, logo_box, palette["accent"])
    result["logo_reference_bounds"], result["logo_nana_bounds"] = expected_logo, actual_logo
    if expected_logo != actual_logo:
        result["issues"].append("The three-block logo differs in position, size or color.")

    if view != "settings":
        # Primary split-button silhouette excludes header and status glyphs.
        run_box = rect((320, 57, 486, 95), scale)
        expected_run = color_bounds(reference, run_box, palette["accent"])
        actual_run = color_bounds(candidate, run_box, palette["accent"])
        result["primary_reference_bounds"], result["primary_nana_bounds"] = expected_run, actual_run
        if expected_run != actual_run:
            result["issues"].append("The primary Run split button differs in geometry, state or accent color.")
        if view != "states":
            mode_color = (196, 255, 210) if view == "build-and-run" else (188, 197, 171)
            build_box = rect((75, 57, 350, 95), scale)
            expected_build = color_bounds(reference, build_box, mode_color)
            actual_build = color_bounds(candidate, build_box, mode_color)
            result["build_reference_bounds"], result["build_nana_bounds"] = expected_build, actual_build
            result["build_mode_color"] = list(mode_color)
            if expected_build is None or actual_build != expected_build:
                result["issues"].append("The Build split button differs in requested mode fill or geometry.")
            if actual_build is not None:
                dark_text = color_bounds(candidate, tuple(actual_build), (36, 41, 51))
                result["build_dark_foreground_bounds"] = dark_text
                if dark_text is None:
                    result["issues"].append("The Build split button has no readable dark foreground in its mode fill.")
    divider_y = int((43 if view == "settings" else 113) * scale / 100)
    rows = range(max(0, divider_y - 2), min(candidate.height, divider_y + 3))
    expected_rows = [y for y in rows if color_fraction(reference, (1, y, reference.width - 1, y + 1), palette["line"]) >= 0.95]
    actual_rows = [y for y in rows if color_fraction(candidate, (1, y, candidate.width - 1, y + 1), palette["line"]) >= 0.95]
    result["divider_reference_rows"], result["divider_nana_rows"] = expected_rows, actual_rows
    if not expected_rows or actual_rows != expected_rows:
        result["issues"].append("The divider differs in exact physical row position, thickness or color.")
    return result


def main() -> int:
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, default=repository / "build/engine-tests/ui-preview")
    parser.add_argument("--nana", type=Path, default=repository / "nana/build/engine-tests/tests/ui-preview")
    parser.add_argument("--report", type=Path, help="Optional JSON report; otherwise print the complete report.")
    parser.add_argument("--manifest", action="store_true", help="Print the 40 comparable snapshot stems and exit.")
    args = parser.parse_args()
    names = [(view, theme, scale, stem(view, theme, scale))
             for view in VIEWS for theme in THEMES for scale in SCALES]
    if args.manifest:
        print("\n".join(name for _, _, _, name in names))
        return 0
    report = []
    ordinary_modes = {}
    for view, theme, scale, name in names:
        reference, candidate = locate(args.reference, name, reference=True), locate(args.nana, name)
        if reference is None or candidate is None:
            report.append({"snapshot": name, "issues": ["Missing actual FLTK/Nana snapshot."],
                           "reference": str(reference) if reference else None, "nana": str(candidate) if candidate else None})
        else:
            item = {"snapshot": name, **compare(reference, candidate, view, theme, scale)}
            if view == "panel":
                ordinary_modes[theme, scale] = item
            elif view == "build-and-run" and (theme, scale) in ordinary_modes:
                ordinary = ordinary_modes[theme, scale]
                for source in ("reference", "nana"):
                    field = f"build_{source}_bounds"
                    if item.get(field) != ordinary.get(field):
                        item["issues"].append(f"The {source} Build split-button geometry changed between saved modes.")
            report.append(item)
    output = json.dumps(report, ensure_ascii=False, indent=2)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(output + "\n", encoding="utf-8")
    else:
        print(output)
    failures = sum(bool(item["issues"]) for item in report)
    print(f"Actual FLTK/Nana pairs: {len(report)}; structural mismatches or missing captures: {failures}.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
