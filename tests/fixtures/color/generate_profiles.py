"""Generate synthetic ICC fixtures and independent LittleCMS references.

Run from the repository root. No application data is read.
"""
import ctypes as c
import json
import struct
import zlib
from pathlib import Path
lib=c.CDLL('liblcms2.so.2')
class XY(c.Structure):
    _fields_=[('x',c.c_double),('y',c.c_double),('Y',c.c_double)]
class Triple(c.Structure):
    _fields_=[('red',XY),('green',XY),('blue',XY)]
lib.cmsCreate_sRGBProfile.restype=c.c_void_p
lib.cmsBuildGamma.argtypes=[c.c_void_p,c.c_double];lib.cmsBuildGamma.restype=c.c_void_p
lib.cmsBuildParametricToneCurve.argtypes=[c.c_void_p,c.c_int,c.POINTER(c.c_double)]
lib.cmsBuildParametricToneCurve.restype=c.c_void_p
lib.cmsCreateRGBProfile.argtypes=[c.POINTER(XY),c.POINTER(Triple),c.POINTER(c.c_void_p)];lib.cmsCreateRGBProfile.restype=c.c_void_p
lib.cmsSaveProfileToMem.argtypes=[c.c_void_p,c.c_void_p,c.POINTER(c.c_uint32)]
lib.cmsCloseProfile.argtypes=[c.c_void_p]
lib.cmsFreeToneCurve.argtypes=[c.c_void_p]
root=Path('tests/fixtures/color');root.mkdir(exist_ok=True)
profiles={'srgb':lib.cmsCreate_sRGBProfile()}
for name,primaries,gamma in [('display_p3',[(.68,.32),(.265,.69),(.15,.06)],2.2),('adobe_rgb',[(.64,.33),(.21,.71),(.15,.06)],2.19921875)]:
    if name == 'display_p3':
        params=(c.c_double*5)(2.4,1/1.055,.055/1.055,1/12.92,.04045)
        curves=(c.c_void_p*3)(*[lib.cmsBuildParametricToneCurve(None,4,params) for _ in range(3)])
    else:
        curves=(c.c_void_p*3)(*[lib.cmsBuildGamma(None,gamma) for _ in range(3)])
    profiles[name]=lib.cmsCreateRGBProfile(c.byref(XY(.3127,.3290,1)),c.byref(Triple(*[XY(x,y,1) for x,y in primaries])),curves)
    for curve in curves:lib.cmsFreeToneCurve(curve)
lib.cmsCreateTransform.argtypes=[c.c_void_p,c.c_uint32,c.c_void_p,c.c_uint32,c.c_uint32,c.c_uint32]
lib.cmsCreateTransform.restype=c.c_void_p
lib.cmsDoTransform.argtypes=[c.c_void_p,c.c_void_p,c.c_void_p,c.c_uint32]
lib.cmsDeleteTransform.argtypes=[c.c_void_p]
references={}
for name,profile in profiles.items():
    rgb8=(4<<16)|(3<<3)|1
    transform=lib.cmsCreateTransform(profile,rgb8,profiles['srgb'],rgb8,0,0)
    source=(c.c_ubyte*3)(191,77,51)
    output=(c.c_ubyte*3)()
    lib.cmsDoTransform(transform,source,output,1)
    references[name]=list(output)
    lib.cmsDeleteTransform(transform)
(root/'reference.json').write_text(json.dumps(references,indent=2)+'\n')
for name,profile in profiles.items():
    size=c.c_uint32();lib.cmsSaveProfileToMem(profile,None,c.byref(size));buf=c.create_string_buffer(size.value);lib.cmsSaveProfileToMem(profile,buf,c.byref(size));lib.cmsCloseProfile(profile)
    (root/(name+'.icc')).write_bytes(buf.raw)
    if name=='srgb':
        lines=['    '+', '.join(str(x) for x in buf.raw[i:i+16])+',' for i in range(0,len(buf.raw),16)]
        Path('src/attachment/srgb_profile.hpp').write_text('#pragma once\n#include <array>\n\n// LittleCMS-generated standard sRGB profile for validation transforms.\ninline constexpr std::array<unsigned char, '+str(len(buf.raw))+'> SRGB_PROFILE{\n'+'\n'.join(lines)+'\n};\n')

# Minimal RGB PNGs with explicit format signaling or embedded profiles.
def chunk(kind, payload):
    return struct.pack('>I', len(payload)) + kind + payload + struct.pack(
        '>I', zlib.crc32(kind + payload))

def png(metadata):
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack(
        '>IIBBBBB', 1, 1, 8, 2, 0, 0, 0)) + metadata +
        chunk(b'IDAT', zlib.compress(bytes([0, 191, 77, 51]))) +
        chunk(b'IEND', b''))

(root/'unprofiled.png').write_bytes(png(b''))
(root/'tagged.png').write_bytes(png(chunk(b'sRGB', b'\0')))
invalid = bytearray(128)
invalid[0:4] = struct.pack('>I', 128)
invalid[36:40] = b'acsp'
(root/'invalid_profile.png').write_bytes(png(chunk(
    b'iCCP', b'invalid\0\0' + zlib.compress(invalid))))
for name in profiles:
    (root/(name+'.png')).write_bytes(png(chunk(
        b'iCCP', name.encode() + b'\0\0' + zlib.compress(
            (root/(name+'.icc')).read_bytes()))))
