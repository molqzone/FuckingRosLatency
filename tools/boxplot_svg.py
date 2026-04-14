#!/usr/bin/env python3
from __future__ import annotations

import argparse
import math
import statistics
from pathlib import Path


def percentile(sorted_values: list[float], p: float) -> float:
    if not sorted_values:
        raise ValueError("empty sample")
    if len(sorted_values) == 1:
        return sorted_values[0]
    pos = (len(sorted_values) - 1) * p
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return sorted_values[lo]
    weight = pos - lo
    return sorted_values[lo] * (1.0 - weight) + sorted_values[hi] * weight


def nice_step(raw_step: float) -> float:
    if raw_step <= 0:
        return 1.0
    exponent = math.floor(math.log10(raw_step))
    fraction = raw_step / (10**exponent)
    if fraction <= 1.0:
        nice_fraction = 1.0
    elif fraction <= 2.0:
        nice_fraction = 2.0
    elif fraction <= 5.0:
        nice_fraction = 5.0
    else:
        nice_fraction = 10.0
    return nice_fraction * (10**exponent)


def format_value(value: float) -> str:
    magnitude = abs(value)
    if magnitude >= 1000:
        return f"{value:.0f}"
    if magnitude >= 100:
        return f"{value:.1f}"
    if magnitude >= 10:
        return f"{value:.2f}"
    return f"{value:.3f}"


def wrapped_title(title: str, width: int = 34) -> list[str]:
    words = title.split()
    if not words:
        return [title]
    lines: list[str] = []
    current = words[0]
    for word in words[1:]:
        candidate = f"{current} {word}"
        if len(candidate) <= width:
            current = candidate
        else:
            lines.append(current)
            current = word
    lines.append(current)
    return lines[:3]


def stats(values: list[float]) -> dict[str, float | list[float]]:
    ordered = sorted(values)
    q1 = percentile(ordered, 0.25)
    q2 = percentile(ordered, 0.50)
    q3 = percentile(ordered, 0.75)
    iqr = q3 - q1
    low_fence = q1 - 1.5 * iqr
    high_fence = q3 + 1.5 * iqr
    whisker_low = next((v for v in ordered if v >= low_fence), ordered[0])
    whisker_high = next((v for v in reversed(ordered) if v <= high_fence), ordered[-1])
    lower_outliers = [v for v in ordered if v < whisker_low]
    upper_outliers = [v for v in ordered if v > whisker_high]
    return {
        "min": ordered[0],
        "q1": q1,
        "median": q2,
        "q3": q3,
        "max": ordered[-1],
        "whisker_low": whisker_low,
        "whisker_high": whisker_high,
        "mean": statistics.fmean(ordered),
        "lower_outliers": lower_outliers,
        "upper_outliers": upper_outliers,
        "iqr": iqr,
    }


def axis_bounds(s: dict[str, float | list[float]]) -> tuple[float, float]:
    whisker_low = float(s["whisker_low"])
    whisker_high = float(s["whisker_high"])
    vmin = float(s["min"])
    vmax = float(s["max"])
    iqr = float(s["iqr"])

    core_min = whisker_low
    core_max = whisker_high
    if math.isclose(core_min, core_max):
        pad = max(abs(core_min) * 0.1, 1.0)
        return core_min - pad, core_max + pad

    core_range = core_max - core_min
    raw_min = min(vmin, core_min - max(core_range * 0.08, iqr * 0.2))
    raw_max = max(vmax, core_max + max(core_range * 0.08, iqr * 0.2))

    # If a tiny number of extreme outliers would crush the box, keep the axis close to
    # the whisker range and render outlier markers against the plot edge instead.
    if vmax - core_max > core_range * 3.0:
        raw_max = core_max + core_range * 0.15
    if core_min - vmin > core_range * 3.0:
        raw_min = core_min - core_range * 0.15

    if math.isclose(raw_min, raw_max):
        pad = max(abs(raw_min) * 0.1, 1.0)
        return raw_min - pad, raw_max + pad
    return raw_min, raw_max


def map_y(value: float, min_value: float, max_value: float, top: float, bottom: float) -> float:
    if math.isclose(max_value, min_value):
        return (top + bottom) / 2.0
    scale = (value - min_value) / (max_value - min_value)
    return bottom - scale * (bottom - top)


