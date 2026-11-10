#!/usr/bin/env python3
# A bare FFS partition image (whdboot.c's / ffs.c's, or any DOS\1 HDF) checked
# block by block: header checksums, the hash chains, each file's data and
# extension blocks, no block used twice, the bitmap matching what is used.
#   ffs_check.py image.hdf [outdir]   (outdir: the files written out)
import os, struct, sys
d=open(sys.argv[1],'rb').read(); P=len(d)//512
B=lambda n:d[n*512:(n+1)*512]
L=lambda b,i:struct.unpack('>I',b[i*4:i*4+4])[0]
def ok(b): return sum(struct.unpack('>128I',b))&0xffffffff==0
print("boot", B(0)[:4], "blocks", P)
R=(P-1+2)//2; r=B(R); print('root',R,'type',L(r,0),'sec',hex(L(r,127)),'ok',ok(r),'name',r[0x1b1:0x1b1+r[0x1b0]], 'bmflag',hex(L(r,0x138//4)))
def h(name):
    v=len(name)
    for c in name.upper(): v=(v*13+c)&0x7ff
    return v%72
used={}
def use(k,what):
    assert k not in used,('overlap',k,what,used[k]); used[k]=what
def ls(blk,ind,path):
    b=B(blk)
    for i in range(72):
        k=L(b,6+i)
        while k:
            e=B(k); n=e[0x1b1:0x1b1+e[0x1b0]]
            use(k,n)
            assert ok(e),('cksum',n); assert L(e,1)==k; assert h(n)==i,('hash',n); assert L(e,125)==blk
            st=L(e,127)
            if st==2: print(' '*ind+n.decode('latin1')+'/'); ls(k,ind+2,path+n+b'/')
            else:
                size=L(e,81); data=bytearray(); hb=e
                while True:
                    for j in range(L(hb,2)): use(L(hb,77-j),n); data+=B(L(hb,77-j))
                    nx=L(hb,126)
                    if not nx: break
                    use(nx,n); hb=B(nx); assert ok(hb) and L(hb,0)==16
                data=data[:size]
                print(' '*ind+n.decode('latin1'),size,hex(L(e,80)),len(data)==size)
                if len(sys.argv)>2:
                    p=os.path.join(sys.argv[2],(path+n).decode('latin1')); os.makedirs(os.path.dirname(p),exist_ok=True); open(p,'wb').write(data)
            k=L(e,124)
ls(R,0,b'')

use(R,'root'); nbm=0
for i in range(25):
    k=L(r,79+i)
    if not k: break
    use(k,'bitmap'); nbm+=1
free=set()
for i in range(nbm):
    bm=B(L(r,79+i)); assert sum(struct.unpack('>128I',bm))&0xffffffff==0,'bm cksum'
    for w in range(127):
        v=L(bm,1+w)
        for bit in range(32):
            blk=2+i*127*32+w*32+bit
            if blk<P and v>>bit&1: free.add(blk)
bad=[k for k in used if k in free]
miss=[k for k in range(2,P) if k not in used and k not in free]
print('blocks used',len(used),'free',len(free),'used-but-free',len(bad),'leaked',len(miss))
assert not bad
