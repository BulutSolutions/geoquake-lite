#!/usr/bin/env python3
"""
PNG to QOI Converter for GeoQuake
Generates QOI (Quite OK Image) files for ultra-fast decoding.
"""

import os
import sys
import struct
import argparse
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow is required. Install with: pip install Pillow")
    sys.exit(1)

# QOI Constants
QOI_OP_INDEX = 0x00
QOI_OP_DIFF  = 0x40
QOI_OP_LUMA  = 0x80
QOI_OP_RUN   = 0xc0
QOI_OP_RGB   = 0xfe
QOI_OP_RGBA  = 0xff
QOI_MASK_2   = 0xc0

def encode_qoi(img):
    width, height = img.size
    pixels = img.load()
    
    # QOI Header
    # magic(4), width(4), height(4), channels(1), colorspace(1)
    data = bytearray(b'qoif')
    data.extend(struct.pack('>IIBB', width, height, 3, 0)) # 3 channels (RGB), 0 (sRGB)

    prev_r = 0
    prev_g = 0
    prev_b = 0
    prev_a = 255
    
    run = 0
    index = [ (0,0,0,0) ] * 64
    
    total_pixels = width * height
    
    for y in range(height):
        for x in range(width):
            pixel = pixels[x, y]
            r, g, b = pixel[0], pixel[1], pixel[2]
            
            if (r, g, b, 255) == (prev_r, prev_g, prev_b, prev_a):
                run += 1
                if run == 62 or (x == width -1 and y == height -1):
                    data.append(QOI_OP_RUN | (run - 1))
                    run = 0
                continue
            
            if run > 0:
                data.append(QOI_OP_RUN | (run - 1))
                run = 0
                
            index_pos = (r * 3 + g * 5 + b * 7 + 255 * 11) % 64
            
            if index[index_pos] == (r, g, b, 255):
                data.append(QOI_OP_INDEX | index_pos)
            else:
                index[index_pos] = (r, g, b, 255)
                
                dr = (r - prev_r) & 0xff
                dg = (g - prev_g) & 0xff
                db = (b - prev_b) & 0xff
                
                dr_signed = dr - 256 if dr > 127 else dr
                dg_signed = dg - 256 if dg > 127 else dg
                db_signed = db - 256 if db > 127 else db
                
                if -2 <= dr_signed <= 1 and -2 <= dg_signed <= 1 and -2 <= db_signed <= 1:
                    data.append(QOI_OP_DIFF | ((dr_signed + 2) << 4) | ((dg_signed + 2) << 2) | (db_signed + 2))
                else:
                    dr_dg = (dr_signed - dg_signed)
                    db_dg = (db_signed - dg_signed)
                    
                    if -32 <= dg_signed <= 31 and -8 <= dr_dg <= 7 and -8 <= db_dg <= 7:
                        data.append(QOI_OP_LUMA | (dg_signed + 32))
                        data.append(((dr_dg + 8) << 4) | (db_dg + 8))
                    else:
                        data.append(QOI_OP_RGB)
                        data.extend((r, g, b))
                        
            prev_r, prev_g, prev_b = r, g, b

    # End marker
    data.extend(b'\x00\x00\x00\x00\x00\x00\x00\x01')
    return data

def convert_tiles(input_dir, output_dir):
    input_path = Path(input_dir)
    output_path = Path(output_dir)
    
    if not input_path.exists():
        print(f"Error: Input directory not found: {input_path}")
        return

    output_path.mkdir(parents=True, exist_ok=True)
    png_files = list(input_path.rglob('*.png'))
    
    print(f"Converting {len(png_files)} tiles to QOI...")
    
    count = 0
    saved_bytes = 0
    
    for png_file in png_files:
        try:
            img = Image.open(png_file).convert('RGB')
            qoi_data = encode_qoi(img)
            
            rel_path = png_file.relative_to(input_path)
            out_file = output_path / rel_path.with_suffix('.qoi')
            out_file.parent.mkdir(parents=True, exist_ok=True)
            
            with open(out_file, 'wb') as f:
                f.write(qoi_data)
                
            count += 1
            if count % 100 == 0:
                print(f"Converted {count}/{len(png_files)}...", end='\r')
                
        except Exception as e:
            print(f"Failed to convert {png_file}: {e}")

    print(f"\nComplete! Converted {count} tiles.")

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--input', '-i', default='./tiles_png')
    parser.add_argument('--output', '-o', default='./tiles_qoi')
    args = parser.parse_args()
    
    convert_tiles(args.input, args.output)
