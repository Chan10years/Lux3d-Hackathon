import io
import time
from PIL import Image
from fastapi.testclient import TestClient
from backend.main import app

def png():
    out=io.BytesIO(); Image.new('RGB',(32,32),'white').save(out,format='PNG'); return out.getvalue()
def create(client,mode='mock'):
    r=client.post('/api/v1/tasks',data={'prompt':'刀和锤子融合','mode':mode,'weapon_type':'long'},files={'sketch':('s.png',png(),'image/png')})
    assert r.status_code==202,r.text
    return r.json()
def finish(client,task):
    for _ in range(100):
        result=client.get('/api/v1/tasks/'+task['id']).json()
        if result['status'] in ('succeeded','failed'): return result
        time.sleep(.03)
    raise AssertionError('timeout')
def test_mock_lifecycle_and_persistence():
    with TestClient(app) as c:
        assert c.get('/').status_code==200
        t=create(c); result=finish(c,t)
        assert result['status']=='succeeded',result
        assert result['draw']==t['draw']
        w=c.get('/api/v1/weapons/'+t['id']).json()
        assert w['is_mock'] and w['checks']['equipment_check']=='requires_engine_review'
        assert c.get(w['model_url']).content[:4]==b'glTF'
        assert c.get(w['image_url']).headers['content-type']=='image/png'
        assert c.post('/api/v1/weapons/'+t['id']+'/equip').json()['equipped']
    with TestClient(app) as c:
        assert c.get('/api/v1/tasks/'+t['id']).json()['draw']==t['draw']
        assert c.get('/api/v1/weapons/'+t['id']).json()['equipped']
def test_real_failure_never_falls_back(monkeypatch):
    monkeypatch.delenv('GENERATION_BRIDGE_URL',raising=False)
    with TestClient(app) as c:
        t=create(c,'real'); result=finish(c,t)
        assert result['status']=='failed' and not result['is_mock']
        assert 'GENERATION_BRIDGE_URL' in result['error']
        assert c.get('/api/v1/weapons/'+t['id']).status_code==404
def test_invalid_inputs():
    with TestClient(app) as c:
        assert c.post('/api/v1/tasks',data={'prompt':'x','mode':'mock','weapon_type':'long'},files={'sketch':('bad.png',b'bad')}).status_code==422
        assert c.get('/api/v1/tasks/missing').status_code==404

def test_provider_error_and_bad_model(monkeypatch):
    from backend.providers import Provider,validate_glb
    import pytest
    with pytest.raises(ValueError): validate_glb(b'bad')
    original=Provider.call
    def broken(self,stage,payload):
        if stage=='model': raise RuntimeError('upstream model failed')
        return original(self,stage,payload)
    monkeypatch.setattr(Provider,'call',broken)
    with TestClient(app) as c:
        t=create(c); result=finish(c,t)
        assert result['status']=='failed' and result['stage']=='model'
        assert result['draw']==t['draw'] and result['image_url']
        assert c.get('/api/v1/weapons/'+t['id']).status_code==404
