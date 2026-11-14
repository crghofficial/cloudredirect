#!/usr/bin/env python3
"""Reject accidental host-library requirements before deploying into Steam."""
import re,subprocess,sys
p=sys.argv[1]
hdr=subprocess.check_output(['readelf','-h',p],text=True)
assert 'ELF64' in hdr and 'X86-64' in hdr, 'Expected x86-64 ELF'
versions=subprocess.check_output(['readelf','-V',p],text=True)
limits={'GLIBC':(2,31),'GLIBCXX':(3,4,28),'CXXABI':(1,3,12)}
for prefix,limit in limits.items():
    found=[tuple(map(int,v.split('.'))) for v in re.findall(r'Name: '+prefix+r'_([0-9.]+)',versions)]
    if found:
        highest=max(found)
        assert highest<=limit, f'{prefix} {highest} exceeds Steam Runtime {limit}'
        print(prefix+'.'+'.'.join(map(str,highest)))
syms=subprocess.check_output(['nm','-D','--defined-only',p],text=True)
assert re.search(r'\bCloudRedirect_CliMain$',syms,re.M),'Missing exported CLI entry'
print('Steam Runtime x86-64 ABI check passed')
