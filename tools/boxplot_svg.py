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


def stats(values: list[float]) -> dict[str, float]:
    ordered = sorted(values)
    q1 = percentile(ordered, 0.25)
    q2 = percentile(ordered, 0.50)
    q3 = percentile(ordered, 0.75)
    iqr = q3 - q1
    low_fence = q1 - 1.5 * iqr
    high_fence = q3 + 1.5 * iqr
    whisker_low = next((v for v in ordered if v >= low_fence), ordered[0])
    whisker_high = next((v for v in reversed(ordered) if v <= high_fence), ordered[-1])
    return {
        "min": ordered[0],
        "q1": q1,
        "median": q2,
        "q3": q3,
        "max": ordered[-1],
        "whisker_low": whisker_low,
        "whisker_high": whisker_high,
        "mean": statistics.fmean(ordered),
    }


def map_y(value: float, min_value: float, max_value: float, top: float, bottom: float) -> float:
    if math.isclose(max_value, min_value):
        return (top + bottom) / 2.0
    scale = (value - min_value) / (max_value - min_value)
    return bottom - scale * (bottom - top)


def render_svg(values: list[float], title: str, ylabel: str) -> str:
    width = 720
    height = 480
    left = 110
    right = width - 80
    top = 70
    bottom = height - 80
    center = (left + right) / 2.0
    box_half = 70

    s = stats(values)
    min_value = s["min"]
    max_value = s["max"]

    lines: list[str] = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#f6f4ef"/>',
        f'<text x="{width / 2:.1f}" y="36" text-anchor="middle" font-size="24" fill="#1d2a33" font-family="Helvetica, Arial, sans-serif">{title}</text>',
        f'<text x="24" y="{height / 2:.1f}" transform="rotate(-90 24 {height / 2:.1f})" text-anchor="middle" font-size="16" fill="#3c4c57" font-family="Helvetica, Arial, sans-serif">{ylabel}</text>',
        f'<line x1="{left}" y1="{bottom}" x2="{right}" y2="{bottom}" stroke="#5d6b75" stroke-width="2"/>',
        f'<line x1="{left}" y1="{top}" x2="{left}" y2="{bottom}" stroke="#5d6b75" stroke-width="2"/>',
    ]

    for tick in range(5):
        ratio = tick / 4.0
        value = min_value + (max_value - min_value) * ratio
        y = map_y(value, min_value, max_value, top, bottom)
        lines.append(
            f'<line x1="{left - 8}" y1="{y:.2f}" x2="{right}" y2="{y:.2f}" stroke="#d8d3c7" stroke-width="1"/>'
        )
        lines.append(
            f'<text x="{left - 14}" y="{y + 5:.2f}" text-anchor="end" font-size="13" fill="#56646f" font-family="Helvetica, Arial, sans-serif">{value:.3f}</text>'
        )

    y_q1 = map_y(s["q1"], min_value, max_value, top, bottom)
    y_q2 = map_y(s["median"], min_value, max_value, top, bottom)
    y_q3 = map_y(s["q3"], min_value, max_value, top, bottom)
    y_low = map_y(s["whisker_low"], min_value, max_value, top, bottom)
    y_high = map_y(s["whisker_high"], min_value, max_value, top, bottom)
    y_mean = map_y(s["mean"], min_value, max_value, top, bottom)

    lines.extend(
        [
            f'<line x1="{center:.2f}" y1="{y_high:.2f}" x2="{center:.2f}" y2="{y_q3:.2f}" stroke="#8b1e3f" stroke-width="3"/>',
            f'<line x1="{center:.2f}" y1="{y_q1:.2f}" x2="{center:.2f}" y2="{y_low:.2f}" stroke="#8b1e3f" stroke-width="3"/>',
            f'<line x1="{center - 24:.2f}" y1="{y_high:.2f}" x2="{center + 24:.2f}" y2="{y_high:.2f}" stroke="#8b1e3f" stroke-width="3"/>',
            f'<line x1="{center - 24:.2f}" y1="{y_low:.2f}" x2="{center + 24:.2f}" y2="{y_low:.2f}" stroke="#8b1e3f" stroke-width="3"/>',
            f'<rect x="{center - box_half:.2f}" y="{y_q3:.2f}" width="{box_half * 2:.2f}" height="{y_q1 - y_q3:.2f}" fill="#d2b48c" stroke="#8b1e3f" stroke-width="3"/>',
            f'<line x1="{center - box_half:.2f}" y1="{y_q2:.2f}" x2="{center + box_half:.2f}" y2="{y_q2:.2f}" stroke="#8b1e3f" stroke-width="4"/>',
            f'<circle cx="{center:.2f}" cy="{y_mean:.2f}" r="6" fill="#1d2a33"/>',
            f'<text x="{center:.2f}" y="{bottom + 36}" text-anchor="middle" font-size="15" fill="#3c4c57" font-family="Helvetica, Arial, sans-serif">n={len(values)}</text>',
        ]
    )

    labels = [
        ("min", s["min"], y_low),
        ("q1", s["q1"], y_q1),
        ("median", s["median"], y_q2),
        ("q3", s["q3"], y_q3),
        ("max", s["max"], y_high),
        ("mean", s["mean"], y_mean),
    ]
    label_x = right - 10
    for name, value, y in labels:
        lines.append(
            f'<text x="{label_x}" y="{y + 5:.2f}" text-anchor="start" font-size="13" fill="#1d2a33" font-family="Helvetica, Arial, sans-serif">{name}: {value:.3f}</text>'
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
    values = [float(line.strip()) for line in input_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    output_path.write_text(render_svg(values, args.title, args.ylabel), encoding="utf-8")


if __name__ == "__main__":
    main()
