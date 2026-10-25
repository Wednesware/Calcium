"""Regenerate the native Unicode case tables, then run clang-format on the C file.

This build-time helper uses Python's Unicode database. Generated programs do not.
The contextual case-ignorable probe derives the final-sigma property from Python's
full casing behavior, alongside Unicode general categories.
"""

import unicodedata
from pathlib import Path

if unicodedata.unidata_version != "16.0.0":
    raise SystemExit("Generate with CPython Unicode 16.0.0 (Python 3.14)")
# Derive context and simple/full case data from the host's versioned Unicode database.
low = []
up = []
space = []
cased = []
ignore = []
for cp in range(0x110000):
    c = chr(cp)
    if c.lower() != c:
        low.append((cp, c.lower().encode("utf8")))
    if c.upper() != c:
        up.append((cp, c.upper().encode("utf8")))
    if c.isspace():
        space.append(cp)
    is_cased = c.islower() or c.isupper() or c.istitle()
    if is_cased:
        cased.append(cp)
    if unicodedata.category(c) in {"Mn", "Me", "Cf", "Lm", "Sk"} or (
        not is_cased and ("AΣ" + c + "A").lower()[1] == "σ"
    ):
        ignore.append(cp)


def ranges(values):
    result = []
    for v in values:
        if result and result[-1][1] + 1 == v:
            result[-1] = (result[-1][0], v)
        else:
            result.append((v, v))
    return result


s = f'/* Generated from CPython Unicode {unicodedata.unidata_version}; full case mappings. */\n#include "cart.h"\n'
s += "typedef struct {uint32_t code;const char *text;size_t length;} Mapping;\n"
for name, rows in [("lower", low), ("upper", up)]:
    s += (
        "static const Mapping "
        + name
        + "[]={\n"
        + "".join(
            '{0x%x,"%s",%d},\n' % (cp, "".join("\\%03o" % b for b in value), len(value))
            for cp, value in rows
        )
        + "};\n"
    )
for name, rows in [("space", space), ("cased", cased), ("ignorable", ignore)]:
    s += (
        "static const uint32_t "
        + name
        + "[][2]={"
        + ",".join("{0x%x,0x%x}" % r for r in ranges(rows))
        + "};\n"
    )
s += """static int in_ranges(uint32_t value,const uint32_t (*table)[2],size_t count){size_t low=0,high=count;while(low<high){size_t mid=low+(high-low)/2;if(value<table[mid][0])high=mid;else if(value>table[mid][1])low=mid+1;else return 1;}return 0;}
int ca_unicode_space(uint32_t c){return in_ranges(c,space,sizeof space/sizeof space[0]);}
int ca_unicode_cased(uint32_t c){return in_ranges(c,cased,sizeof cased/sizeof cased[0]);}
int ca_unicode_ignorable(uint32_t c){return in_ranges(c,ignorable,sizeof ignorable/sizeof ignorable[0]);}
const char *ca_unicode_case(uint32_t c,int upper_case,size_t *length){const Mapping *table=upper_case?upper:lower;size_t low=0,high=upper_case?sizeof upper/sizeof upper[0]:sizeof lower/sizeof lower[0];while(low<high){size_t mid=low+(high-low)/2;if(c<table[mid].code)high=mid;else if(c>table[mid].code)low=mid+1;else{*length=table[mid].length;return table[mid].text;}}return NULL;}
"""
(Path(__file__).resolve().parents[1] / "calcium/runtime/native/src/unicode_case.c").write_text(s)
