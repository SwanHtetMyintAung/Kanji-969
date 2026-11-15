import argparse
import re
from pathlib import Path


def to_c_identifier(name: str) -> str:
    name = re.sub(r"\W+", "_", name)

    if not name or name[0].isdigit():
        name = "image_" + name

    return name


def convert_image(input_path: Path, output_path: Path):
    data = input_path.read_bytes()

    name = to_c_identifier(input_path.stem)
    upper_name = name.upper()

    lines = []

    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        values = ", ".join(f"0x{byte:02X}" for byte in chunk)
        lines.append(f"    {values},")

    byte_array = "\n".join(lines)

    header = f"""#pragma once

#include <stdint.h>

#define {upper_name}_SIZE {len(data)}

static const uint8_t {name}_data[] = {{
{byte_array}
}};
"""

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(header, encoding="utf-8")

    print(f"Generated: {output_path}")
    print(f"Original size: {len(data):,} bytes")


def main():
    parser = argparse.ArgumentParser(
        description="Embed an image file into a C byte array."
    )

    parser.add_argument("input", help="Input image path")
    parser.add_argument("-o", "--output", help="Output C header path")

    args = parser.parse_args()

    input_path = Path(args.input)

    if not input_path.exists():
        parser.error(f"Input file not found: {input_path}")

    output_path = (
        Path(args.output)
        if args.output
        else input_path.with_suffix(".h")
    )

    convert_image(input_path, output_path)


if __name__ == "__main__":
    main()

