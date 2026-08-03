#!/usr/bin/env python3

from __future__ import annotations

import argparse
import sys

import vareg


def main() -> int:
    if sys.argv[1:] == ["--check"]:
        return 0

    parser = argparse.ArgumentParser(
        description="Set VAREG Del_reg values from 4x64 median pedestals on stdin."
    )
    parser.add_argument("--in", dest="config_in", required=True)
    parser.add_argument("--out", dest="config_out", required=True)
    args = parser.parse_args()

    values = [float(value) for value in sys.stdin.read().split()]
    if len(values) != 4 * 64:
        raise ValueError(f"expected 256 pedestal values, got {len(values)}")

    register = vareg.Vareg4ASIC()
    register.load(args.config_in)
    asics = [register.asic0, register.asic1, register.asic2, register.asic3]
    for asic_index, asic in enumerate(asics):
        pedestals = values[asic_index * 64 : (asic_index + 1) * 64]
        highest = max(pedestals)
        for channel, pedestal in enumerate(pedestals):
            asic.Del_reg[channel] = max(0, min(63, round(highest - pedestal)))

    register.save(args.config_out)
    print(f"Pedestal register written to {args.config_out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"Error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
