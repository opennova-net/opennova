import argparse, pefile,struct,hashlib,json
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32,UC_HOOK_CODE
from unicorn.x86_const import *
from pathlib import Path
parser=argparse.ArgumentParser(description="Execute original mixer preset selection and output with controlled bus input.")
parser.add_argument('--retail-exe',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
exe=args.retail_exe
assert hashlib.sha256(exe.read_bytes()).hexdigest()=='b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac'
pe=pefile.PE(str(exe));image=pe.get_memory_mapped_image()
P=0x60000000;SP=P+0xe0000;STOP=P+0xff000
pack=lambda *v:struct.pack('<'+'I'*len(v),*(x&0xffffffff for x in v))
results=[]
for index,custom in [(i,custom) for custom in (False,True) for i in range(20)]:
 u=Uc(UC_ARCH_X86,UC_MODE_32);u.mem_map(0x400000,(pe.OPTIONAL_HEADER.SizeOfImage+4095)&~4095);u.mem_write(0x400000,image);u.mem_map(P,0x100000)
 u.mem_write(0x795000,bytes(0x3b2c));u.mem_write(0x798878,pack(128,128,255,128));u.mem_write(0x3346f8c,pack(4));u.mem_write(0x3346f94,pack(4096,P+0x10000,0x7bd71a,index));u.mem_write(SP,pack(*([0]*8),STOP));u.reg_write(UC_X86_REG_ESP,SP)
 if custom:u.mem_write(0x7bf400+24*index,bytes((i*71+3)&255 for i in range(24)))
 u.mem_write(P+0x2000,b'\x0f\x6f\x05'+pack(P+0x1000)+b'\x0f\x6f\x0d'+pack(P+0x1008)+b'\x0f\x6f\x15'+pack(P+0x1010)+b'\xe9'+pack(0x7bec79-(P+0x2000+26)))
 frame=[0]
 def mix(uc,pc,size,data):
  n=frame[0];frame[0]+=1
  values=[(12000,-10000,4000,2000),(3000,5000,-2000,1000),(500,800,0,0)] if n==0 else [(0,0,0,0)]*3
  uc.mem_write(P+0x1000,b''.join(struct.pack('<4h',*vals) for vals in values))
  uc.reg_write(UC_X86_REG_EIP,P+0x2000)
 u.hook_add(UC_HOOK_CODE,mix,begin=0x7bdd90,end=0x7bdd90)
 u.emu_start(0x7bdd12,STOP,count=2000000)
 assert u.reg_read(UC_X86_REG_EIP)==STOP
 output=bytes(u.mem_read(P+0x10000,4096))
 results.append(dict(index=index,custom=custom,frames=frame[0],sha256=hashlib.sha256(output).hexdigest(),selected=bytes(u.mem_read(0x798898,24)).hex(),nonzero=sum(x!=0 for x in output)))
args.output.write_text(json.dumps({'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'service':'controlled MMX bus samples at 0x7BDD90; original preset select, filters, stereo pack, and pacing execute','cases':results},indent=2))
assert len(set(x['sha256'] for x in results))==1
assert results[0]['nonzero']>0

print("PASS:",len(results),"original mixer preset cases; output SHA256",results[0]["sha256"])
