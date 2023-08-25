#!/usr/bin/env python3

from PIL import Image

TABLE_NAME = 'CE_DATA';

TGA_HEADER = [
        0, 0, 3, # type 0 (default), no colormap, greyscale
        0,0, 0,0, 0, # no palette
        0,0, 0,0, 160,0, 144,0, # offset x, y, width, heigh
        8, 0, # bpp, descriptor
        ]

RAW_OFFSET = len(TGA_HEADER)

image = Image.open("china-export.png")

image = image.transpose(Image.FLIP_TOP_BOTTOM)

data = TGA_HEADER + list(image.convert('L').tobytes())


f = open('ce_data.h', 'w')
f.write('// china-export.tga table\n')
f.write('// Automatically generated\n\n')
f.write('#define %s_OFFSET %d\n' % (TABLE_NAME, RAW_OFFSET))
f.write('#define %s_SIZE %d\n' % (TABLE_NAME, len(data)))

f.write('#define %s_WIDTH %d\n' % (TABLE_NAME, image.width))
f.write('#define %s_HEIGHT %d\n' % (TABLE_NAME, image.height))

f.write('const unsigned char %s[%d] = {\n' % (TABLE_NAME, len(data)))
f.write('\t')
for i,byte in enumerate(data):
    f.write('0x%02x, ' % int(byte))
    if i % 16 == 15:
        f.write('\n\t')
f.write('\n};\n\n')

f = open('ce_data.tga', 'wb')
f.write(bytes(data))