def render_svg(values: list[float], title: str, ylabel: str) -> str:
    width = 920
    height = 560
    left = 110
    plot_right = 560
    info_left = 620
    top = 110
    bottom = height - 90
    center = (left + plot_right) / 2.0
    box_half = 80

    s = stats(values)
    axis_min, axis_max = axis_bounds(s)
    y_q1 = map_y(float(s["q1"]), axis_min, axis_max, top, bottom)
    y_q2 = map_y(float(s["median"]), axis_min, axis_max, top, bottom)
    y_q3 = map_y(float(s["q3"]), axis_min, axis_max, top, bottom)
    y_low = map_y(float(s["whisker_low"]), axis_min, axis_max, top, bottom)
    y_high = map_y(float(s["whisker_high"]), axis_min, axis_max, top, bottom)
    y_mean = map_y(float(s["mean"]), axis_min, axis_max, top, bottom)

    title_lines = wrapped_title(title)
    tick_count = 6
    step = nice_step((axis_max - axis_min) / (tick_count - 1))
    axis_start = math.floor(axis_min / step) * step
    axis_end = math.ceil(axis_max / step) * step

    lines: list[str] = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#f4f1ea"/>',
        f'<rect x="{info_left - 30}" y="{top - 20}" width="{width - info_left - 20}" height="{bottom - top + 40}" fill="#fbfaf7" stroke="#d7d0c2" stroke-width="1.5"/>',
    ]

    for idx, line in enumerate(title_lines):
        lines.append(
            f'<text x="{width / 2:.1f}" y="{42 + idx * 28}" text-anchor="middle" font-size="24" fill="#14212b" font-family="Segoe UI, Helvetica, Arial, sans-serif">{line}</text>'
        )

    lines.extend(
        [
            f'<text x="34" y="{height / 2:.1f}" transform="rotate(-90 34 {height / 2:.1f})" text-anchor="middle" font-size="16" fill="#3f4f5a" font-family="Segoe UI, Helvetica, Arial, sans-serif">{ylabel}</text>',
            f'<line x1="{left}" y1="{bottom}" x2="{plot_right}" y2="{bottom}" stroke="#596873" stroke-width="2"/>',
            f'<line x1="{left}" y1="{top}" x2="{left}" y2="{bottom}" stroke="#596873" stroke-width="2"/>',
        ]
    )

    tick = axis_start
    while tick <= axis_end + step * 0.5:
        y = map_y(tick, axis_min, axis_max, top, bottom)
        lines.append(
            f'<line x1="{left - 8}" y1="{y:.2f}" x2="{plot_right}" y2="{y:.2f}" stroke="#ddd7cb" stroke-width="1"/>'
        )
        lines.append(
            f'<text x="{left - 14}" y="{y + 5:.2f}" text-anchor="end" font-size="13" fill="#4e5d68" font-family="Segoe UI, Helvetica, Arial, sans-serif">{format_value(tick)}</text>'
        )
        tick += step

    lines.extend(
        [
            f'<line x1="{center:.2f}" y1="{y_high:.2f}" x2="{center:.2f}" y2="{y_q3:.2f}" stroke="#8f2743" stroke-width="3"/>',
            f'<line x1="{center:.2f}" y1="{y_q1:.2f}" x2="{center:.2f}" y2="{y_low:.2f}" stroke="#8f2743" stroke-width="3"/>',
            f'<line x1="{center - 28:.2f}" y1="{y_high:.2f}" x2="{center + 28:.2f}" y2="{y_high:.2f}" stroke="#8f2743" stroke-width="3"/>',
            f'<line x1="{center - 28:.2f}" y1="{y_low:.2f}" x2="{center + 28:.2f}" y2="{y_low:.2f}" stroke="#8f2743" stroke-width="3"/>',
            f'<rect x="{center - box_half:.2f}" y="{y_q3:.2f}" width="{box_half * 2:.2f}" height="{y_q1 - y_q3:.2f}" fill="#d7b891" stroke="#8f2743" stroke-width="3"/>',
            f'<line x1="{center - box_half:.2f}" y1="{y_q2:.2f}" x2="{center + box_half:.2f}" y2="{y_q2:.2f}" stroke="#8f2743" stroke-width="4"/>',
            f'<circle cx="{center:.2f}" cy="{y_mean:.2f}" r="6" fill="#16222c"/>',
            f'<text x="{center:.2f}" y="{bottom + 40}" text-anchor="middle" font-size="15" fill="#42515c" font-family="Segoe UI, Helvetica, Arial, sans-serif">distribution, n={len(values)}</text>',
        ]
    )

    lower_outliers = list(s["lower_outliers"])
    upper_outliers = list(s["upper_outliers"])
    outlier_x = center + box_half + 30
    for idx, value in enumerate(upper_outliers[:6]):
        y = max(top + 8, map_y(value, axis_min, axis_max, top, bottom))
        lines.append(f'<circle cx="{outlier_x + (idx % 3) * 12:.2f}" cy="{y:.2f}" r="3.5" fill="#4c6b81"/>')
    for idx, value in enumerate(lower_outliers[:6]):
        y = min(bottom - 8, map_y(value, axis_min, axis_max, top, bottom))
        lines.append(f'<circle cx="{outlier_x + (idx % 3) * 12:.2f}" cy="{y:.2f}" r="3.5" fill="#4c6b81"/>')

    stats_lines = [
        f"min: {format_value(float(s['min']))}",
        f"q1: {format_value(float(s['q1']))}",
        f"median: {format_value(float(s['median']))}",
        f"q3: {format_value(float(s['q3']))}",
        f"max: {format_value(float(s['max']))}",
        f"mean: {format_value(float(s['mean']))}",
        f"outliers+: {len(upper_outliers)}",
        f"outliers-: {len(lower_outliers)}",
    ]
    lines.append(
        f'<text x="{info_left}" y="{top + 10}" font-size="18" fill="#16222c" font-family="Segoe UI, Helvetica, Arial, sans-serif">Summary</text>'
    )
    for idx, line in enumerate(stats_lines):
        lines.append(
            f'<text x="{info_left}" y="{top + 42 + idx * 28}" font-size="15" fill="#44545f" font-family="Segoe UI, Helvetica, Arial, sans-serif">{line}</text>'
        )

    lines.append("</svg>")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--title", required=True)
    parser.add_argument("--ylabel", required=True)
    args = parser.parse_args()

    input_path = Path(args.input)
    output_path = Path(args.output)
    values = [
        float(line.strip())
        for line in input_path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    output_path.write_text(render_svg(values, args.title, args.ylabel), encoding="utf-8")


if __name__ == "__main__":
    main()
