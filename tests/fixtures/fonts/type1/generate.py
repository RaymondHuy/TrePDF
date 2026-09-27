"""Generate an original, deterministic Type 1 PFA fixture using only stdlib.

Outlines are simple rectangles, with constant advances and varying ink widths.
No third-party font data is used. Run with Python 3 to regenerate the fixture.
"""
from pathlib import Path

def encrypt(data, seed):
    out = bytearray()
    for value in data:
        value ^= seed >> 8
        out.append(value)
        seed = ((value + seed) * 52845 + 22719) & 0xffff
    return bytes(out)

def number(n):
    if -107 <= n <= 107:
        return bytes([n + 139])
    if 108 <= n <= 1131:
        n -= 108
        return bytes([247 + n // 256, n % 256])
    if -1131 <= n <= -108:
        n = -n - 108
        return bytes([251 + n // 256, n % 256])
    return b'\xff' + n.to_bytes(4,'big',signed=True)

def glyph(index):
    # Constant 500-unit advance with visibly different outlines per character.
    code = number(0) + number(500) + b'\x0d'
    if index:
        w = 160 + index % 5 * 45
        h = 350 + index % 7 * 35
        code += number(30) + number(0) + b'\x15'
        for dx,dy in [(w,0),(0,h),(-w,0),(0,-h)]:
            code += number(dx) + number(dy) + b'\x05'
        code += b'\x09'
    return encrypt(b'\x00'*4 + code + b'\x0e', 4330)

names = {32:'space',46:'period',47:'slash',58:'colon'}
names.update({ord(c):c for c in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'})
names.update(dict(zip(range(48,58),'zero one two three four five six seven eight nine'.split())))
header = '''%!PS-AdobeFont-1.0: TrePDFTestMono-Regular 1.0
% Synthetic test outlines generated locally; no third-party font data.
10 dict begin
/FontType 1 def
/FontMatrix [0.001 0 0 0.001 0 0] readonly def
/FontName /TrePDFTestMono-Regular def
/FontBBox [0 0 500 700] readonly def
/PaintType 0 def
/FontInfo 8 dict dup begin
/version (1.0) readonly def
/Notice (Synthetic TrePDF regression font) readonly def
/FullName (TrePDF Test Mono Regular) readonly def
/FamilyName (TrePDF Test Mono) readonly def
/Weight (Regular) readonly def
/ItalicAngle 0 def
/isFixedPitch true def
/UnderlinePosition -100 def
end readonly def
/Encoding 256 array
0 1 255 {1 index exch /.notdef put} for
'''
header += ''.join(f'dup {code} /{name} put\n' for code,name in sorted(names.items()))
header += 'readonly def\ncurrentdict end\ncurrentfile eexec\n'
private = b'dup /Private 8 dict dup begin\n/RD {string currentfile exch readstring pop} executeonly def\n/ND {noaccess def} executeonly def\n/NP {noaccess put} executeonly def\n/BlueValues [] def\n/MinFeature {16 16} def\n/password 5839 def\n/lenIV 4 def\n/Subrs 0 array def\n2 index /CharStrings '+str(len(names)+1).encode()+b' dict dup begin\n'
for i,name in [(0,'.notdef')]+list(enumerate(names.values(),1)):
    char = glyph(0 if name=='space' else i)
    private += f'/{name} {len(char)} RD '.encode()+char+b' ND\n'
private += b'end\nend\nreadonly put\nnoaccess put\ndup /FontName get exch definefont pop\nmark currentfile closefile\n'
cipher = encrypt(b'\0'*4+private,55665).hex()
result=header.encode()+('\n'.join(cipher[i:i+64] for i in range(0,len(cipher),64))+'\n').encode()+b'0'*512+b'\ncleartomark\n'
Path(__file__).with_name('trepdf-test-mono.pfa').write_bytes(result)
