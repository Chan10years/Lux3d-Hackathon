import asyncio
import io
import json
import os
from contextlib import asynccontextmanager
from pathlib import Path
from uuid import uuid4
from fastapi import FastAPI, HTTPException, UploadFile, File, Form
from fastapi.staticfiles import StaticFiles
from PIL import Image, UnidentifiedImageError
from .db import Repository
from .storage import FileStorage
from .jobs import Worker, draw, now

ROOT=Path(__file__).resolve().parents[1]
DATA=Path(os.getenv('WEAPON_DATA_DIR',str(ROOT/'data')))
repo=Repository(str(DATA/'weapons.sqlite3')); storage=FileStorage(DATA/'assets'); worker=Worker(repo,storage)
queue=asyncio.Queue()
async def consume():
    while True:
        key=await queue.get()
        try: await asyncio.to_thread(worker.run,key)
        finally: queue.task_done()
@asynccontextmanager
async def lifespan(app):
    global queue
    queue=asyncio.Queue()
    # Interrupted remote calls are not replayed: that could incur duplicate charges.
    for task in repo.list('tasks'):
        if task['status']=='running':
            task.update(status='failed',error='服务中断，远端状态需人工确认；未自动重试',updated_at=now()); repo.save('tasks',task)
        elif task['status']=='queued': await queue.put(task['id'])
    consumer=asyncio.create_task(consume())
    yield
    consumer.cancel()
app=FastAPI(title='画个锤子 · 本地武器制作 API',version='1.0',lifespan=lifespan)
@app.get('/api/v1/health')
def health(): return {'status':'ok','real_provider_configured':bool(os.getenv('GENERATION_BRIDGE_URL'))}
@app.post('/api/v1/tasks',status_code=202)
async def create(prompt: str=Form(...,min_length=1,max_length=2000), mode: str=Form(...), weapon_type: str=Form(...), sketch: UploadFile=File(...)):
    if mode not in ('mock','real') or weapon_type not in ('short','long'): raise HTTPException(422,'无效模式或武器类型')
    cfg=json.loads((ROOT/'backend/config.json').read_text())
    data=await sketch.read(cfg['max_upload_bytes']+1)
    if len(data)>cfg['max_upload_bytes']: raise HTTPException(413,'草图超过大小限制')
    try:
        with Image.open(io.BytesIO(data)) as im:
            if im.width*im.height>16000000: raise ValueError('图片尺寸过大')
            out=io.BytesIO(); im.convert('RGB').save(out,format='PNG'); data=out.getvalue()
    except (UnidentifiedImageError,OSError,ValueError,Image.DecompressionBombError): raise HTTPException(422,'草图不是有效图片或尺寸过大')
    key=uuid4().hex
    task={'id':key,'prompt':prompt,'mode':mode,'is_mock':mode=='mock','weapon_type':weapon_type,'status':'queued','stage':'queued','draw':draw(cfg,weapon_type),'rules':cfg,'created_at':now(),'updated_at':now(),'error':None}
    task['sketch_url']=storage.write(key,'sketch.png',data)
    repo.save('tasks',task); await queue.put(key)
    return task

def get(table,key):
    value=repo.get(table,key)
    if not value: raise HTTPException(404,'记录不存在')
    return value
@app.get('/api/v1/tasks')
def tasks(): return repo.list('tasks')
@app.get('/api/v1/tasks/{key}')
def task(key:str): return get('tasks',key)
@app.get('/api/v1/weapons')
def weapons(): return repo.list('weapons')
@app.get('/api/v1/weapons/{key}')
def weapon(key:str): return get('weapons',key)
@app.post('/api/v1/weapons/{key}/equip')
def equip(key:str):
    value=get('weapons',key); value['equipped']=True; repo.save('weapons',value); return value
app.mount('/assets',StaticFiles(directory=storage.root),name='assets')
app.mount('/',StaticFiles(directory=ROOT/'test/1028_Lux3D_assumption_test/dist',html=True),name='web')
