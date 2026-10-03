#!/usr/bin/env python3
"""Counts what every installed .mdl carries, read the way wallpaper64.exe 2.8.42 reads it (sub_140261880).

Usage:
    mdl_scan.py [workshop dir]

Walks every item's .pkg files and loose .mdl files plus the assets dir and prints, per feature, how many files
have it and which items: MDLV versions, vertex formats, mesh flag bits, the MDLV 21 auxiliary vertex block and part
ranges, MDLV 23 clipping records, and every section after the meshes (MDLS/MDLA/MDAT/MDMP/MDLE with versions).
"""
import struct, os, glob, collections, sys
roots=[sys.argv[1] if len(sys.argv) > 1 else os.environ.get('LWE_WORKSHOP_DIR', '/workspace/SteamLibrary/steamapps/workshop/content/431960')]
assets=os.environ.get('LWE_ASSETS_DIR', '/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets')
def pkg_entries(pkg):
    with open(pkg,'rb') as f:
        n=struct.unpack('<I',f.read(4))[0]; f.read(n)
        c=struct.unpack('<I',f.read(4))[0]; ents=[]
        for _ in range(c):
            n=struct.unpack('<I',f.read(4))[0]; name=f.read(n).decode('utf-8','replace')
            o,l=struct.unpack('<II',f.read(8)); ents.append((name,o,l))
        base=f.tell()
        for name,o,l in ents:
            if name.endswith('.mdl'):
                f.seek(base+o); yield name,f.read(l)
def files():
    for item in sorted(os.listdir(roots[0])):
        d=os.path.join(roots[0],item)
        for pkg in glob.glob(d+'/*.pkg'):
            for name,data in pkg_entries(pkg): yield item,name,data
        for p in glob.glob(d+'/**/*.mdl',recursive=True):
            yield item,os.path.relpath(p,d),open(p,'rb').read()
    for p in glob.glob(assets+'/**/*.mdl',recursive=True):
        yield 'assets',os.path.relpath(p,assets),open(p,'rb').read()
class R:
    def __init__(s,d,o=0): s.d=d; s.o=o
    def u32(s): v=struct.unpack_from('<I',s.d,s.o)[0]; s.o+=4; return v
    def u8(s): v=s.d[s.o]; s.o+=1; return v
    def skip(s,n): s.o+=n
    def s(s):
        e=s.d.index(b'\0',s.o); v=s.d[s.o:e].decode('utf-8','replace'); s.o=e+1; return v
def note(stats,ex,key,item,n=1):
    if n: stats[key]+=n; ex[key].add(item)

# MDLS like PuppetRig::parsePuppetBones: bones, then (v2+) extra records, rest pose, constraints
def mdls(d,o,ver,stats,ex,item):
    r=R(d,o+13); bones=r.u32()
    for _ in range(bones):
        r.s(); r.u32(); r.u32(); r.skip(r.u32()); r.s()
    rig={'bones':bones,'extras':0,'constraints':0}
    if ver<2: return rig
    extras=struct.unpack_from('<H',d,r.o)[0]; r.skip(2)
    for _ in range(extras): r.s(); r.skip(8+64)
    note(stats,ex,'MDLS extra records',item,extras)
    if r.u8(): r.skip(64*(bones+extras)); note(stats,ex,'MDLS rest pose',item)
    cons=r.u32(); note(stats,ex,'MDLS constraints',item,cons)
    for _ in range(cons):
        r.skip(12); fl=r.u32() if ver>=4 else 0
        if fl&2: r.skip(8)
    rig.update(extras=extras,constraints=cons)
    return rig

