"""Explicit simulation and HTTP bridge contract; no automatic fallback."""
import base64
import io
import os
import struct
import json
import httpx
from PIL import Image

def mock_glb():
    # A real, viewable placeholder triangle, deliberately NOT an AI weapon.
    binary = struct.pack('<9f', -.2,0,0, .2,0,0, 0,1,0)
    doc = {'asset':{'version':'2.0','generator':'EXPLICIT MOCK'},'scene':0,'scenes':[{'nodes':[0]}],'nodes':[{'mesh':0}], 'meshes':[{'primitives':[{'attributes':{'POSITION':0}}]}], 'buffers':[{'byteLength':len(binary)}], 'bufferViews':[{'buffer':0,'byteOffset':0,'byteLength':len(binary)}], 'accessors':[{'bufferView':0,'componentType':5126,'count':3,'type':'VEC3','min':[-.2,0,0],'max':[.2,1,0]}]}
    raw = json.dumps(doc).encode(); raw += b' ' * (-len(raw)%4)
    return struct.pack('<III',0x46546c67,2,28+len(raw)+len(binary))+struct.pack('<II',len(raw),0x4e4f534a)+raw+struct.pack('<II',len(binary),0x004e4942)+binary

class Provider:
    def __init__(self, mode, timeout):
        self.mode, self.timeout = mode, timeout
    def call(self, stage, payload):
        if self.mode == 'mock':
            if stage == 'fusion':
                return {'description': payload['prompt'], 'weapon_type':payload['weapon_type']}
            if stage == 'image':
                out=io.BytesIO(); Image.new('RGB',(512,512),'#b6b0a0').save(out,format='PNG')
                return out.getvalue()
            return mock_glb()
        url = os.getenv('GENERATION_BRIDGE_URL')
        if not url:
            raise RuntimeError('真实生成未配置 GENERATION_BRIDGE_URL；不会使用模拟结果')
        headers = {'Authorization':'Bearer '+os.getenv('GENERATION_BRIDGE_TOKEN','')}
        with httpx.Client(timeout=self.timeout) as client:
            response = client.post(url.rstrip('/')+'/'+stage, json=payload, headers=headers)
            response.raise_for_status()
            result = response.json()
        if stage == 'fusion':
            if not isinstance(result.get('description'),str) or result.get('weapon_type') not in ('short','long'):
                raise ValueError('融合 API 返回格式错误')
            return result
        return base64.b64decode(result['content_base64'], validate=True)

def validate_image(data):
    with Image.open(io.BytesIO(data)) as im:
        im.verify()
    return data

def validate_glb(data):
    if len(data)<20 or len(data)>50*1024*1024:
        raise ValueError('模型为空或超过 50MB')
    magic, version, size = struct.unpack('<III',data[:12])
    if magic != 0x46546c67 or version != 2 or size != len(data):
        raise ValueError('无效 GLB 2.0 文件')
    length, kind = struct.unpack('<II',data[12:20])
    if kind != 0x4e4f534a:
        raise ValueError('GLB 缺少 JSON')
    doc=json.loads(data[20:20+length])
    if not doc.get('meshes') or not doc.get('scenes'):
        raise ValueError('GLB 缺少网格或场景')
    return {'format':'glb','structural_check':'passed','equipment_check':'requires_engine_review'}
