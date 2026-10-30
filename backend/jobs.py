import base64
import copy
import random
from datetime import datetime, timezone
from .providers import Provider, validate_glb, validate_image

def now(): return datetime.now(timezone.utc).isoformat()

def draw(config, kind):
    rarity=random.choices(list(config['rarities']), weights=list(config['rarities'].values()))[0]
    def choose(pool):
        return random.choice([s for s in config[pool] if kind in s['types']])
    return {'rarity':rarity,'attack':round(10*config['multipliers'][rarity]), 'normal_skill':choose('normal_skills'), 'special_skill':choose('special_skills')}

class Worker:
    def __init__(self, repo, storage): self.repo,self.storage=repo,storage
    def run(self, task_id):
        task=self.repo.get('tasks',task_id)
        def stage(name):
            task.update(status='running',stage=name,updated_at=now()); self.repo.save('tasks',task)
        try:
            cfg=task['rules']; provider=Provider(task['mode'],cfg['provider_timeout_seconds'])
            stage('fusion')
            sketch=(self.storage.root/task_id/'sketch.png').read_bytes()
            payload={'prompt':task['prompt'],'weapon_type':task['weapon_type'],'sketch_base64':base64.b64encode(sketch).decode()}
            fusion=provider.call('fusion',payload)
            task['fusion']=fusion
            # Player-selected type is authoritative for gameplay; AI only describes appearance.
            stage('image')
            image=validate_image(provider.call('image',{**payload,'description':fusion['description']}))
            task['image_url']=self.storage.write(task_id,'reference.png',image)
            stage('model')
            model=provider.call('model',{'image_base64':base64.b64encode(image).decode(),'description':fusion['description']})
            stage('validation')
            checks=validate_glb(model)
            model_url=self.storage.write(task_id,'weapon.glb',model)
            weapon={'id':task_id,'task_id':task_id,'mode':task['mode'],'is_mock':task['mode']=='mock','name':task['prompt'],'attributes':task['draw'],'image_url':task['image_url'],'model_url':model_url,'checks':checks,'equipment':{'length_m':cfg['length_m'][task['weapon_type']],'forward_axis':'+Z','up_axis':'+Y','grip_position':[0,0,0],'collider':'engine_review_required'},'equipped':False,'created_at':now()}
            self.repo.save('weapons',weapon)
            task.update(status='succeeded',stage='complete',weapon_id=task_id,error=None)
        except Exception as exc:
            task.update(status='failed',error=f'{type(exc).__name__}: {exc}')
        task['updated_at']=now(); self.repo.save('tasks',task)
