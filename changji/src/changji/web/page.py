"""Web 界面。

单页，不用打包工具。整个界面就是一个文件，改起来直接，
用户也不需要装 node。

分成五个标签页：环境、项目、剧本、分镜、设置。功能多了之后堆在一屏
会让人找不到东西，而这几件事本来就属于不同的工作阶段。

配色取自影视的色温语汇：冷调灰底配钨丝灯琥珀。
"""

from __future__ import annotations

import html

_PAGE = """<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>场记</title>
<style>
:root{
  --ground:#F1F3F6; --surface:#FAFBFD; --surface-2:#E9EDF2;
  --ink:#131820; --ink-2:#39424F; --muted:#5C6675; --faint:#8A94A3;
  --rule:#D9DFE7; --rule-strong:#BCC5D0;
  --accent:#A8630F; --pass:#2C6E52; --warn:#A8630F; --fail:#9C3730;
  --mono:ui-monospace,"Cascadia Mono",Consolas,monospace;
}
@media (prefers-color-scheme:dark){
  :root{
    --ground:#0F1218; --surface:#161A22; --surface-2:#1E242E;
    --ink:#E7EBF1; --ink-2:#C2CAD5; --muted:#8D97A6; --faint:#6B7585;
    --rule:#262D39; --rule-strong:#39434F;
    --accent:#D9942F; --pass:#5FB088; --warn:#D9942F; --fail:#D07068;
  }
}
*{box-sizing:border-box}
body{margin:0;background:var(--ground);color:var(--ink);
  font:15px/1.6 system-ui,-apple-system,"PingFang SC","Microsoft YaHei",sans-serif}
.wrap{max-width:1160px;margin:0 auto;padding:0 24px 80px}
header{border-bottom:2px solid var(--ink);padding:24px 0 0;margin-bottom:24px}
.top{display:flex;align-items:baseline;gap:16px;flex-wrap:wrap;margin-bottom:16px}
h1{font-size:21px;margin:0;letter-spacing:-.01em}
.sub{font-family:var(--mono);font-size:11px;letter-spacing:.14em;
  text-transform:uppercase;color:var(--accent)}
.tabs{display:flex;gap:2px;flex-wrap:wrap}
.tab{padding:9px 18px;cursor:pointer;border:none;background:transparent;
  color:var(--muted);font:inherit;border-bottom:2px solid transparent;
  margin-bottom:-2px}
.tab:hover{color:var(--ink)}
.tab.on{color:var(--ink);border-bottom-color:var(--accent);font-weight:600}
.tab .dot{display:inline-block;width:6px;height:6px;border-radius:50%;
  margin-left:6px;vertical-align:middle}
.tab .dot.ok{background:var(--pass)} .tab .dot.warn{background:var(--warn)}
.tab .dot.fail{background:var(--fail)}
.page{display:none} .page.on{display:block}
h2{font-size:15px;margin:0 0 12px;font-weight:600}
h3{font-size:13.5px;margin:20px 0 8px;font-weight:600;color:var(--ink-2)}
.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-bottom:12px}
input,select,button,textarea{font:inherit;padding:7px 11px;
  border:1px solid var(--rule-strong);border-radius:3px;
  background:var(--surface);color:var(--ink)}
input{min-width:220px}
input[type=number]{min-width:0;width:92px}
textarea{width:100%;resize:vertical;line-height:1.65;padding:10px 12px}
input:focus,select:focus,button:focus,textarea:focus{
  outline:2px solid var(--accent);outline-offset:1px}
button{cursor:pointer;background:var(--ink);color:var(--ground);
  border-color:var(--ink);font-weight:500}
button:hover:not(:disabled){opacity:.86}
button:disabled{opacity:.4;cursor:not-allowed}
button.ghost{background:transparent;color:var(--ink);
  border-color:var(--rule-strong);font-weight:400}
button.sm{padding:4px 10px;font-size:13px}
label.chk{display:flex;align-items:center;gap:6px;font-size:14px;color:var(--muted)}
label.chk input{min-width:0}
.fields{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));
  gap:12px 18px;margin-bottom:14px}
.field{display:flex;flex-direction:column;gap:4px}
.field label{font-size:12.5px;color:var(--muted)}
.field .hint{font-size:11.5px;color:var(--faint)}
.card{background:var(--surface);border:1px solid var(--rule);
  padding:16px 18px;margin-bottom:14px}
.checks{display:grid;gap:1px;background:var(--rule);border:1px solid var(--rule)}
.check{background:var(--surface);padding:11px 14px;display:grid;
  grid-template-columns:20px 92px 1fr;gap:10px;align-items:start;font-size:14px}
.check .sym{font-family:var(--mono);font-weight:700}
.ok .sym{color:var(--pass)} .warn .sym{color:var(--warn)} .fail .sym{color:var(--fail)}
.check .fix{grid-column:3;color:var(--muted);font-size:13px;white-space:pre-wrap;
  margin-top:4px;font-family:var(--mono)}
table{width:100%;border-collapse:collapse;font-size:14px}
th{text-align:left;font-family:var(--mono);font-size:10.5px;letter-spacing:.12em;
  text-transform:uppercase;color:var(--faint);font-weight:500;
  padding:0 12px 8px 0;border-bottom:1px solid var(--rule-strong)}
td{padding:9px 12px 9px 0;border-bottom:1px solid var(--rule);
  color:var(--ink-2);vertical-align:top}
tr.sel td{background:var(--surface-2)}
/* 点开编辑器的是单元格不是整行，勾选框那一格不该跟着变手型 */
tr[data-shot] td[onclick]{cursor:pointer}
td.num{font-family:var(--mono);font-variant-numeric:tabular-nums;white-space:nowrap}
.pill{display:inline-block;font-family:var(--mono);font-size:10.5px;
  padding:1px 7px;border:1px solid var(--rule-strong);border-radius:2px;
  white-space:nowrap}
.pill.final_done,.pill.locked{border-color:var(--pass);color:var(--pass)}
.pill.fallback,.pill.draft_rejected,.pill.final_rejected{
  border-color:var(--fail);color:var(--fail)}
.pill.draft_done,.pill.audio_done,.pill.frame_done{
  border-color:var(--accent);color:var(--accent)}
.bar{height:5px;background:var(--surface-2);overflow:hidden;margin:10px 0}
.bar i{display:block;height:100%;background:var(--accent);width:0;
  transition:width .3s ease}
.log{font-family:var(--mono);font-size:12px;line-height:1.55;max-height:260px;
  overflow-y:auto;background:var(--surface-2);padding:12px 14px;
  border:1px solid var(--rule);white-space:pre-wrap;word-break:break-word}
.log div{margin-bottom:2px}
.log .k-gate,.log .k-warn{color:var(--warn)}
.log .k-error{color:var(--fail)}
.log .k-shot_done,.log .k-done{color:var(--pass)}
.muted{color:var(--muted);font-size:13.5px}
.err{color:var(--fail);font-size:14px;margin-top:8px}
.ok-msg{color:var(--pass);font-size:13.5px;margin-top:8px}
.hide{display:none}
.notes{color:var(--fail);font-size:12.5px;margin-top:3px}
.stages{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0}
.stage-btn{padding:5px 12px;font-size:13px;border:1px solid var(--rule-strong);
  background:transparent;color:var(--muted);border-radius:3px;cursor:pointer}
.stage-btn.on{border-color:var(--accent);color:var(--accent);
  background:var(--surface)}
.asset{background:var(--surface);border:1px solid var(--rule);
  padding:14px 16px;margin-bottom:12px}
.asset h4{margin:0 0 10px;font-size:14.5px}
.asset .rendered{font-family:var(--mono);font-size:12px;color:var(--muted);
  background:var(--surface-2);padding:8px 10px;margin-top:10px;
  border-left:2px solid var(--accent);word-break:break-all}
.asset .out{font-size:13px;margin-top:8px}
.refs{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-top:12px}
.refslot{border:1px dashed var(--rule-strong);border-radius:3px;padding:8px;
  text-align:center;font-size:12px;color:var(--muted)}
.refslot img{width:100%;height:96px;object-fit:cover;background:var(--surface-2);
  border-radius:2px;display:block;margin-bottom:6px}
.refslot .empty{height:96px;display:flex;align-items:center;
  justify-content:center;background:var(--surface-2);border-radius:2px;
  margin-bottom:6px;color:var(--faint)}
.refslot input[type=file]{display:none}
.refslot label.pick{cursor:pointer;color:var(--accent);text-decoration:underline}
.preview{margin-bottom:14px}
.preview img,.preview video{width:100%;border:1px solid var(--rule);
  background:#000;display:block;border-radius:2px}
.preview .tag{font-family:var(--mono);font-size:10.5px;color:var(--faint);
  letter-spacing:.1em;text-transform:uppercase;margin:8px 0 4px}
.preview audio{width:100%;margin-top:6px}
.outcard{background:var(--surface);border:1px solid var(--rule);padding:14px 16px;
  margin-bottom:12px}
.outcard video{width:100%;max-width:420px;border:1px solid var(--rule);
  background:#000;display:block;margin-top:10px;border-radius:2px}
.outcard .meta{font-family:var(--mono);font-size:12px;color:var(--muted)}
/* 成片在容器里，用户要能拿下来。做成链接而不是按钮，
   这样右键另存为也能用。 */
a.dl{display:inline-block;padding:6px 14px;font-size:13px;text-decoration:none;
  border:1px solid var(--rule-strong);border-radius:3px;color:var(--ink)}
a.dl:hover{border-color:var(--accent);color:var(--accent)}
.split{display:grid;grid-template-columns:1fr 380px;gap:20px;align-items:start}
@media(max-width:900px){.split{grid-template-columns:1fr}}
@media(max-width:640px){
  .wrap{padding:0 16px 60px} input{min-width:0;flex:1}
  .check{grid-template-columns:20px 1fr} .check .fix{grid-column:2}
  .tab{padding:8px 12px;font-size:14px}
}
</style>
</head>
<body>
<div class="wrap">
<header>
  <div class="top">
    <h1>场记</h1>
    <span class="sub">AI 短剧生产流水线</span>
    <span class="muted" style="margin-left:auto" id="topinfo">ComfyUI __COMFY__</span>
  </div>
  <nav class="tabs">
    <button class="tab on" data-page="env">环境<span class="dot" id="envdot"></span></button>
    <button class="tab" data-page="proj">项目</button>
    <button class="tab" data-page="assets">角色场景</button>
    <button class="tab" data-page="script">剧本</button>
    <button class="tab" data-page="shots">分镜</button>
    <button class="tab" data-page="run">运行</button>
    <button class="tab" data-page="output">成片</button>
    <button class="tab" data-page="settings">参数</button>
  </nav>
</header>

<!-- 环境 -->
<section class="page on" id="page-env">
  <h2>环境体检</h2>
  <div id="checks" class="checks"><div class="check"><span class="sym">…</span>
    <span>体检中</span><span></span></div></div>
  <div class="row" style="margin-top:14px">
    <button class="ghost" onclick="loadDoctor()">重新体检</button>
  </div>
  <h3>连接的机器</h3>
  <div class="muted">显卡不在这台机器上也能用。改完会立刻重新体检，
    并写回配置文件，下次启动还是这套。</div>
  <div class="card">
    <div class="fields">
      <div class="field"><label>ComfyUI 地址</label>
        <input id="c_comfy" placeholder="http://127.0.0.1:8188">
        <span class="hint">局域网另一台有显卡的机器也行</span></div>
      <div class="field"><label>单镜超时（秒）</label>
        <input type="number" id="c_job" min="60" step="60"></div>
      <div class="field"><label>提交重试次数</label>
        <input type="number" id="c_retry" min="0" max="10"></div>
      <div class="field"><label>大模型地址</label>
        <input id="c_llm" placeholder="http://127.0.0.1:11434/v1">
        <span class="hint">任何兼容 OpenAI 接口的服务</span></div>
      <div class="field"><label>模型名</label>
        <input id="c_model" placeholder="qwen3:14b"></div>
      <div class="field"><label>api key</label>
        <input type="password" id="c_key" placeholder="不改就留空">
        <span class="hint" id="c_keyhint"></span></div>
      <div class="field"><label>温度</label>
        <input type="number" id="c_temp" min="0" max="2" step="0.1">
        <span class="hint">低了呆板，高了跑题。0.7 常用</span></div>
      <div class="field"><label>配音后端</label>
        <select id="c_ttsbk">
          <option value="comfy">走 ComfyUI 节点</option>
          <option value="http">独立 HTTP 服务</option>
        </select></div>
      <div class="field"><label>配音服务地址</label>
        <input id="c_ttsurl" placeholder="后端选 http 时必填"></div>
      <div class="field"><label>显存覆盖（GB）</label>
        <input type="number" id="c_vram" min="1" step="1">
        <span class="hint">ComfyUI 在别的机器上时本机探测不到，手填</span></div>
    </div>
    <div class="row" style="margin-top:12px">
      <button onclick="saveConns()">保存并重新体检</button>
      <button class="ghost" onclick="loadConns()">重新读取</button>
    </div>
    <div id="connout" class="out"></div>
    <div class="muted" id="connfile" style="margin-top:8px"></div>
  </div>
  <h3>硬件与画质档位</h3>
  <div id="hw" class="card muted">读取中</div>
</section>

<!-- 项目 -->
<section class="page" id="page-proj">
  <h2>打开或新建项目</h2>
  <div class="card">
    <div class="row">
      <input id="path" placeholder="项目目录，例如 D:\\短剧\\雨夜迷局" value="__PROJECT__">
      <button class="ghost" onclick="loadProject()">打开</button>
      <button class="ghost" onclick="loadProjectList()">刷新列表</button>
    </div>
    <div id="projlist"></div>
    <div class="row">
      <input id="newpath" placeholder="新建，起个名字就行">
      <select id="newstyle">
        <option value="realistic">真人写实</option>
        <option value="anime">动漫漫剧</option>
      </select>
      <button class="ghost" onclick="newProject()">新建</button>
    </div>
    <div id="projerr" class="err hide"></div>
  </div>
  <div id="projinfo" class="hide">
    <div class="card">
      <div id="projmeta"></div>
      <div class="row" style="margin:12px 0 0">
        <label class="chk">剧集
          <select id="ep"></select>
        </label>
        <button class="ghost sm" onclick="newEpisode()">新建一集</button>
        <button class="ghost sm" onclick="epAction('duplicate')">复制</button>
        <button class="ghost sm" onclick="epAction('rename')">改名</button>
        <button class="ghost sm" onclick="epAction('delete')">删除</button>
      </div>
      <div id="epout" class="muted"></div>
    </div>
    <h3>角色与场景</h3>
    <div id="assets" class="card muted"></div>
  </div>
</section>

<!-- 角色场景 -->
<section class="page" id="page-assets">
  <h2>角色与场景</h2>
  <div class="muted">这些是一致性的锚点。同一个角色在所有镜头里用的都是这段
    文字，逐字不变。改了会影响全剧，所以默认把已完成的镜头退回重跑。</div>
  <div class="row" style="margin-top:12px">
    <label class="chk"><input type="checkbox" id="a_reset" checked>
      改完把已完成的镜头退回重跑</label>
    <button class="ghost sm" onclick="loadAssets()">重新读取</button>
  </div>
  <div id="assetsbox" class="muted">先在项目页打开一个项目。</div>
</section>

<!-- 剧本 -->
<section class="page" id="page-script">
  <h2>剧本</h2>
  <div class="card">
    <h3 style="margin-top:0">让 AI 写一集</h3>
    <div class="muted">写完先放到下面的框里，你看过改过再保存。
      剧本是整条流水线的源头，源头没审过就往下跑，后面几十分钟全白跑。</div>
    <textarea id="premise" rows="3" style="margin-top:10px"
      placeholder="这一集想讲什么。一句话也行：深夜便利店，店员发现每晚同一时间进来的客人从没买过东西。"></textarea>
    <div class="row" style="margin-top:10px">
      <label class="chk"><input type="checkbox" id="wcont" checked> 接着前几集往下写</label>
      <label class="chk"><input type="checkbox" id="wchars" checked> 沿用已有角色</label>
      <button class="ghost" id="writebtn" onclick="writeScript()">写这一集</button>
    </div>
    <div class="row">
      <label class="chk">一口气写
        <input type="number" id="wcount" value="3" min="1" max="20"> 集</label>
      <button class="ghost" id="seriesbtn" onclick="writeSeries()">连着写</button>
      <button class="ghost" id="planallbtn" onclick="planAll()">给没分镜的都出分镜</button>
      <button class="ghost sm" onclick="stopSeries()">停</button>
    </div>
    <div class="muted">连着写会直接建出剧集并存进去，每一集拿前几集当上下文。
      写完可以逐集再改。写这一集只回一稿，不落库。</div>
    <div id="writeout" class="muted"></div>
  </div>
  <div class="card">
    <textarea id="script" rows="16" placeholder="把剧本粘在这里，或者用上面的 AI 写一稿。
角色对白用「名字：台词」的写法，程序会自动识别角色并生成设定。"></textarea>
    <div class="row" style="margin-top:12px">
      <label class="chk">目标时长
        <input type="number" id="dur" value="60" min="5" max="600" step="5"> 秒</label>
      <label class="chk"><input type="checkbox" id="rebible"> 重新生成角色设定</label>
      <button class="ghost" onclick="saveScript(false)">只保存</button>
      <button id="planbtn" onclick="saveScript(true)">保存并重出分镜</button>
    </div>
    <div class="muted">重出分镜会覆盖整张表，人工改过的镜头会丢。</div>
    <div id="planout" class="muted" style="margin-top:10px"></div>
  </div>
</section>

<!-- 分镜 -->
<section class="page" id="page-shots">
  <h2>分镜表</h2>
  <div class="row" id="batchbar">
    <label class="chk"><input type="checkbox" id="selall"> 全选</label>
    <span class="muted" id="selcount">未选</span>
    <button class="ghost sm" onclick="batchAct('reset')">重置去重跑</button>
    <button class="ghost sm" onclick="batchAct('lock')">锁定</button>
    <button class="ghost sm" onclick="batchAct('unlock')">解锁</button>
    <button class="ghost sm" onclick="batchAct('clear_notes')">清除闸门备注</button>
    <span id="batchout" class="muted"></span>
  </div>
  <div class="split">
    <div style="overflow-x:auto">
      <table>
        <thead><tr><th style="width:28px"></th><th>镜号</th><th>景别</th>
          <th>时长</th><th>台词</th><th>口型</th><th>状态</th></tr></thead>
        <tbody id="shots"></tbody>
      </table>
      <div id="shotsempty" class="muted">先在项目页打开一个项目。</div>
    </div>
    <div id="editor" class="card hide">
      <h3 style="margin-top:0" id="edtitle">编辑镜头</h3>
      <div id="ed_preview" class="preview hide"></div>
      <div class="field">
        <label>首帧提示词</label>
        <textarea id="ed_ffp" rows="3"></textarea>
        <span class="hint">描述环境、光线、构图。不要写角色长相，系统会自动拼接。</span>
      </div>
      <div class="field" style="margin-top:10px">
        <label>运动描述</label>
        <textarea id="ed_motion" rows="2"></textarea>
      </div>
      <div class="fields" style="margin-top:12px">
        <div class="field"><label>景别</label>
          <select id="ed_size"></select></div>
        <div class="field"><label>机位</label>
          <select id="ed_angle"></select></div>
        <div class="field"><label>运镜</label>
          <select id="ed_move"></select></div>
        <div class="field"><label>时长（秒）</label>
          <select id="ed_dur"></select></div>
      </div>
      <div class="field">
        <label>台词</label>
        <div id="ed_lines"></div>
        <span class="hint">改台词会清掉已有配音并重新锁定时长。</span>
      </div>
      <div class="field" style="margin-top:10px">
        <label>字幕</label>
        <input id="ed_sub" style="width:100%">
      </div>
      <div class="row" style="margin-top:14px">
        <label class="chk"><input type="checkbox" id="ed_lip"> 需要口型</label>
        <label class="chk"><input type="checkbox" id="ed_lock"> 锁定不再重跑</label>
      </div>
      <div class="row">
        <button onclick="saveShot()">保存</button>
        <button class="ghost" onclick="rerunShot()">保存并只重跑这一镜</button>
        <button class="ghost" onclick="closeEditor()">取消</button>
      </div>
      <div id="ederr" class="err hide"></div>
      <div id="edok" class="ok-msg hide"></div>
    </div>
  </div>
</section>

<!-- 运行 -->
<section class="page" id="page-run">
  <h2>运行</h2>
  <div class="card">
    <div class="muted">一键跑全流程，或只跑选中的阶段。</div>
    <div class="stages" id="stagebtns"></div>
    <div class="row">
      <label class="chk"><input type="checkbox" id="skipfinal" checked> 只跑草稿档</label>
      <label class="chk"><input type="checkbox" id="force"> 全部重做</label>
      <label class="chk"><input type="checkbox" id="allep"> 这个项目的每一集</label>
      <button id="runbtn" onclick="startRun()">开始</button>
      <button class="ghost" onclick="stopRun()">停止</button>
    </div>
    <div class="muted">草稿档验证叙事和构图，过闸门的镜头才升级成片档。中途停止不丢进度。<br>
      勾上「每一集」就一次排完整个项目，一集出错也接着跑后面几集。</div>
    <div id="runplan" class="muted" style="margin-top:8px"></div>
  </div>
  <div class="card">
    <div id="runline" class="muted">未开始</div>
    <div class="bar"><i id="bar"></i></div>
    <div id="runerr" class="err hide"></div>
    <div id="log" class="log"></div>
  </div>
</section>

<!-- 成片 -->
<section class="page" id="page-output">
  <h2>成片</h2>
  <div class="row">
    <button class="ghost" onclick="loadOutputs()">刷新</button>
  </div>
  <div id="outlist" class="muted">先在项目页打开一个项目。</div>
</section>

<!-- 参数 -->
<section class="page" id="page-settings">
  <h2>运行参数</h2>
  <div class="muted">默认只影响本次进程。想让改动重启后还在，
    在下面勾上写回配置文件。</div>
  <div class="card">
    <h3 style="margin-top:0">画质档位</h3>
    <div class="fields">
      <div class="field"><label>草稿档 宽</label>
        <input type="number" id="s_dw" step="32"><span class="hint">须为 32 的倍数</span></div>
      <div class="field"><label>草稿档 高</label>
        <input type="number" id="s_dh" step="32"></div>
      <div class="field"><label>草稿档 步数</label>
        <input type="number" id="s_ds" min="1" max="100"></div>
      <div class="field"><label>成片档 宽</label>
        <input type="number" id="s_fw" step="32"></div>
      <div class="field"><label>成片档 高</label>
        <input type="number" id="s_fh" step="32"></div>
      <div class="field"><label>成片档 步数</label>
        <input type="number" id="s_fs" min="1" max="100"></div>
    </div>
    <h3>装配</h3>
    <div class="fields">
      <div class="field"><label>帧率</label>
        <input type="number" id="s_fps" min="1" max="120"></div>
      <div class="field"><label>画质 crf</label>
        <input type="number" id="s_crf" min="0" max="51">
        <span class="hint">越小越清晰，18 到 23 常用</span></div>
      <div class="field"><label>字幕字体</label>
        <input id="s_font" style="min-width:0"></div>
      <div class="field"><label>字幕单行字数</label>
        <input type="number" id="s_line" min="6" max="30"></div>
      <div class="field"><label>字幕最多行数</label>
        <input type="number" id="s_lines" min="1" max="3"></div>
      <div class="field"><label>场景转场（秒）</label>
        <input type="number" id="s_trans" min="0" max="2" step="0.1">
        <span class="hint">只在换场处溶解，同场景内一律硬切</span></div>
    </div>
    <h3>配音</h3>
    <div class="fields">
      <div class="field"><label>时长容差（秒）</label>
        <input type="number" id="s_tol" min="0" max="2" step="0.05">
        <span class="hint">台词和镜头对不齐时允许的误差</span></div>
      <div class="field"><label>变速上限</label>
        <input type="number" id="s_tempo" min="0" max="0.2" step="0.01">
        <span class="hint">0.03 约等于 3%，再多人耳能听出来</span></div>
    </div>
    <h3>质量闸门</h3>
    <div class="fields">
      <div class="field"><label>单镜最多重试</label>
        <input type="number" id="s_retry" min="1" max="10"></div>
      <div class="field"><label>画面展布下限</label>
        <input type="number" id="s_std" min="0" max="60" step="1">
        <span class="hint">低于此判为纯色废片</span></div>
      <div class="field"><label>与首帧相似度下限</label>
        <input type="number" id="s_sim" min="0" max="1" step="0.05">
        <span class="hint">太低说明画面跑飞了，太高会拦住正常运镜</span></div>
      <div class="field"><label>台词落点最大偏差（秒）</label>
        <input type="number" id="s_drift" min="0.01" max="2" step="0.01"></div>
      <div class="field"><label>响度目标 LUFS</label>
        <input type="number" id="s_lufs" min="-40" max="0" step="0.5">
        <span class="hint">短视频平台常用 -16</span></div>
    </div>
    <div class="row" style="margin-top:8px">
      <label class="chk"><input type="checkbox" id="s_gates"> 启用质量闸门</label>
      <label class="chk"><input type="checkbox" id="s_fb">
        重试超限时降级为静帧加运镜</label>
    </div>
    <div class="muted" style="margin-top:4px">
      降级是为了保证整集能出片。关掉的话这一镜会直接失败，整集卡住。</div>
    <div class="row" style="margin-top:8px">
      <label class="chk"><input type="checkbox" id="s_persist">
        写回配置文件，重启后还在</label>
      <button onclick="saveSettings()">应用</button>
      <button class="ghost" onclick="loadSettings()">重新读取</button>
    </div>
    <div class="muted">画质档位不写回。它是按显存推出来的，
      刻进配置等于把这台机器的显存写死进项目，换台机器就不对了。</div>
    <div id="seterr" class="err hide"></div>
    <div id="setok" class="ok-msg hide"></div>
  </div>
</section>
</div>

<script>
const $ = id => document.getElementById(id);
const esc = s => String(s ?? "").replace(/[&<>"]/g,
  c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
let timer = null, currentShot = null, shotsCache = [];

const SIZES = ["ECU","CU","MCU","MS","MLS","LS","ELS"];
const ANGLES = ["low","eye_level","high","overhead","dutch"];
const MOVES = ["static","pan_left","pan_right","tilt_up","tilt_down",
               "push_in","pull_out","handheld","orbit"];
const DURS = [2,3,4,5];
const STAGES = [["audio","配音"],["frames","首帧"],["draft","草稿档"],
                ["final","成片档"],["assemble","装配"]];

async function api(url, opts) {
  const r = await fetch(url, opts);
  if (!r.ok) {
    let msg = r.statusText;
    try { const b = await r.json(); msg = b.detail || JSON.stringify(b); } catch (e) {}
    throw new Error(typeof msg === "string" ? msg : JSON.stringify(msg));
  }
  return r.json();
}
const post = (url, body) => api(url, {method:"POST",
  headers:{"Content-Type":"application/json"}, body:JSON.stringify(body)});
const show = (el, msg, cls) => { el.className = cls; el.textContent = msg;
  el.classList.remove("hide"); };
const hide = el => el.classList.add("hide");

// ---- 标签页 ----
document.querySelectorAll(".tab").forEach(t => t.onclick = () => {
  document.querySelectorAll(".tab").forEach(x => x.classList.remove("on"));
  document.querySelectorAll(".page").forEach(x => x.classList.remove("on"));
  t.classList.add("on");
  $("page-" + t.dataset.page).classList.add("on");
  if (t.dataset.page === "settings") loadSettings();
  if (t.dataset.page === "shots") loadShots();
  if (t.dataset.page === "script") loadScript();
  if (t.dataset.page === "output") loadOutputs();
  if (t.dataset.page === "assets") loadAssets();
});

// ---- 环境 ----
let connSnapshot = null;

function renderChecks(checks) {
  $("checks").innerHTML = checks.map(c => `
    <div class="check ${c.level}">
      <span class="sym">${c.level==="ok"?"✓":c.level==="warn"?"!":"✗"}</span>
      <span>${esc(c.name)}</span><span>${esc(c.detail)}</span>
      ${c.fix ? `<span class="fix">${esc(c.fix)}</span>` : ""}
    </div>`).join("");
  const worst = checks.some(c=>c.level==="fail") ? "fail"
              : checks.some(c=>c.level==="warn") ? "warn" : "ok";
  $("envdot").className = "dot " + worst;
}

async function loadConns() {
  try {
    const d = await api("/api/connections");
    connSnapshot = d;
    $("c_comfy").value = d.comfy_base_url;
    $("c_job").value = d.comfy_job_timeout_s;
    $("c_retry").value = d.comfy_max_retries;
    $("c_llm").value = d.llm_base_url;
    $("c_model").value = d.llm_model;
    $("c_temp").value = d.llm_temperature;
    $("c_ttsbk").value = d.tts_backend;
    $("c_ttsurl").value = d.tts_base_url;
    $("c_vram").value = d.vram_gb_override ?? "";
    $("c_key").value = "";
    $("c_keyhint").textContent = d.llm_api_key_set
      ? `已设置 ${d.llm_api_key_hint}，留空表示不动` : "未设置";
    $("connfile").textContent = "配置文件：" + d.config_file;
    // 环境变量优先级最高。被顶住的字段改了也白改，直接标在旁边。
    const LOCK = {comfy_base_url:"c_comfy", llm_base_url:"c_llm",
                  llm_model:"c_model", llm_api_key:"c_key",
                  tts_base_url:"c_ttsurl", vram_gb_override:"c_vram"};
    Object.values(LOCK).forEach(id => {
      const w = $(id).parentElement.querySelector(".envlock");
      if (w) w.remove();
    });
    Object.entries(d.env_locked || {}).forEach(([field, envname]) => {
      const id = LOCK[field]; if (!id) return;
      const tag = document.createElement("span");
      tag.className = "hint envlock";
      tag.textContent = `被环境变量 ${envname} 顶着，改了重启就回去`;
      tag.style.color = "var(--warn)";
      $(id).parentElement.appendChild(tag);
    });
  } catch (e) { show($("connout"), "读取失败：" + e.message, "err"); }
}

async function saveConns() {
  const out = $("connout");
  const num = id => { const v = parseFloat($(id).value); return isNaN(v)?null:v; };
  const body = {
    comfy_base_url: $("c_comfy").value.trim() || null,
    comfy_job_timeout_s: num("c_job"),
    comfy_max_retries: num("c_retry"),
    llm_base_url: $("c_llm").value.trim() || null,
    llm_model: $("c_model").value.trim() || null,
    llm_temperature: num("c_temp"),
    tts_backend: $("c_ttsbk").value,
    tts_base_url: $("c_ttsurl").value.trim(),
    vram_gb_override: num("c_vram"),
  };
  // api key 单独处理：留空是「别动」，不是「清空」。
  const key = $("c_key").value;
  if (key) body.llm_api_key = key;

  if (connSnapshot) {
    const cur = {
      comfy_base_url: connSnapshot.comfy_base_url,
      comfy_job_timeout_s: connSnapshot.comfy_job_timeout_s,
      comfy_max_retries: connSnapshot.comfy_max_retries,
      llm_base_url: connSnapshot.llm_base_url,
      llm_model: connSnapshot.llm_model,
      llm_temperature: connSnapshot.llm_temperature,
      tts_backend: connSnapshot.tts_backend,
      tts_base_url: connSnapshot.tts_base_url,
      vram_gb_override: connSnapshot.vram_gb_override,
    };
    Object.keys(cur).forEach(k => {
      if (k in body && String(body[k] ?? "") === String(cur[k] ?? "")) delete body[k];
    });
  }
  Object.keys(body).forEach(k => body[k] === null && delete body[k]);
  if (!Object.keys(body).length) {
    show(out, "没有改动", "muted"); return;
  }

  show(out, "正在重新体检…", "muted");
  try {
    const d = await post("/api/connections", {patch: body, persist: true});
    renderChecks(d.checks);
    const where = d.saved_to ? `，已写入 ${d.saved_to}` : "";
    const locked = (d.env_locked || []).length
      ? `注意：${d.env_locked.join("、")} 被环境变量顶着，重启会退回去。` : "";
    show(out, `改了 ${d.changed.length} 项${where}。` +
      (d.can_run ? "体检通过。" : "体检没过，看上面的红项。") + locked,
      locked || !d.can_run ? "err" : "ok-msg");
    await loadConns();
  } catch (e) { show(out, e.message, "err"); }
}

async function loadDoctor() {
  try {
    const d = await api("/api/doctor");
    renderChecks(d.checks);
  } catch (e) {
    $("checks").innerHTML = `<div class="check fail"><span class="sym">✗</span>
      <span>体检</span><span>${esc(e.message)}</span></div>`;
    $("envdot").className = "dot fail";
  }
  try {
    const h = await api("/api/hardware");
    const rows = Object.entries(h.tiers).map(([k,v]) =>
      `${k}: ${v.width}x${v.height} ${v.steps} 步` +
      (v.seconds ? `，单镜约 ${Math.round(v.seconds)} 秒` : "")).join("<br>");
    $("hw").innerHTML = `${esc(h.gpu || "未探测到显卡")}，显存 ${h.vram_gb} GB<br>${rows}`;
  } catch (e) { $("hw").textContent = "读取失败：" + e.message; }
}

// ---- 项目 ----
async function newProject() {
  const path = $("newpath").value.trim();
  if (!path) { alert("给项目起个名字"); return; }
  try {
    const d = await post("/api/new", {path, style_line: $("newstyle").value});
    $("path").value = d.root; $("newpath").value = "";
    await loadProjectList();
    loadProject();
  } catch (e) { show($("projerr"), "新建失败：" + e.message, "err"); }
}

// 项目库里有哪些项目。以前只能手打绝对路径，在容器里跑的时候
// 那是 /data/projects/剧名，用户根本不知道该填什么。
async function loadProjectList() {
  const box = $("projlist");
  try {
    const d = await api("/api/projects");
    if (!d.projects.length) {
      box.innerHTML = `<div class="muted">项目库 ${esc(d.workspace)} 还是空的，`
        + `在下面起个名字新建一个。</div>`;
      return;
    }
    box.innerHTML = `<div class="muted" style="margin-bottom:8px">`
      + `项目库 ${esc(d.workspace)}，共 ${d.projects.length} 个</div>`
      + `<table><thead><tr><th>项目</th><th>风格</th><th>剧集</th>`
      + `<th>镜头</th><th>成片</th><th></th></tr></thead><tbody>`
      + d.projects.map(p => p.broken
        ? `<tr><td>${esc(p.name)}</td><td colspan="4" class="err">`
          + `${esc(p.broken)}</td><td></td></tr>`
        : `<tr>
             <td>${esc(p.name)}</td>
             <td>${p.style_line === "anime" ? "动漫" : "写实"}</td>
             <td class="num">${p.episodes}</td>
             <td class="num">${p.done_shots}/${p.shots}</td>
             <td class="num">${p.outputs || "—"}</td>
             <td><button class="ghost sm"
                   onclick="openProject('${esc(p.path)}')">打开</button>
                 <button class="ghost sm"
                   onclick="delProject('${esc(p.path)}','${esc(p.name)}','${esc(p.dir)}')"
                   >删除</button></td>
           </tr>`).join("") + `</tbody></table>`;
  } catch (e) { box.innerHTML = `<div class="err">读不到项目库：${esc(e.message)}</div>`; }
}

function openProject(path) {
  $("path").value = path;
  loadProject();
}

// 删项目是整个目录连素材带成片一起没。让用户把目录名重打一遍，
// 一次误点删掉跑了一夜的成片是不能接受的。
async function delProject(path, name, dir) {
  const typed = prompt(
    `删掉「${name}」会连同它的分镜、素材和成片一起删掉，删了找不回来。
`
    + `确定的话把目录名打一遍：${dir}`);
  if (typed === null) return;
  try {
    await post("/api/project/delete", {path, confirm_name: typed.trim()});
    if ($("path").value === path) { $("path").value = ""; $("projinfo").classList.add("hide"); }
    hide($("projerr"));
    loadProjectList();
  } catch (e) { show($("projerr"), "删不掉：" + e.message, "err"); }
}

async function newEpisode() {
  const title = prompt("这一集叫什么？（可留空）");
  if (title === null) return;
  try {
    const d = await post("/api/episode", {
      project: $("path").value.trim(), title: title || "",
      target_duration_s: parseFloat($("dur").value) || 60});
    $("epout").textContent = `已新建 ${d.episode_id}`;
    await loadProject();
    $("ep").value = d.episode_id;
    $("ep").dispatchEvent(new Event("change"));
  } catch (e) { $("epout").innerHTML = `<span class="err">${esc(e.message)}</span>`; }
}

async function epAction(action) {
  const ep = $("ep").value;
  if (!ep) return;
  const body = {project: $("path").value.trim(), episode_id: ep, action};
  if (action === "rename") {
    const t = prompt("改成什么名字？");
    if (t === null) return;
    body.new_title = t;
  } else if (action === "delete") {
    if (!confirm(`确定删除 ${ep}？分镜表会一起没掉，产出的文件保留在磁盘上。`)) return;
  } else if (action === "duplicate") {
    if (!confirm(`复制 ${ep}？只带走剧本和分镜文案，产出物和状态不带。`)) return;
  }
  try {
    const d = await post("/api/episode/action", body);
    $("epout").textContent = action === "duplicate"
      ? `已复制为 ${d.episode_id}，${d.shots} 个镜头待重跑`
      : action === "delete" ? `已删除 ${d.deleted}` : `已改名为「${d.title}」`;
    await loadProject();
    if (d.episode_id) { $("ep").value = d.episode_id;
                        $("ep").dispatchEvent(new Event("change")); }
  } catch (e) { $("epout").innerHTML = `<span class="err">${esc(e.message)}</span>`; }
}

async function loadProject() {
  const path = $("path").value.trim();
  if (!path) return;
  try {
    const p = await api("/api/project?path=" + encodeURIComponent(path));
    hide($("projerr"));
    $("projinfo").classList.remove("hide");
    $("projmeta").innerHTML =
      `<strong>${esc(p.title)}</strong> <span class="muted">（${esc(p.project_id)}，`
      + `${p.style_line === "anime" ? "动漫" : "真人写实"}）</span><br>`
      + `<span class="muted">${esc(p.root)}</span>`
      + (p.premise ? `<div class="muted" style="margin-top:8px">`
          + `讲的是：${esc(p.premise)}</div>` : "");
    // 梗概回填到剧本页。隔天想接着写第六集，不用凭记忆把当初那句话重打。
    if (p.premise && !$("premise").value.trim()) $("premise").value = p.premise;
    $("assets").innerHTML =
      `角色：${p.characters.map(c=>esc(c.name)).join("、") || "（无）"}<br>`
      + `场景：${p.locations.map(l=>esc(l.name)).join("、") || "（无）"}`;
    $("ep").innerHTML = p.episodes.map(e =>
      `<option value="${esc(e.episode_id)}">${esc(e.episode_id)}`
      + ` ${esc(e.title||"")} · ${e.shots} 镜 · ${e.duration_s} 秒</option>`).join("");
    // 这一集讲什么，选中哪一集就显示哪一集的
    const cur = p.episodes.find(e => e.episode_id === $("ep").value)
      || p.episodes[0];
    $("epout").textContent = cur && cur.synopsis ? cur.synopsis : "";
    localStorage.setItem("changji.path", path);
    if (p.episodes.length) { loadShots(); loadScript(); }
  } catch (e) {
    $("projinfo").classList.add("hide");
    show($("projerr"), "打开失败：" + e.message, "err");
  }
}

// ---- 剧本 ----
async function loadScript() {
  const path = $("path").value.trim(), ep = $("ep").value;
  if (!path || !ep) return;
  try {
    const d = await api(`/api/script?path=${encodeURIComponent(path)}`
      + `&episode_id=${encodeURIComponent(ep)}`);
    $("script").value = d.script || "";
    if (d.target_duration_s) $("dur").value = d.target_duration_s;
  } catch (e) {}
}

// 写剧本要跑十几秒到一分钟，按钮得当场变样，不然人会连点好几下。
async function writeScript() {
  const path = $("path").value.trim();
  if (!path) { show($("writeout"), "先在项目页打开一个项目。", "err"); return; }
  const premise = $("premise").value.trim();
  if (!premise) { show($("writeout"), "先说说这一集要讲什么。", "err"); return; }
  const btn = $("writebtn");
  btn.disabled = true;
  show($("writeout"), "正在写，大模型这一步要十几秒到一分钟…", "muted");
  try {
    const d = await post("/api/script/write", {
      project: path, episode_id: $("ep").value,
      premise, duration_s: parseFloat($("dur").value) || 60,
      continue_from_previous: $("wcont").checked,
      reuse_characters: $("wchars").checked});
    // 直接覆盖下面的框。写的是一稿，人接着在框里改。
    $("script").value = d.script;
    const bits = [
      `《${d.title || "无题"}》`,
      `${d.beats} 拍，对白 ${d.dialogue_chars} 字（这个时长大约装 ${d.budget_chars} 字，${d.fit}）`,
    ];
    if (d.speakers.length) bits.push("出场：" + d.speakers.join("、"));
    if (d.continued_from) bits.push("接着前几集写的");
    show($("writeout"), bits.join(" · ") + (d.logline ? "。" + d.logline : ""),
         d.fit === "合适" ? "ok-msg" : "muted");
  } catch (e) {
    show($("writeout"), "没写出来：" + e.message, "err");
  } finally { btn.disabled = false; }
}

let seriesTimer = null;

// 连着写好几集。一集一集手点写、手点新建，写到第五集人就放弃了，
// 量产也就无从谈起。
async function writeSeries() {
  const path = $("path").value.trim();
  if (!path) { show($("writeout"), "先在项目页打开一个项目。", "err"); return; }
  const premise = $("premise").value.trim();
  if (!premise) { show($("writeout"), "先说说这几集要讲什么。", "err"); return; }
  const n = parseInt($("wcount").value, 10) || 3;
  if (!confirm(`会新建 ${n} 集并把剧本存进去，现有的几集不动。继续？`)) return;
  try {
    await post("/api/script/series", {
      project: path, premise, episodes: n,
      duration_s: parseFloat($("dur").value) || 60,
      reuse_characters: $("wchars").checked});
    pollSeries();
  } catch (e) { show($("writeout"), e.message, "err"); }
}

// 连着写完五集之后每集还得单独点一次重出分镜，漏掉一次那集就跑不了。
async function planAll() {
  const path = $("path").value.trim();
  if (!path) { show($("writeout"), "先在项目页打开一个项目。", "err"); return; }
  try {
    await post("/api/plan/all", {project: path, overwrite: false});
    pollSeries();
  } catch (e) { show($("writeout"), e.message, "err"); }
}

async function stopSeries() {
  try { await post("/api/script/series/stop", {}); } catch (e) {}
}

async function pollSeries() {
  if (seriesTimer) clearTimeout(seriesTimer);
  let s;
  try { s = await api("/api/script/series"); }
  catch (e) { seriesTimer = setTimeout(pollSeries, 4000); return; }
  $("seriesbtn").disabled = s.running;
  $("planallbtn").disabled = s.running;
  const ok = s.episodes.filter(e => !e.error);
  const bad = s.episodes.filter(e => e.error);
  const lines = [];
  if (s.running) lines.push(`${s.message}（${s.done}/${s.total}）`);
  else if (s.total) lines.push(s.message || `写完了 ${s.done} 集`);
  ok.forEach(e => lines.push(
    e.shots !== undefined
      ? `${e.episode_id} ${e.shots} 个镜头，${e.duration_s} 秒`
      : `${e.episode_id} 《${e.title || "无题"}》 对白 ${e.dialogue_chars} 字`
        + (e.speakers && e.speakers.length ? " · " + e.speakers.join("、") : "")));
  bad.forEach(e => lines.push("有一集没写出来：" + e.error));
  // 换行用常量拼，别直接写转义序列：这段 JS 住在 Python 的三引号
  // 字符串里，反斜杠先被 Python 吃一道，稍不留神就变成真的换行，
  // 字符串没闭合，整个页面的脚本一行都跑不起来。
  const NL = String.fromCharCode(10);
  if (lines.length) {
    show($("writeout"), lines.join(NL), s.error ? "err" : "muted");
    $("writeout").style.whiteSpace = "pre-wrap";
  }
  if (s.error && !s.running) show($("writeout"),
    lines.join(NL) + NL + s.error, "err");
  if (s.running) seriesTimer = setTimeout(pollSeries, 3000);
  else if (s.total) { loadProject(); }
}

async function saveScript(regenerate) {
  const script = $("script").value.trim();
  if (!script) { alert("剧本是空的"); return; }
  const btn = $("planbtn"); btn.disabled = true;
  $("planout").textContent = regenerate
    ? "正在重出分镜，要跑大模型，请稍候。" : "保存中。";
  try {
    const hasAssets = $("assets").textContent.includes("角色：") &&
                      !$("assets").textContent.includes("角色：（无）");
    let d;
    if (regenerate && (!hasAssets || $("rebible").checked)) {
      // 没有角色设定或要求重生成，走完整的两阶段
      d = await post("/api/plan", {
        project: $("path").value.trim(), script,
        episode_id: $("ep").value || "ep01",
        duration_s: parseFloat($("dur").value) || 60,
        regenerate_bible: $("rebible").checked});
      $("planout").innerHTML =
        `<strong>${d.shots} 个镜头，${d.duration_s} 秒，其中 ${d.lipsync} 个需要口型</strong>`;
    } else {
      d = await post("/api/script", {
        project: $("path").value.trim(), episode_id: $("ep").value,
        script, regenerate,
        duration_s: parseFloat($("dur").value) || null});
      $("planout").innerHTML = d.regenerated
        ? `<strong>已重出 ${d.shots} 个镜头</strong>` : "已保存。";
    }
    loadProject();
  } catch (e) {
    $("planout").innerHTML = `<span class="err">${esc(e.message)}</span>`;
  } finally { btn.disabled = false; }
}

// ---- 分镜 ----
async function loadShots() {
  const path = $("path").value.trim(), ep = $("ep").value;
  if (!path || !ep) return;
  try {
    const d = await api(`/api/shots?path=${encodeURIComponent(path)}`
      + `&episode_id=${encodeURIComponent(ep)}`);
    shotsCache = d.shots;
    $("shotsempty").classList.add("hide");
    $("shots").innerHTML = d.shots.map(s => `
      <tr data-shot="${esc(s.shot_id)}"
          class="${currentShot===s.shot_id?"sel":""}">
        <td>
          <input type="checkbox" class="pick" value="${esc(s.shot_id)}"
                 style="min-width:0"></td>
        <td class="num" onclick="openEditor('${esc(s.shot_id)}')">${esc(s.shot_id.split("_").pop())}</td>
        <td onclick="openEditor('${esc(s.shot_id)}')">${esc(s.shot_size)}</td>
        <td onclick="openEditor('${esc(s.shot_id)}')" class="num">${s.duration_s}s${s.duration_locked?" 锁":""}</td>
        <td onclick="openEditor('${esc(s.shot_id)}')">${s.dialogue.map(x=>esc(x.text)).join("<br>") || "—"}</td>
        <td onclick="openEditor('${esc(s.shot_id)}')">${s.needs_lipsync?"是":"—"}</td>
        <td><span class="pill ${esc(s.status)}">${esc(s.status)}</span>
          ${s.attempts?`<span class="muted"> ${s.attempts} 次</span>`:""}
          ${s.gate_notes.length
            ? `<div class="notes">${esc(s.gate_notes.join("；"))}</div>`:""}</td>
      </tr>`).join("");
    document.querySelectorAll(".pick").forEach(c => c.onchange = refreshSel);
    refreshSel();
  } catch (e) {}
}

function picked() {
  return [...document.querySelectorAll(".pick:checked")].map(c => c.value);
}

function refreshSel() {
  const n = picked().length;
  $("selcount").textContent = n ? `已选 ${n} 个` : "未选，操作将作用于整集";
}

async function batchAct(action) {
  const ids = picked();
  const labels = {reset:"重置去重跑", lock:"锁定", unlock:"解锁",
                  clear_notes:"清除闸门备注"};
  const scope = ids.length ? `选中的 ${ids.length} 个镜头` : "整集所有镜头";
  if (!confirm(`确定要对${scope}执行「${labels[action]}」吗？`)) return;
  try {
    const d = await post("/api/shots/batch", {
      project: $("path").value.trim(), episode_id: $("ep").value,
      shot_ids: ids, action});
    let msg = `${labels[action]}：改动 ${d.changed} / ${d.total} 个`;
    if (d.skipped_locked) msg += `，跳过 ${d.skipped_locked} 个已锁定的`;
    $("batchout").textContent = msg;
    $("selall").checked = false;
    await loadShots();
  } catch (e) { $("batchout").innerHTML = `<span class="err">${esc(e.message)}</span>`; }
}

function fillSelect(el, values, cur, labels) {
  el.innerHTML = values.map((v,i) =>
    `<option value="${esc(v)}"${String(v)===String(cur)?" selected":""}>`
    + `${esc(labels?labels[i]:v)}</option>`).join("");
}

function openEditor(shotId) {
  const s = shotsCache.find(x => x.shot_id === shotId);
  if (!s) return;
  currentShot = shotId;
  loadShots();
  $("editor").classList.remove("hide");
  $("edtitle").textContent = "编辑 " + shotId.split("_").pop();
  $("ed_ffp").value = s.first_frame_prompt || "";
  $("ed_motion").value = s.motion_prompt || "";
  fillSelect($("ed_size"), SIZES, s.shot_size);
  fillSelect($("ed_angle"), ANGLES, s.camera_angle);
  fillSelect($("ed_move"), MOVES, s.camera_move);
  fillSelect($("ed_dur"), DURS, s.duration_s);
  $("ed_sub").value = s.subtitle_text || "";
  $("ed_lip").checked = !!s.needs_lipsync;
  $("ed_lock").checked = s.status === "locked";
  renderPreview(s);
  $("ed_lines").innerHTML = s.dialogue.length
    ? s.dialogue.map((l,i) =>
        `<input class="edline" data-i="${i}" style="width:100%;margin-bottom:6px"
                value="${esc(l.text)}">`).join("")
    : '<span class="muted">这一镜没有台词</span>';
  hide($("ederr")); hide($("edok"));
}

function mediaUrl(rel) {
  return `/api/media?path=${encodeURIComponent($("path").value.trim())}`
    + `&rel=${encodeURIComponent(rel)}`;
}

function renderPreview(s) {
  const box = $("ed_preview");
  const parts = [];
  if (s.video_path) {
    parts.push(`<div class="tag">草稿视频</div>`);
    parts.push(`<video controls preload="metadata" src="${mediaUrl(s.video_path)}"></video>`);
  } else if (s.frame_path) {
    parts.push(`<div class="tag">首帧</div>`);
    parts.push(`<img src="${mediaUrl(s.frame_path)}" alt="首帧">`);
  }
  if (s.frame_path && s.video_path) {
    parts.push(`<div class="tag">首帧</div>`);
    parts.push(`<img src="${mediaUrl(s.frame_path)}" alt="首帧">`);
  }
  (s.audio_paths || []).forEach((a, i) => {
    parts.push(`<div class="tag">配音 ${i + 1}</div>`);
    parts.push(`<audio controls preload="none" src="${mediaUrl(a)}"></audio>`);
  });
  if (!parts.length) {
    box.classList.add("hide");
    return;
  }
  box.innerHTML = parts.join("");
  box.classList.remove("hide");
}

// 服务端有哪些参考音色。手打一条路径的话，要跑到配音那一步
// 才会因为节点校验不过而报错，所以这里只让人从列表里选。
let voiceList = [];

function voiceOptions(selected) {
  const opts = ['<option value="">自动（按性别挑）</option>'];
  const pool = voiceList.slice();
  if (selected && !pool.includes(selected)) pool.unshift(selected);
  pool.forEach(v => {
    // 不用正则：这段 JS 住在 Python 的三引号字符串里，
    // 反斜杠会被 Python 先当成转义序列，留下一个语法警告。
    let label = v.startsWith("voices_examples/") ? v.slice(16) : v;
    if (label.toLowerCase().endsWith(".wav")) label = label.slice(0, -4);
    opts.push(`<option value="${esc(v)}"${v === selected ? " selected" : ""}>`
              + `${esc(label)}</option>`);
  });
  return opts.join("");
}

async function loadVoices(path) {
  try {
    const d = await api("/api/voices?path=" + encodeURIComponent(path));
    voiceList = d.voices || [];
  } catch (e) { voiceList = []; }
}

async function loadAssets() {
  const path = $("path").value.trim();
  if (!path) return;
  try {
    await loadVoices(path);
    const d = await api("/api/assets?path=" + encodeURIComponent(path));
    // 参考图这条路通不通，得当场说清楚，不能让人传了图以为生效了
    const refsUsed = d.reference_images_used;
    const refsHint = d.reference_hint || "";
    const chars = d.characters.map(c => `
      <div class="asset" data-char="${esc(c.char_id)}">
        <h4>${esc(c.name)} <span class="muted">${esc(c.char_id)}</span></h4>
        <div class="fields">
          <div class="field"><label>称呼</label>
            <input class="cf" data-k="name" value="${esc(c.name)}"></div>
          <div class="field"><label>参考音色</label>
            <select class="cf" data-k="voice_id">${voiceOptions(c.voice_id)}</select>
            <span class="hint">${c.voice_id ? "已指定"
              : "自动：按" + ({female:"女声", male:"男声"}[c.voice_gender]
                             || "轮换") + "挑"}</span></div>
        </div>
        <div class="field"><label>身份（性别、年龄、气质）</label>
          <input class="cf" data-k="identity" value="${esc(c.identity)}"></div>
        <div class="field" style="margin-top:8px"><label>体型</label>
          <input class="cf" data-k="body" value="${esc(c.body)}"></div>
        <div class="field" style="margin-top:8px"><label>面部（发型、发色、五官）</label>
          <input class="cf" data-k="face" value="${esc(c.face)}"></div>
        <div class="field" style="margin-top:8px"><label>默认服装</label>
          <input class="cf" data-k="attire" value="${esc(c.attire)}"></div>
        <div class="rendered">${esc(c.rendered)}</div>
        <div class="refs">
          ${refSlot(c, "front", "正面")}
          ${refSlot(c, "three_quarter", "四分之三侧")}
          ${refSlot(c, "back", "背面")}
        </div>
        <div class="${refsUsed ? "muted" : "err"}" style="font-size:12px">
          ${refsUsed
            ? "有参考图就照着画，没有就只按上面那段文字想象。机位不同会自动挑用哪一张。"
            : esc(refsHint)}</div>
        <div class="row" style="margin:10px 0 0">
          <button class="sm" onclick="saveChar('${esc(c.char_id)}')">保存</button>
        </div>
        <div class="out"></div>
      </div>`).join("");
    const locs = d.locations.map(l => `
      <div class="asset" data-loc="${esc(l.location_id)}">
        <h4>${esc(l.name)} <span class="muted">${esc(l.location_id)}</span></h4>
        <div class="field"><label>名称</label>
          <input class="lf" data-k="name" value="${esc(l.name)}"></div>
        <div class="field" style="margin-top:8px"><label>空间与布景</label>
          <input class="lf" data-k="space" value="${esc(l.space)}"></div>
        <div class="field" style="margin-top:8px"><label>光线基调</label>
          <input class="lf" data-k="lighting" value="${esc(l.lighting)}"></div>
        <div class="field" style="margin-top:8px"><label>色彩方案</label>
          <input class="lf" data-k="palette" value="${esc(l.palette||"")}"></div>
        <div class="rendered">${esc(l.rendered)}</div>
        <div class="row" style="margin:10px 0 0">
          <button class="sm" onclick="saveLoc('${esc(l.location_id)}')">保存</button>
        </div>
        <div class="out"></div>
      </div>`).join("");
    $("assetsbox").innerHTML =
      `<h3>角色 ${d.characters.length} 个</h3>${chars || '<div class="muted">还没有角色</div>'}`
      + `<h3>场景 ${d.locations.length} 个</h3>${locs || '<div class="muted">还没有场景</div>'}`
      + `<h3>全剧风格</h3>
         <div class="asset" id="stylebox">
           <div class="field"><label>画风与质感（所有镜头都会带上）</label>
             <input class="sf" data-k="global_style" value="${esc(d.style.global_style)}"></div>
           <div class="field" style="margin-top:8px"><label>负向提示词</label>
             <input class="sf" data-k="negative_prompt" value="${esc(d.style.negative_prompt)}"></div>
           <div class="field" style="margin-top:8px"><label>画幅</label>
             <select class="sf" data-k="aspect_ratio">
               ${["9:16","16:9","1:1"].map(r=>`<option${r===d.style.aspect_ratio?" selected":""}>${r}</option>`).join("")}
             </select></div>
           <div class="row" style="margin:10px 0 0">
             <button class="sm" onclick="saveStyle()">保存</button></div>
           <div class="out"></div>
         </div>`;
  } catch (e) {
    $("assetsbox").innerHTML = `<span class="err">${esc(e.message)}</span>`;
  }
}

function collect(sel, root) {
  const patch = {};
  (root || document).querySelectorAll(sel).forEach(i => patch[i.dataset.k] = i.value);
  return patch;
}

function reportAsset(box, d) {
  box.innerHTML = d.reset_shots
    ? `<span class="ok-msg">已保存。${d.reset_shots} 个镜头退回重跑。</span>`
    : `<span class="ok-msg">已保存。</span>`;
}

// 参考图是一致性最硬的手段：文字描述再细，模型每次也会重新想象
// 一遍这张脸；给一张图，它就照着画。
function refSlot(c, slot, label) {
  const rel = c["ref_" + slot];
  const id = c.char_id + "_" + slot;
  const body = rel
    ? `<img src="${mediaUrl(rel)}?v=${Date.now()}" alt="${esc(label)}">`
    : `<div class="empty">没有</div>`;
  const action = rel
    ? `<a href="#" onclick="clearRef('${esc(c.char_id)}','${slot}');`
      + `return false">撤掉</a>`
    : "";
  return `<div class="refslot">
    ${body}
    <div>${esc(label)}</div>
    <label class="pick">选图
      <input type="file" accept="image/png,image/jpeg,image/webp"
             onchange="uploadRef(this,'${esc(c.char_id)}','${slot}')"></label>
    ${action}
  </div>`;
}

async function uploadRef(input, charId, slot) {
  const file = input.files && input.files[0];
  if (!file) return;
  const card = document.querySelector(`[data-char="${charId}"]`);
  const out = card.querySelector(".out");
  const fd = new FormData();
  fd.append("project", $("path").value.trim());
  fd.append("char_id", charId);
  fd.append("slot", slot);
  fd.append("file", file);
  show(out, "正在上传…", "muted");
  try {
    const r = await fetch("/api/character/reference", {method: "POST", body: fd});
    if (!r.ok) {
      let msg = r.statusText;
      try { msg = (await r.json()).detail || msg; } catch (e) {}
      throw new Error(typeof msg === "string" ? msg : JSON.stringify(msg));
    }
    const d = await r.json();
    reportAsset(out, d);
    loadAssets();
    loadShots();
  } catch (e) {
    show(out, "传不上去：" + e.message, "err");
  } finally { input.value = ""; }
}

async function clearRef(charId, slot) {
  const card = document.querySelector(`[data-char="${charId}"]`);
  const out = card.querySelector(".out");
  try {
    const d = await post("/api/character/reference/clear", {
      project: $("path").value.trim(), char_id: charId, slot});
    reportAsset(out, d);
    loadAssets();
    loadShots();
  } catch (e) { show(out, e.message, "err"); }
}

async function saveChar(id) {
  const card = document.querySelector(`[data-char="${id}"]`);
  const out = card.querySelector(".out");
  try {
    const d = await post("/api/character", {
      project: $("path").value.trim(), char_id: id,
      patch: collect(".cf", card), reset_shots: $("a_reset").checked});
    reportAsset(out, d);
    card.querySelector(".rendered").textContent = d.rendered;
    loadShots();
  } catch (e) { out.innerHTML = `<span class="err">${esc(e.message)}</span>`; }
}

async function saveLoc(id) {
  const card = document.querySelector(`[data-loc="${id}"]`);
  const out = card.querySelector(".out");
  try {
    const d = await post("/api/location", {
      project: $("path").value.trim(), location_id: id,
      patch: collect(".lf", card), reset_shots: $("a_reset").checked});
    reportAsset(out, d);
    card.querySelector(".rendered").textContent = d.rendered;
    loadShots();
  } catch (e) { out.innerHTML = `<span class="err">${esc(e.message)}</span>`; }
}

async function saveStyle() {
  const card = $("stylebox");
  const out = card.querySelector(".out");
  try {
    const d = await post("/api/style", {
      project: $("path").value.trim(), patch: collect(".sf", card),
      reset_shots: $("a_reset").checked});
    reportAsset(out, d);
    loadShots();
  } catch (e) { out.innerHTML = `<span class="err">${esc(e.message)}</span>`; }
}

async function loadOutputs() {
  const path = $("path").value.trim();
  if (!path) return;
  try {
    const d = await api("/api/outputs?path=" + encodeURIComponent(path));
    if (!d.files.length) {
      $("outlist").innerHTML = '<span class="muted">还没有成片。去运行页跑一次。</span>';
      return;
    }
    $("outlist").innerHTML = d.files.map(f => `
      <div class="outcard">
        <strong>${esc(f.name)}</strong>
        <div class="meta">${f.size_mb} MB · ${new Date(f.mtime*1000).toLocaleString()}</div>
        <video controls preload="metadata" src="${mediaUrl(f.rel)}"></video>
        <div class="row" style="margin:10px 0 0">
          <a class="dl" download="${esc(f.name)}"
             href="${mediaUrl(f.rel)}">下载这一集</a>
        </div>
      </div>`).join("");
  } catch (e) {
    $("outlist").innerHTML = `<span class="err">${esc(e.message)}</span>`;
  }
}

function closeEditor() { currentShot = null; $("editor").classList.add("hide"); loadShots(); }

// 审片时最常做的事就是「这一镜不行，重来」。分开点的话要跨两个页面
// 四步：分镜页勾上、重置、切到运行页、开始。这里一个按钮做完。
//
// 不需要新的接口：其它镜头是 final_done，各阶段本来就只挑
// 状态对得上的镜头，所以跑全流程实际只会重做被重置的这一镜，
// 最后重新装配一次。
async function rerunShot() {
  if (!currentShot) return;
  const path = $("path").value.trim(), ep = $("ep").value, id = currentShot;
  if (!await saveShot()) return;
  try {
    const r = await post("/api/shots/batch", {
      project: path, episode_id: ep, shot_ids: [id], action: "reset"});
    if (!r.changed) {
      show($("ederr"), "这一镜锁着，先解锁再重跑。", "err"); return;
    }
    await post("/api/run", {project: path, episode_id: ep});
    show($("edok"), `已开跑，只重做 ${id}，跑完会重新装配整集。`, "ok-msg");
    hide($("ederr"));
    document.querySelector(".tab[data-page=run]")?.click();
    poll();
  } catch (e) { show($("ederr"), e.message, "err"); }
}

async function saveShot() {
  if (!currentShot) return false;
  const patch = {
    first_frame_prompt: $("ed_ffp").value,
    motion_prompt: $("ed_motion").value,
    shot_size: $("ed_size").value,
    camera_angle: $("ed_angle").value,
    camera_move: $("ed_move").value,
    duration_s: parseFloat($("ed_dur").value),
    subtitle_text: $("ed_sub").value,
    needs_lipsync: $("ed_lip").checked,
  };
  if ($("ed_lock").checked) patch.status = "locked";
  const lines = [...document.querySelectorAll(".edline")].map(i => i.value);
  if (lines.length) patch.dialogue_texts = lines;
  try {
    const d = await post("/api/shot", {
      project: $("path").value.trim(), episode_id: $("ep").value,
      shot_id: currentShot, patch});
    show($("edok"), d.reset_to_planned
      ? "已保存。画面有改动，这一镜会重新生成。" : "已保存。", "ok-msg");
    hide($("ederr"));
    loadShots();
    return true;
  } catch (e) {
    show($("ederr"), e.message, "err"); hide($("edok"));
    return false;
  }
}

// ---- 运行 ----
$("stagebtns").innerHTML = STAGES.map(([k,label]) =>
  `<button class="stage-btn" data-stage="${k}">${label}</button>`).join("")
  + `<span class="muted" style="align-self:center;margin-left:6px">
       不选则跑全流程</span>`;
document.querySelectorAll(".stage-btn").forEach(b =>
  b.onclick = () => b.classList.toggle("on"));

// 按下开始就是几十分钟。哪些镜头会重做、大概要等多久，
// 这两件事应该在按下去之前就知道。
async function previewRun() {
  const box = $("runplan");
  const path = $("path").value.trim(), ep = $("ep").value;
  if (!path || !ep) { box.textContent = ""; return; }
  const q = new URLSearchParams({
    path, episode_id: ep,
    all_episodes: $("allep").checked ? "true" : "false",
    skip_final: $("skipfinal").checked ? "true" : "false",
    force: $("force").checked ? "true" : "false"});
  try {
    const d = await api("/api/run/preview?" + q);
    if (d.idle) {
      box.textContent = "这一次没有要做的镜头。想重做就勾上「全部重做」，"
        + "或者去分镜页把要重来的几镜重置。";
      return;
    }
    const what = d.stages.map(x => `${x.label} ${x.shots} 镜`).join("，");
    box.textContent = `这一次会做：${what}。`
      + (d.estimate_text ? `按当前档位粗估 ${d.estimate_text}。` : "")
      + (d.episodes.length > 1 ? ` 共 ${d.episodes.length} 集。` : "");
  } catch (e) { box.textContent = ""; }
}

async function startRun() {
  const stages = [...document.querySelectorAll(".stage-btn.on")]
    .map(b => b.dataset.stage);
  try {
    await post("/api/run", {
      project: $("path").value.trim(), episode_id: $("ep").value,
      skip_final: $("skipfinal").checked, force: $("force").checked, stages,
      all_episodes: $("allep").checked});
    $("runbtn").disabled = true;
    poll();
  } catch (e) { alert("启动失败：" + e.message); }
}

async function stopRun() { try { await post("/api/stop", {}); } catch (e) {} }

async function poll() {
  if (timer) clearTimeout(timer);
  let s;
  try { s = await api("/api/run"); } catch (e) { timer=setTimeout(poll,3000); return; }
  $("runbtn").disabled = s.running;
  const pct = s.total ? Math.round(s.current/s.total*100) : (s.running?3:0);
  $("bar").style.width = pct + "%";
  // 跑多集时要说清楚现在在第几集，否则进度条归零看着像重跑了
  const q = s.queue_total > 1
    ? `第 ${s.queue_done + 1}/${s.queue_total} 集 ${s.episode_id}  ` : "";
  $("runline").textContent = s.running
    ? `${q}${s.stage}  ${s.current}/${s.total}  已用 ${Math.round(s.elapsed_s)} 秒  ${s.message}`
    // 「已停止」是用户按了停止，「没跑完」是自己出错了。混成一句话
    // 的话，出错的那次看起来像是自己不小心点了停止。
    : ((s.outputs && s.outputs.length > 1)
       ? `出了 ${s.outputs.length} 集：` + s.outputs.join("，")
       : s.output ? "成片：" + s.output
       : (s.error ? (s.error.includes("手动停止") ? "已停止" : "没跑完，看下面的原因")
          : "未开始"));
  if (s.error) show($("runerr"), s.error, "err"); else hide($("runerr"));
  $("log").innerHTML = s.events.map(e =>
    `<div class="k-${esc(e.kind)}">${esc(e.stage)} · ${esc(e.message)}</div>`).join("");
  $("log").scrollTop = $("log").scrollHeight;
  if (s.running) loadShots();
  timer = setTimeout(poll, s.running ? 2000 : 6000);
}

// ---- 参数 ----
let settingsSnapshot = null;

// 参数字段表。一处声明，读取、提交、差分共用。
// 之前读一遍写一遍差分一遍，各写各的字段名，加个参数漏一处就是
// 「改了没生效」或者「没改却说改了」。
const SETTING_FIELDS = [
  ["s_dw", "tiers.draft.width",  "draft_width",  "num"],
  ["s_dh", "tiers.draft.height", "draft_height", "num"],
  ["s_ds", "tiers.draft.steps",  "draft_steps",  "num"],
  ["s_fw", "tiers.final.width",  "final_width",  "num"],
  ["s_fh", "tiers.final.height", "final_height", "num"],
  ["s_fs", "tiers.final.steps",  "final_steps",  "num"],
  ["s_fps",  "assembly.fps", "fps", "num"],
  ["s_crf",  "assembly.crf", "crf", "num"],
  ["s_font", "assembly.subtitle_font", "subtitle_font", "text"],
  ["s_line", "assembly.subtitle_max_chars_per_line",
             "subtitle_max_chars_per_line", "num"],
  ["s_lines","assembly.subtitle_max_lines", "subtitle_max_lines", "num"],
  ["s_trans","assembly.scene_transition_s", "scene_transition_s", "num"],
  ["s_tol",  "tts.tolerance_s",      "tts_tolerance_s", "num"],
  ["s_tempo","tts.max_tempo_shift",  "tts_max_tempo_shift", "num"],
  ["s_retry","gates.max_attempts_per_shot", "max_attempts_per_shot", "num"],
  ["s_std",  "gates.min_pixel_std",  "min_pixel_std", "num"],
  ["s_sim",  "gates.min_frame_similarity", "min_frame_similarity", "num"],
  ["s_drift","gates.max_audio_drift_s", "max_audio_drift_s", "num"],
  ["s_lufs", "gates.target_lufs",    "target_lufs", "num"],
  ["s_gates","gates.enabled",        "gates_enabled", "bool"],
  ["s_fb",   "gates.fallback_on_exhausted", "fallback_on_exhausted", "bool"],
];

const dig = (obj, path) => path.split(".").reduce((o, k) => o && o[k], obj);

async function loadSettings() {
  try {
    const d = await api("/api/settings");
    settingsSnapshot = d;
    SETTING_FIELDS.forEach(([id, path, , kind]) => {
      const v = dig(d, path);
      if (v === undefined) return;
      if (kind === "bool") $(id).checked = v; else $(id).value = v;
    });
    hide($("seterr"));
  } catch (e) { show($("seterr"), "读取失败：" + e.message, "err"); }
}

async function saveSettings() {
  const body = {};
  SETTING_FIELDS.forEach(([id, path, key, kind]) => {
    let v;
    if (kind === "bool") v = $(id).checked;
    else if (kind === "num") { v = parseFloat($(id).value); if (isNaN(v)) return; }
    else { v = $(id).value; if (!v) return; }
    // 只提交真正改过的。全都提交的话「已应用 21 项」会让人以为
    // 自己不小心动了一堆东西，实际只改了一两个。
    if (settingsSnapshot) {
      const cur = dig(settingsSnapshot, path);
      if (cur !== undefined && String(cur) === String(v)) return;
    }
    body[key] = v;
  });
  if (!Object.keys(body).length) {
    show($("setok"), "没有改动。", "ok-msg"); hide($("seterr")); return;
  }
  try {
    const d = await post("/api/settings",
                         {patch: body, persist: $("s_persist").checked});
    const where = d.saved_to ? `，已写入 ${d.saved_to}` : "";
    show($("setok"),
         `已应用 ${d.changed.length} 项：${(d.labels||d.changed).join("、")}${where}`,
         "ok-msg");
    hide($("seterr"));
    await loadSettings();
  } catch (e) { show($("seterr"), e.message, "err"); hide($("setok")); }
}

$("selall").onchange = () => {
  document.querySelectorAll(".pick").forEach(c => c.checked = $("selall").checked);
  refreshSel();
};
$("ep").addEventListener("change", () => {
  loadShots(); loadScript(); previewRun();
});
["skipfinal", "force", "allep"].forEach(id =>
  $(id).addEventListener("change", previewRun));
document.querySelector(".tab[data-page=run]")
  .addEventListener("click", previewRun);
loadDoctor();
loadConns();
loadProjectList();
const saved = localStorage.getItem("changji.path");
if (saved && !$("path").value) $("path").value = saved;
if ($("path").value) loadProject();
poll();
</script>
</body>
</html>
"""


def render_page(comfy_url: str = "", default_project: str = "") -> str:
    return (
        _PAGE
        .replace("__COMFY__", html.escape(comfy_url))
        .replace("__PROJECT__", html.escape(default_project))
    )
