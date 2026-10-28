#!/usr/bin/env python3
"""A stand-in libSDL2-2.0.so.0 to link the TrimUI build against: every SDL
function the sources call, as an empty function. The device's own SDL2
(TrimUI OS ships it) is the one loaded at run time; this only gives the
linker the names and the library's soname.

usage: mksdlstub.py OUT.c SOURCE.c ...
"""
import re
import sys

# SDL macros that call other functions
MACROS = {"SDL_SaveBMP": ["SDL_SaveBMP_RW", "SDL_RWFromFile"], "SDL_LoadBMP": ["SDL_LoadBMP_RW", "SDL_RWFromFile"]}

names = set()
for path in sys.argv[2:]:
    src = open(path).read()
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    names |= set(re.findall(r"\b(SDL_[A-Za-z0-9_]+)\s*\(", src))
for m, real in MACROS.items():
    if m in names:
        names.discard(m)
        names |= set(real)
with open(sys.argv[1], "w") as f:
    f.write("/* made by tools/mksdlstub.py: link-time stand-ins, never run */\n")
    for n in sorted(names):
        f.write("void %s(void) {}\n" % n)