# MDLA like PuppetRig::parsePuppetAnimationClips, counting the tracks we skip
def mdla(d,o,ver,rig,meshes,stats,ex,item):
    r=R(d,o+13); clips=r.u32()
    for _ in range(clips):
        r.skip(8); r.s(); r.s(); r.skip(4); frames=r.u32(); flags=r.u32(); bones=r.u32(); n=frames+1
        for _ in range(bones): r.u32(); r.skip(r.u32())
        def tracks(count, key):
            k=0
            for _ in range(count): r.u32(); r.skip(r.u32()); k+=1
            note(stats,ex,key,item,k)
        if ver>=2:
            tracks(rig['extras'],'MDLA v2 extra tracks'); tracks(rig['constraints'],'MDLA v2 constraint tracks')
        if ver>=3:
            tracks(r.u32(),'MDLA v3 counted float tracks')
            if r.u8(): tracks(bones,'MDLA v3 bone alpha tracks')
        if ver>=4 and r.u8():
            for _ in range(meshes):
                if r.u32()&1:
                    r.u32(); c=struct.unpack_from('<H',d,r.o)[0]; r.skip(2)
                    for _ in range(c): r.skip(2); r.skip(r.u32())
                    note(stats,ex,'MDLA v4 morph tracks',item,c)
        if ver>=5:
            ints=struct.unpack_from('<6I',d,r.o); r.skip(24)
            if any(ints): note(stats,ex,'MDLA v5 ints nonzero',item)
        if ver>=6 and r.u8(): tracks(bones,'MDLA v6 draw order tracks')
        if flags&1: r.skip(2+16); note(stats,ex,'MDLA root motion',item)
        ev=r.u32()
        for _ in range(ev): r.u32(); r.s()
        note(stats,ex,'MDLA events',item,ev)

stats=collections.Counter(); ex=collections.defaultdict(set)
for item,name,d in files():
    try:
        r=R(d); magic=r.s()
        if not magic.startswith('MDLV'): stats['notMDLV']+=1; continue
        ver=int(magic[4:]); fmt=r.u32(); mpm=r.u32(); mc=r.u32()
        stats['MDLV%04d'%ver]+=1; ex['MDLV%04d'%ver].add(item)
        if mc>1: stats['multimesh']+=1; ex['multimesh'].add(item)
        if mpm!=1: stats['materialsPerMesh=%d'%mpm]+=1; ex['materialsPerMesh=%d'%mpm].add(item)
        for i in range(mc):
            for _ in range(mpm): r.s()
            fl=0
            if ver>=4:
                fl=r.u32()
                if fl&2: r.u32(); stats['meshflag2']+=1; ex['meshflag2'].add(item)
                if ver>=17: r.o+=24
                if ver>=15: fmt2=r.u32(); stats['fmt=%x'%fmt2]+=1; ex['fmt=%x'%fmt2].add(item)
            for b in range(32):
                if fl&(1<<b): stats['meshflagbit %#x'%(1<<b)]+=1; ex['meshflagbit %#x'%(1<<b)].add(item)
            r.skip(r.u32()); r.skip(r.u32())
            if ver>=21:
                if r.u8(): r.u32(); r.skip(r.u32()); stats['v21 aux block']+=1; ex['v21 aux block'].add(item)
                if r.u8(): r.skip(r.u32()); stats['v21 ranges']+=1
            if ver>=23:
                n=r.u32()
                if n: stats['clip records']+=1; ex['clip records'].add(item)
                for _ in range(n):
                    r.o+=8; r.s(); r.u32(); r.skip(4*r.u32()); r.skip(4*r.u32())
        o=r.o; seen=set(); rig=None
        while o+13<=len(d) and d[o]!=0 and o not in seen:
            seen.add(o); tag=d[o:o+8].decode('ascii','replace'); nxt=struct.unpack_from('<I',d,o+9)[0]
            stats[tag]+=1; ex[tag].add(item)
            try:
                if tag.startswith('MDLS'): rig=mdls(d,o,int(tag[4:]),stats,ex,item)
                if tag.startswith('MDLA') and rig: mdla(d,o,int(tag[4:]),rig,mc,stats,ex,item)
            except Exception as e:
                stats['%s parseERR'%tag[:4]]+=1; ex['%s parseERR'%tag[:4]].add(item)
            if nxt<=o: break
            o=nxt
        if o<len(d) and o not in seen: pass
    except Exception as e:
        stats['parseERR']+=1; ex['parseERR'].add(item+':'+name+':'+str(e)[:40])
for k in sorted(stats): print('%-22s %5d  %s'%(k,stats[k],' '.join(sorted(ex[k]))[:150]))
