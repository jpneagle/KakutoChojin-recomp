import struct,sys
d=open(sys.argv[1],"rb").read()
base,=struct.unpack_from("<I",d,0x104)
def va(a): return a-base
def cstr(a):
    o=va(a); return d[o:d.index(b"\0",o)].decode("latin1")
def wstr(a):
    o=va(a); e=o
    while d[e:e+2]!=b"\0\0": e+=2
    return d[o:e].decode("utf-16le")
(hsize,imgsize,imghdr,ts,cert,nsec,secaddr,initf,entry,tls,stack,heapr,heapc,pebase,pesize,pecsum,pets,dbgpath,dbgfile,dbgufile,kthunk,nonkern,nlib,libaddr,kliba,xapia)=struct.unpack_from("<IIIIIIIIIIIIIIIIIIIIIIIIII",d,0x108)
import time
print("base",hex(base),"timestamp",time.strftime("%Y-%m-%d %H:%M:%S",time.gmtime(ts)))
print("debug path:",cstr(dbgpath)); print("debug file:",cstr(dbgfile))
co=va(cert); tid,=struct.unpack_from("<I",d,co+8)
print("title id:",hex(tid),"title:",d[co+0xC:co+0xC+80].decode("utf-16le").rstrip("\0"))
reg,=struct.unpack_from("<I",d,co+0xA0); print("region",hex(reg))
# entry/thunk XOR (retail)
print("entry",hex(entry^0xA8FC57AB),"kthunk",hex(kthunk^0x5B6D40B7))
print("sections:")
for i in range(nsec):
    o=va(secaddr)+i*0x38
    fl,v,vs,ra,rs,na=struct.unpack_from("<IIIIII",d,o)
    print(f"  {cstr(na):10s} va={v:08x} vsize={vs:8x} raw={rs:8x} flags={fl:x}")
print("libraries:")
for i in range(nlib):
    o=va(libaddr)+i*16
    name=d[o:o+8].rstrip(b"\0").decode(); maj,mn,bld,fl=struct.unpack_from("<HHHH",d,o+8)
    print(f"  {name:8s} {maj}.{mn}.{bld}")
