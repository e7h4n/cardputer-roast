#!/usr/bin/env python3
"""Validate the OTA image before publishing; never flash a full-flash or wrong-board image."""
import hashlib,json,os,struct,sys
from pathlib import Path
PROFILE=b'ESPHUB1/micropython/cardputer-adv\0'
def validate(data):
    if len(data)<320 or len(data)>0x400000: raise ValueError('application must fit the 4 MiB app slot')
    if data[0]!=0xe9 or struct.unpack_from('<H',data,12)[0]!=9: raise ValueError('expected an ESP32-S3 app image')
    if struct.unpack_from('<I',data,32)[0]!=0xabcd5432: raise ValueError('missing ESP application descriptor')
    if PROFILE not in data[288:4096]: raise ValueError('missing Cardputer ADV ESP Hub OTA profile')
    return hashlib.sha256(data).hexdigest()
if __name__=='__main__':
    binary=Path(sys.argv[1]);data=binary.read_bytes();sha=validate(data)
    version=sys.argv[2];repo=os.environ.get('GITHUB_REPOSITORY','e7h4n/cardputer-roast')
    manifest={'board':'cardputer-adv','kind':'micropython','native':True,'version':version,'file':binary.name,'size':len(data),'sha256':sha,'app_offset':0x2a0000,'image_profile':PROFILE[:-1].decode(),'url':f'https://github.com/{repo}/releases/download/{version}/{binary.name}'}
    (binary.parent/'SHA256SUMS').write_text(f'{sha}  {binary.name}\n')
    (binary.parent/'esphub.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(manifest))
