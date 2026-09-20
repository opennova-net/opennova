#!/usr/bin/env python3
"""Regenerate synthetic JO instruction witnesses (requires pefile, capstone, unicorn).

Uses jo-c's pinned execution harness: original pursuit/rotation/math instructions,
bounded target/aim/network services. Does not execute a reconstructed motor.
The committed C++ tests consume these outputs without the game or Python modules.
"""
import argparse, hashlib, json, os, struct, sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--jo-c', type=Path, required=True)
parser.add_argument('--retail-exe', type=Path, required=True)
parser.add_argument('--write', action='store_true')
args = parser.parse_args()
port = Path(__file__).resolve().parents[2]
jo = args.jo_c.resolve()
retail = args.retail_exe.resolve()
assert hashlib.sha256(retail.read_bytes()).hexdigest() == 'b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac'
os.chdir(jo)
sys.path.insert(0,str(jo/'tools'))
import guided_missile_oracle as oracle
oracle.configure()
machine=oracle.Machine(retail)

def fixture(path, content):
    path=port/path
    if args.write:
        path.write_text(content,encoding='utf-8')
    else:
        assert path.read_text(encoding='utf-8')==content, str(path)+' differs'

U=65536
roots={'pursuit':0,'steer':1,'stinger':2,'hellfire':3,'javelin':4,'stinger_init':5,'javelin_init':6}
def signed(v): return (v+2**31)%2**32-2**31
cases=[]
for root,category,v in oracle.cases():
 if root not in roots or v.get('fpcw',0x027f)!=0x027f:continue
 r=machine.run(root,v);e=bytes.fromhex(r['entity'])
 i=lambda off:struct.unpack_from('<i',e,off)[0]
 short=lambda off:struct.unpack_from('<H',e,off)[0]
 pos=v.get('position',(10*U,20*U,3*U));angles=v.get('angles',(0,0,0));vel=v.get('velocity',(U,0,0));steer=v.get('steer_position',(120*U,20*U,5*U));saved=v.get('saved_position',(120*U,20*U,5*U))
 target=v.get('target',False);aimtarget=v.get('aim_target',False);tp=v.get('target_position',(120*U,20*U,5*U))
 inp=[roots[root],*pos,*angles,v.get('age',39)-v.get('spawn_phase',0),*vel,*steer,*saved,v.get('flags',0),v.get('phase',0),1 if target else 65535,v.get('timer',10),v.get('initial_range',300*U),v.get('previous_distance',0x7fffffff),v.get('speed',480),*v.get('turn_limits',(6734910,6734910)),v.get('tracking',0x8000000),v.get('authority',1),v.get('session',0),v.get('owner',True),v.get('aim_callback',False),v.get('owner_ai',False),1 if (aimtarget or (v.get('owner_ai',False) and v.get('ai_target',False))) else 65535,*v.get('aim_position',(120*U,20*U,5*U)),target or v.get('acquire',False) or v.get('ai_target',False),v.get('target_health',100)>0,*tp,True,v.get('acquire',False),v.get('flare',False),1,*tp,*v.get('error',(0,0,0)),0,v.get('forward',U),v.get('error_pointer',True)]
 if root=='javelin_init':
  inp[30]=bool(v.get('owner_ai',False))
  inp[32]=1 if (v.get('ai_target',False) if v.get('owner_ai',False) else v.get('owner_lock',False)) else 65535
  inp[33:36]=v.get('camera_pose',(90*U,25*U,10*U))[:3]
  inp[36]=inp[32]!=65535
 target_out=1 if i(724)==oracle.TARGET else 65535
 exp=[i(16),i(20),i(24),i(152),i(156),i(160),i(700),i(704),i(708),i(740),i(744),i(748),short(696),short(698),target_out,i(728),i(736),i(752),*struct.unpack('<4i',bytes.fromhex(r['output']))]
 cases.append((root,category,v,inp,exp))
# Signed dwords, except target/flags/phase and booleans (all below INT_MAX).
fixture('tests/world/fixtures/guided_missile_vectors.inc',
    '// Original x86 PC53 vectors; regenerate with scripts/oracles/jo_c_parity.py.\n' +
    ''.join('{'+','.join(str(signed(int(x))) for x in row[3]+row[4])+'}, // '+row[0]+' '+row[1]+'\n' for row in cases))

from material_producers_runtime_oracle import Machine, OBJ, NODE
m=Machine(retail)
rows=[]
for w,h in [(4,4),(8,4),(4,8),(8,8),(16,16),(7,9),(32,8)]:
    for pattern in (0,1):
        m.reset(); m.services()
        pixels=m.pixels(NODE,w,h,pattern)
        m.call(0x58A220,(OBJ,NODE))
        expected=m.read(m.rd(OBJ+40),(w>>2)*(h>>2)*16*4)
        rows.append((w,h,pixels.hex(),expected.hex()))
fixture('tests/renderer/material_horizon_vectors.inc',
    '// Original x86 @0x58A220; fixture provenance in docs/jo-c-validation-2026-09-18.md.\n' +
    ''.join('{%d, %d, "%s", "%s"},\n'%r for r in rows))
print('PASS:',len(cases),'guided launch/motor/math cases and',len(rows),'horizon volumes')
