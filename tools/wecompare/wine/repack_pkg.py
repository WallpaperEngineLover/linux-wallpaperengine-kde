import struct,sys,json
# repack.py in.pkg out.pkg name=replacementfile ...
src,dst=sys.argv[1],sys.argv[2]
repl=dict(a.split('=',1) for a in sys.argv[3:])
f=open(src,'rb');r=lambda:struct.unpack('<I',f.read(4))[0]
hl=r();header=f.read(hl);n=r();ents=[]
for _ in range(n):
    nl=r();name=f.read(nl);o=r();l=r();ents.append((name,o,l))
base=f.tell();data=[]
for name,o,l in ents:
    k=name.decode('utf-8','replace')
    if k in repl: data.append((name,open(repl[k],'rb').read()))
    else: f.seek(base+o); data.append((name,f.read(l)))
out=open(dst,'wb');w=lambda v:out.write(struct.pack('<I',v))
w(len(header));out.write(header);w(len(data));off=0
for name,b in data:
    w(len(name));out.write(name);w(off);w(len(b));off+=len(b)
for _,b in data: out.write(b)
