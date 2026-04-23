#!/usr/bin/env python3
"""Extract files from Arcanum DAT archives."""

import struct
import os
import sys
import zlib

FOURCC_DAT = 0x44415420   # ' TAD'
FOURCC_DAT1 = 0x44415431  # '1TAD'

def extract_dat(dat_path, output_dir=None):
    """Extract all files from a DAT archive."""
    if output_dir is None:
        output_dir = os.path.splitext(dat_path)[0] + "_extracted"

    os.makedirs(output_dir, exist_ok=True)

    with open(dat_path, 'rb') as f:
        # Get file size
        f.seek(0, 2)
        file_size = f.tell()

        # Read file ID and name_table_size from end
        f.seek(-12, 2)
        file_id = struct.unpack('<I', f.read(4))[0]
        name_table_size = struct.unpack('<I', f.read(4))[0]

        # Check format and read entry_table_offset
        if file_id == FOURCC_DAT:
            guid = None
            f.seek(-12, 2)
            entry_table_offset = struct.unpack('<I', f.read(4))[0]
        elif file_id == FOURCC_DAT1:
            f.seek(-24, 2)
            guid = f.read(16)
            entry_table_offset = struct.unpack('<I', f.read(4))[0]
        else:
            print(f"Unknown file format: {file_id:08x}")
            return

        print(f"File size: {file_size}")
        print(f"Name table size: {name_table_size}")
        print(f"Entry table offset: {entry_table_offset}")

        # Seek to entry_table_size location
        f.seek(-4 - entry_table_offset, 2)
        entry_table_size = struct.unpack('<I', f.read(4))[0]

        print(f"Entry table size: {entry_table_size}")

        # Calculate offset adjustment
        offset_adjustment = file_size - entry_table_size - entry_table_offset

        print(f"Offset adjustment: {offset_adjustment}")

        # Read entries count
        entries_count = struct.unpack('<I', f.read(4))[0]

        print(f"Entries count: {entries_count}")

        entries = []

        for i in range(entries_count):
            name_len = struct.unpack('<I', f.read(4))[0]

            if name_len > 1024:  # Sanity check
                print(f"Entry {i}: name_len too large: {name_len}, stopping")
                break

            name = f.read(name_len).decode('utf-8', errors='replace')
            f.read(4)  # padding

            flags = struct.unpack('<I', f.read(4))[0]
            size = struct.unpack('<I', f.read(4))[0]
            compressed_size = struct.unpack('<I', f.read(4))[0]
            entry_offset = struct.unpack('<I', f.read(4))[0]

            entry_offset += offset_adjustment

            entries.append({
                'name': name.lower(),
                'flags': flags,
                'size': size,
                'compressed_size': compressed_size,
                'offset': entry_offset
            })

            if i < 5 or 'mainmenu' in name.lower():
                print(f"Entry {i}: {name} (offset={entry_offset}, size={size}, flags={flags:08x})")

        # Extract mes files
        mes_entries = [e for e in entries if e['name'].endswith('.mes')]
        print(f"\nFound {len(mes_entries)} .mes files")

        for entry in mes_entries:
            if 'mainmenu' in entry['name']:
                print(f"\nExtracting: {entry['name']}")
                print(f"  Offset: {entry['offset']}, Size: {entry['size']}, Compressed: {entry['compressed_size']}")

                f.seek(entry['offset'])

                if entry['flags'] & 0x02:  # Compressed
                    compressed_data = f.read(entry['compressed_size'])
                    try:
                        data = zlib.decompress(compressed_data)
                        print(f"  Decompressed {entry['compressed_size']} -> {len(data)} bytes")
                    except Exception as e:
                        print(f"  Failed to decompress: {e}")
                        continue
                else:
                    data = f.read(entry['size'])
                    print(f"  Read {len(data)} bytes (uncompressed)")

                # Create output directory
                out_file = os.path.join(output_dir, entry['name'])
                os.makedirs(os.path.dirname(out_file), exist_ok=True)

                with open(out_file, 'wb') as out:
                    out.write(data)
                print(f"  Wrote: {out_file}")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: extract_dat.py <dat_file> [output_dir]")
        sys.exit(1)

    dat_path = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else None

    extract_dat(dat_path, output_dir)
