import struct
d=open("na.bin","rb").read();B=0x08010000
def decomp(src,n):
    i=src-B;out=bytearray()
    while len(out)<n:
        t=d[i];i+=1
        lit=t&7
        if not lit: lit=d[i];i+=1
        ln=t>>4
        if not ln: ln=d[i];i+=1
        out+=d[i:i+lit-1];i+=lit-1
        if t&8:
            off=d[i];i+=1;ln+=2;p=len(out)-off
            for _ in range(ln): out.append(out[p]);p+=1
        else: out+=bytes(ln)
    return bytes(out[:n])
ram=decomp(0x801acd0,0x648)
open("data0.bin","wb").write(ram)
G={0x40010800:"PA",0x40010C00:"PB",0x40011000:"PC",0x40011400:"PD",0:"--"}
def pin(p,m): return G.get(p,hex(p))+(",".join(str(k) for k in range(16) if m>>k&1) if p else "")
for ax in range(5):
    w=struct.unpack_from("<10I",ram,0x540+ax*40)
    print(ax,"step",pin(w[0],w[1]),"dir",pin(w[2],w[3]),"en",pin(w[4],w[5]),"es1",pin(w[6],w[7]),"es2",pin(w[8],w[9]))
