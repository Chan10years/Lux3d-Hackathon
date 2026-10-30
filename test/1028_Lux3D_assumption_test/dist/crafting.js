const area = document.createElement('article');
area.innerHTML = `<h2>武器制作 · 本地后端</h2><p>使用左侧草图和文字。模拟结果是占位图与三角模型，不代表 AI 生成。</p><label>生成模式<select id="craftMode"><option value="mock">模拟 · 本地流程测试</option><option value="real">真实 API · 需配置服务</option></select></label><label>基础类型（临时由玩家选择）<select id="craftType"><option value="short">短武器</option><option value="long">长武器</option></select></label><button id="craftSubmit" class="primary">创建武器任务</button><button id="craftRefresh">刷新任务记录</button><p id="craftStatus" role="status"></p><div id="craftTasks"></div><div id="craftPreview"></div>`;
document.querySelector('#create .grid').append(area);
const $ = id => document.getElementById(id);
async function request(url, options) {
  const r = await fetch(url, options);
  if (!r.ok) throw new Error(`${r.status}: ${await r.text()}`);
  return r.json();
}
let timer;
async function refresh() {
  clearTimeout(timer);
  const tasks = await request('/api/v1/tasks');
  $('craftTasks').replaceChildren();
  for (const task of tasks) {
    const row = document.createElement('div');
    const label = document.createElement('p');
    label.textContent = `${task.is_mock ? '【模拟】' : '【真实 API】'} ${task.id.slice(0,8)} · ${task.prompt} · ${task.status}/${task.stage} · ${task.draw.rarity}${task.error ? ' · '+task.error : ''}`;
    row.append(label);
    if (task.weapon_id) {
      const button = document.createElement('button'); button.textContent = '预览 / 装备';
      button.onclick = async () => {
        try {
          const w = await request('/api/v1/weapons/'+task.weapon_id);
          const box = $('craftPreview'); box.replaceChildren();
          const info = document.createElement('pre'); info.textContent = JSON.stringify(w,null,2); box.append(info);
          const image = document.createElement('img'); image.src=w.image_url; image.alt=w.is_mock?'模拟占位图':'生成参考图'; image.style.maxWidth='100%'; box.append(image);
          const viewer = document.createElement('model-viewer'); viewer.setAttribute('src',w.model_url); viewer.setAttribute('camera-controls',''); viewer.setAttribute('loading','eager'); viewer.style.cssText='width:100%;height:300px'; box.append(viewer);
          const link = document.createElement('a'); link.href=w.model_url; link.textContent='下载 GLB'; link.download='weapon.glb'; box.append(link);
          const equip = document.createElement('button'); equip.textContent = w.equipped?'已标记装备':'标记装备（引擎适配待检）';
          equip.onclick = async () => { try { await request('/api/v1/weapons/'+w.id+'/equip',{method:'POST'}); equip.textContent='已标记装备'; } catch(e) { $('craftStatus').textContent=e.message; } }; box.append(equip);
        } catch(e) { $('craftStatus').textContent=e.message; }
      }; row.append(button);
    }
    $('craftTasks').append(row);
  }
  if (tasks.some(t=>['queued','running'].includes(t.status))) timer=setTimeout(()=>refresh().catch(showError),1500);
}
function showError(e) { $('craftStatus').textContent='后端请求失败：'+e.message; }
$('craftSubmit').onclick=async()=>{
  const button=$('craftSubmit'); button.disabled=true;
  try {
    const prompt=$('prompt').value.trim(); if(!prompt) throw new Error('请填写武器描述');
    const blob=await new Promise(resolve=>$('drawing').toBlob(resolve,'image/png'));
    const data=new FormData(); data.append('prompt',prompt); data.append('mode',$('craftMode').value); data.append('weapon_type',$('craftType').value); data.append('sketch',blob,'sketch.png');
    const task=await request('/api/v1/tasks',{method:'POST',body:data}); $('craftStatus').textContent='已保存任务 '+task.id+'，查询不会重新抽卡'; await refresh();
  } catch(e) { showError(e); } finally { button.disabled=false; }
};
$('craftRefresh').onclick=()=>refresh().catch(showError);
refresh().catch(showError);
