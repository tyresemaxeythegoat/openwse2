import os, struct, collections
import capstone

PROOF=os.path.dirname(os.path.abspath(__file__))

def elf_text(path):
    data=open(path,"rb").read()
    assert data[:4]==b"\x7fELF"
    is64=data[4]==2
    le=data[5]==1
    e_shoff=struct.unpack_from("<Q" if le else ">Q",data,0x28)[0]
    e_shentsize=struct.unpack_from("<H" if le else ">H",data,0x3A)[0]
    e_shnum=struct.unpack_from("<H" if le else ">H",data,0x3C)[0]
    e_shstrndx=struct.unpack_from("<H" if le else ">H",data,0x3E)[0]
    secs=[]
    for i in range(e_shnum):
        off=e_shoff+i*e_shentsize
        name,typ,flags,addr,offset,size=struct.unpack_from("<IIQQQQ",data,off)
        secs.append((name,typ,flags,addr,offset,size))
    strtab=secs[e_shstrndx]
    stroff=strtab[4]
    out=[]
    for name,typ,flags,addr,offset,size in secs:
        end=data.index(b"\0",stroff+name)
        nm=data[stroff+name:end].decode()
        if nm==".text":
            out.append((data[offset:offset+size],addr,size))
    return out

md=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_ARM)
for fn in ("lifted_memcmp_arm64.o","lifted_scaling_arm64.o","lifted_compare_arm64.o","lifted_queryinterface_arm64.o"):
    p=os.path.join(PROOF,fn)
    for code,addr,size in elf_text(p):
        ins=list(md.disasm(code,addr))
        print(f"{fn}: .text {size} bytes, {len(ins)} AArch64 instructions")
        for i in ins[:12]:
            print(f"   {i.address:04x}  {i.mnemonic:<8} {i.op_str}")
        mnem=collections.Counter(i.mnemonic for i in ins)
        print("   top:", ", ".join(f"{k}={v}" for k,v in mnem.most_common(8)))
        print()
