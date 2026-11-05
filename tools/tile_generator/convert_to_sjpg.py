#!/usr/bin/env python3
"""
PNG to SJPG Converter for GeoQuake

Converts PNG tiles to SJPG (Split JPEG) format for fast LVGL rendering.
SJPG format splits JPEG into small chunks for progressive decoding on ESP32.

Usage:
    python convert_to_sjpg.py [--input INPUT_DIR] [--output OUTPUT_DIR] [--quality QUALITY]

Requirements:
    pip install Pillow
"""

import os
import sys
import struct
import argparse
from pathlib import Path
from io import BytesIO

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow is required. Install with: pip install Pillow")
    sys.exit(1)

# SJPG format constants
SJPG_MAGIC = b'_SJPG__'  # 7 bytes magic header
SPLIT_HEIGHT = 16         # Height of each JPEG strip


def rgb_to_rgb565(r, g, b):
    """Convert RGB to RGB565 format (used by LVGL)"""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def create_sjpg(png_path, output_path, quality=85):
    """
    Convert a PNG image to SJPG format.

    SJPG Format:
    - 7 bytes: Magic header "_SJPG__"
    - 6 bytes: Reserved
    - 2 bytes: Image width (little endian)
    - 2 bytes: Image height (little endian)
    - 2 bytes: Number of JPEG strips
    - 4 bytes * num_strips: Size of each JPEG strip (little endian)
    - JPEG data for each strip
    """
    try:
        img = Image.open(png_path)
        img = img.convert('RGB')
        width, height = img.size

        # Calculate number of strips
        num_strips = (height + SPLIT_HEIGHT - 1) // SPLIT_HEIGHT

        # Create JPEG strips
        strips = []
        for i in range(num_strips):
            y_start = i * SPLIT_HEIGHT
            y_end = min(y_start + SPLIT_HEIGHT, height)
            strip_height = y_end - y_start

            # Crop strip from image
            strip_img = img.crop((0, y_start, width, y_end))

            # Encode as JPEG
            buffer = BytesIO()
            strip_img.save(buffer, format='JPEG', quality=quality, optimize=True)
            strips.append(buffer.getvalue())

        # Build SJPG file
        output_path.parent.mkdir(parents=True, exist_ok=True)

        with open(output_path, 'wb') as f:
            # --- Header (22 bytes) ---
            # 0-7: Signature "_SJPG__\x00"
            f.write(b'_SJPG__\x00')
            
            # 8-13: Version "V1.00\x00"
            f.write(b'V1.00\x00')
            
            # 14-15: Width (LE uint16)
            f.write(struct.pack('<H', width))
            
            # 16-17: Height (LE uint16)
            f.write(struct.pack('<H', height))
            
            # 18-19: Total Frames (LE uint16)
            f.write(struct.pack('<H', num_strips))
            
            # 20-21: Block Width / Single Frame Height (LE uint16)
            f.write(struct.pack('<H', SPLIT_HEIGHT))

            # --- Frame Info Array (Offset Table) ---
            # Contains size of each frame (uint16 LE)
            # LVGL reads this to calculate offsets: offset[i] = offset[i-1] + size[i-1]
            for strip in strips:
                f.write(struct.pack('<H', len(strip)))

            # --- JPEG Data ---
            # Contiguous data of all strips
            for strip in strips:
                f.write(strip)

        return True

    except Exception as e:
        print(f"  Error converting {png_path}: {e}")
        return False


def convert_tiles(input_dir, output_dir, quality=85):
    """Convert all PNG tiles to SJPG format"""

    input_path = Path(input_dir)
    output_path = Path(output_dir)

    if not input_path.exists():
        print(f"Error: Input directory not found: {input_path}")
        sys.exit(1)

    output_path.mkdir(parents=True, exist_ok=True)

    # Find all PNG tiles
    png_files = list(input_path.rglob('*.png'))
    if not png_files:
        print(f"No PNG files found in {input_path}")
        sys.exit(1)

    print(f"SJPG Converter for GeoQuake Lite")
    print(f"============================")
    print(f"Input: {input_path}")
    print(f"Output: {output_path}")
    print(f"Quality: {quality}")
    print(f"Files: {len(png_files)}")
    print()

    converted = 0
    failed = 0
    total_input_size = 0
    total_output_size = 0

    for i, png_file in enumerate(png_files):
        # Preserve directory structure
        rel_path = png_file.relative_to(input_path)
        sjpg_path = output_path / rel_path.with_suffix('.sjpg')

        if create_sjpg(png_file, sjpg_path, quality):
            converted += 1
            total_input_size += png_file.stat().st_size
            total_output_size += sjpg_path.stat().st_size
        else:
            failed += 1

        # Progress update
        if (i + 1) % 50 == 0 or i == len(png_files) - 1:
            print(f"  Progress: {i + 1}/{len(png_files)}", end='\r')

    print()
    print()
    print(f"Conversion Complete!")
    print(f"  Converted: {converted}")
    print(f"  Failed: {failed}")
    print(f"  Input Size: {total_input_size / 1024 / 1024:.1f} MB")
    print(f"  Output Size: {total_output_size / 1024 / 1024:.1f} MB")
    print(f"  Compression: {(1 - total_output_size/total_input_size) * 100:.1f}%")
    print()
    print(f"Next step: Copy {output_path} to SD card as /GeoQuakeLite/tiles/")

    return converted, failed


def main():
    parser = argparse.ArgumentParser(description='Convert PNG tiles to SJPG format')
    parser.add_argument('--input', '-i', default='./tiles_png',
                        help='Input directory with PNG tiles')
    parser.add_argument('--output', '-o', default='./tiles_sjpg',
                        help='Output directory for SJPG tiles')
    parser.add_argument('--quality', '-q', type=int, default=85,
                        help='JPEG quality (1-100, default: 85)')

    args = parser.parse_args()

    if args.quality < 1 or args.quality > 100:
        print("Error: quality must be between 1 and 100")
        sys.exit(1)

    convert_tiles(args.input, args.output, args.quality)


if __name__ == '__main__':
    main()
