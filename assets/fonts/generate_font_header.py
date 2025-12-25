#!/usr/bin/env python3
"""Generate C header file with embedded font binary data."""

import sys

def embed_font(font_path, output_path, var_name='inter_font_data'):
    """Convert font binary to C hex array."""
    with open(font_path, 'rb') as f:
        data = f.read()
    
    # Generate C code
    lines = [
        f'// Auto-generated font data - {len(data)} bytes',
        f'// Open font from google (https://fonts.google.com/specimen/Inter)',
        f'// Auto-generated font data - {len(data)} bytes',
        f'#ifndef INTER_FONT_DATA_H',
        f'#define INTER_FONT_DATA_H',
        f'',
        f'static const unsigned char {var_name}[] = {{',
    ]
    
    # Write hex bytes in rows of 16
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_str = ', '.join(f'0x{b:02X}' for b in chunk)
        lines.append(f'    {hex_str},')
    
    lines.extend([
        '};',
        f'',
        f'static const unsigned int {var_name}_size = {len(data)};',
        f'',
        f'#endif // INTER_FONT_DATA_H',
        '',
    ])
    
    with open(output_path, 'w') as f:
        f.write('\n'.join(lines))
    
    print(f'Generated {output_path}: {len(data)} bytes embedded')

if __name__ == '__main__':
    font_path = 'assets/fonts/static/Inter_18pt-Regular.ttf'
    output_path = 'misrc_gui/inter_font_data.h'
    embed_font(font_path, output_path)
