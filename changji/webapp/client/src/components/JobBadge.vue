<script setup>
/**
 * 顶栏那块「AI 作业中」。
 *
 * 用户 2026-09-11：「在顶部增加 AI 作业中，鼠标放到上面列表出正在作业的
 * 全部内容和排队中的内容，点击直接到那个页面」，随后改成「应该是点击弹出
 * 菜单显示」。
 *
 * **点开，不是划过就开。** 划过就开的那版实际用起来有个毛病：这块在顶栏
 * 上，鼠标从导航移到设置、从标题移到引擎灯，都要路过它——每次路过弹一张
 * 列表出来，挡住底下的东西。点开的话，什么时候看是人说了算。
 *
 * **没在跑就整个不显示。** 顶栏上常驻一个写着"0 个任务"的东西，是在提醒
 * 一件不存在的事。
 *
 * **搭系统表那趟车。** 引擎两秒推一条 `system`，里面顺带带着此刻在跑的
 * 那几件（`jobs`）——要的就是这个节奏。那份表在 `useSystemFeed` 里接
 * （顶栏那三个小表也读它），连不上 WebSocket 时它自己会退回拉
 * `/api/system`，回的是同一份 body。
 *
 * **"排队中"是真的。** 显存这一层现在会排队：借不到槽的活按先来后到
 * 排成一列等着（见 Scheduler::AcquireOptions），排到自己才干。所以这个
 * 列表里两种行都有，靠 `queued` 分——不靠那句话的前缀，那句话改一个字
 * 界面就会悄悄把排队的全算成在跑的。
 *
 * ⚠️ 长跑任务（出片、写整季）不在此列：一种只有一个槽，同种再点当场被拒
 * （409「已经在写了」）。那是另一个层面的事，要排也得先有个任务队列。
 */
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { RouterLink, useRouter } from 'vue-router'

import AppIcon from '@/components/AppIcon.vue'
import { STAGE_LABELS } from '@/api/labels'
import { useSystemFeed } from '@/composables/useSystemFeed'
import { useProjects } from '@/stores/projects'
import { useSession } from '@/stores/session'

const router = useRouter()
const session = useSession()
const projects = useProjects()

/**
 * 那份表在哪儿接的：`useSystemFeed`（模块级一份，和顶栏那三个小表共用）。
 *
 * **这儿原来自己开一条 socket**，和 SysMeter 逐字重复一份——连讣告串台、
 * 看门狗、重连越积越多那几个坑都分别踩了一遍、修了一遍。两条订的还是同一
 * 个频道，引擎两秒一次那份表要发两遍。
 *
 * 收在一处顺带补上了**连不上 WebSocket 时退回轮询**。这一块尤其要紧：
 * 从设定页点完「批量补分镜」之后，它是**唯一**看得见的出口（那一页自己的
 * 提示就写着「顶栏那块「AI 作业中」里看进度」），而代理掐了 Upgrade 的
 * 部署上它原来一次都不会出现——人按下去之后屏幕上再没有任何东西。
 */
const { stat } = useSystemFeed()
const jobs = computed(() => stat.value?.jobs ?? [])
const open = ref(false)
const root = ref(null)

/** 点别处关掉。划过就开那版靠 mouseleave，点开这版得自己收。 */
function onDocClick(e) {
  if (!open.value) return
  if (root.value && !root.value.contains(e.target)) open.value = false
}
function onEsc(e) {
  if (e.key === 'Escape') open.value = false
}

onMounted(() => {
  // **捕获阶段**：页面上别处有不少 `@click.stop`（卡片、抽屉、列表行），
  // 挂在冒泡阶段的话，点到那些地方这个菜单收不掉——而"点哪儿都关不上的
  // 浮层"是最烦人的一种。捕获阶段先于它们拿到事件，且只读不拦。
  document.addEventListener('click', onDocClick, true)
  document.addEventListener('keydown', onEsc)
  // **自己拉一次项目库。** 平时是项目栏在拉，但专注模式下那条栏整个不
  // 渲染——那时候这块就只能显示路径尾巴了，而"哪部剧在跑"正是它要回答的。
  if (!projects.loaded) projects.load()
})
onUnmounted(() => {
  document.removeEventListener('click', onDocClick, true)
  document.removeEventListener('keydown', onEsc)
})

/**
 * 引擎报上来的那几种活。
 *
 * 前两种是**长跑任务**（任务表里那两个槽），后面几种是**同步请求**——
 * 几十秒就回，没有任务表那一套，以前在界面上整个不可见。2026-09-11 撞上
 * 过：用户正出着参考图（占着图像槽），另一头的批量写作四章全挂在「显存
 * 不够加载 LLM：「图像」正用着」，而顶栏一片安静、GPU 占用 0%——挡路的
 * 那件事只能登服务器翻日志才查得到。现在它们都登记，都在这儿。
 */
const KIND = {
  run: { label: '出片', page: '/episode', icon: 'film' },
  // **「写」这个槽里跑的不止一种活**（JobKind::Write 只有一个，三条路共用
  // 它）：展开剩下 N 章、写整季、以及**批量补分镜**（/api/plan/all，从设定页
  // 的「分集」那一格按下去的）。写死「写正文」的话，补分镜跑着的时候顶栏
  // 说的是「写正文」，而它右边那句引擎现说的消息写着「正在给 ep02 出分镜」
  // ——同一行里两种说法。而设定页那条提示恰恰是叫人来这儿看的
  // （「正在给 … 补分镜，顶栏那块「AI 作业中」里看进度」）。
  //
  // 三条路的 message 都自带主语（「正在写 ch03（第三章）」「正在写第 3 集」
  // 「正在给 ep02 出分镜」），所以这一格退回**类别**就够，具体干什么由它说。
  // 引擎那边今天分不出来：三条 start 传的 episode_id 都是空串，stage 也只有
  // 出片那一族在报——真要分，得先给这个槽加一个能认的字段。
  //
  // 页面仍然指 /story：三条里两条（展开正文、写整季）在那儿，补分镜那条在
  // 设定页的「分集」格。分不出来的时候指中多数那一个。
  write: { label: '批量', page: '/story', icon: 'sparkle' },
  write_one: { label: '写这一章', page: '/story', icon: 'sparkle' },
  revise: { label: '改稿', page: '/story', icon: 'sparkle' },
  outline: { label: '出大纲', page: '/story', icon: 'sparkle' },
  analyze: { label: '读故事', page: '/story', icon: 'sparkle' },
  image: { label: '出参考图', page: '/assets', icon: 'image' },
  say: { label: '朗读', page: '/story', icon: 'sparkle' },
  // 2026-09-13 补的这四种：剧本和分镜那几个接口以前一个都没登记，
  // 点了「重新改编」顶栏一片安静，而那一刻 LLM 槽正被它占着。
  // 梗概在故事页那个框里想（`/story` 的「想几个给我挑」），不在这一集里。
  // 原来指着 /episode 是照着"剧本大纲"那一页写的，那一页 2026-09-11 删了。
  //
  // **预告片同样不在 /episode 上。** 整份界面里只有设定页「分集」那一格的
  // 折叠区（标题「预告片 · 手动加一集」）有它：剪的按钮、时长下拉、以及
  // 剪完那份**只活在内存里**的草稿和它的「存成 trailer 这一集」。跳去
  // /episode 的话，落在的是当前这一集的镜头屏——那儿一个预告片的字都没有，
  // 而草稿就在你没去的那一页上等着按「采用」，退出去就没了。
  premise: { label: '想梗概', page: '/story', icon: 'sparkle' },
  script: { label: '写剧本', page: '/episode', icon: 'sparkle' },
  trailer: { label: '剪预告', page: '/assets?tab=episodes', icon: 'film' },
  bible: { label: '定角色场景', page: '/assets', icon: 'sparkle' },
  plan: { label: '拆镜头', page: '/episode', icon: 'board' },
}

function nameOf(path) {
  if (!path) return ''
  // **字段是 name**（/api/projects 回的那份），不是 title 也不是 project_id。
  // 写错了不会报错，只会一路退到下面那个路径尾巴——看着像"项目库没加载"，
  // 其实是找错了键。第一次就栽在这儿。
  const hit = projects.items?.find((p) => p.path === path)
  // 找不到就拿目录名顶上：项目库还没拉回来的那一小会儿，显示一个路径
  // 尾巴也比显示空白强。
  return hit?.name || path.split(/[\\/]/).filter(Boolean).pop() || ''
}

const rows = computed(() =>
  jobs.value.map((j) => {
    const k = KIND[j.kind] ?? { label: j.kind, page: '/project', icon: 'sparkle' }
    // **出片那条要说清在跑哪一段。**
    //
    // `run` 是一个长跑任务，底下依次是配音、首帧、成片档、装配。一律写
    // 「出片」的话，跑配音的那几分钟顶栏也说「出片」——用户报的原话是
    // 「配音在界面上都不显示」。stage 引擎本来就在推，用上就行。
    const stage = j.kind === 'run' ? STAGE_LABELS[j.stage] : ''
    return {
      ...j,
      label: stage || k.label,
      page: k.page,
      icon: k.icon,
      name: nameOf(j.project),
      pct: j.total > 0 ? Math.min(100, Math.round((j.current / j.total) * 100)) : 0,
    }
  }),
)

/** 真在跑的有几件。**顶栏那个数只数这些**：排队的还没开始干。 */
const running = computed(() => rows.value.filter((r) => !r.queued).length)
const queued = computed(() => rows.value.length - running.value)

/**
 * 点一行就过去。
 *
 * **先切项目再跳页**：任务可能是另一部剧的，不切的话跳过去看到的是当前
 * 这部剧的同名页面——比不跳更误导。
 */
async function go(row) {
  open.value = false
  if (row.project && row.project !== session.projectPath) {
    await session.selectProject(row.project)
  }
  if (row.episode_id) session.selectEpisode(row.episode_id)
  router.push(row.page)
}
</script>

<template>
  <div v-if="rows.length" ref="root" class="jb">
    <button
      class="jb__btn"
      :class="{ 'is-open': open }"
      type="button"
      :title="open ? '收起' : '看看在跑什么'"
      @click.stop="open = !open"
    >
      <span class="jb__dot" />
      <span class="jb__t">AI 作业中</span>
      <span v-if="running > 1" class="jb__n">{{ running }}</span>
      <!-- 排队的单独说。混进上面那个数的话，"3 件"里可能只有 1 件真在跑，
           而用户是照着这个数判断"还要等多久"的。 -->
      <span v-if="queued" class="jb__q">+{{ queued }} 排队</span>
      <AppIcon :name="open ? 'arrowLeft' : 'arrowRight'" :size="11" />
    </button>

    <div v-if="open" class="jb__pop">
      <button
        v-for="(r, i) in rows"
        :key="i"
        class="jb__row"
        :class="{ 'is-queued': r.queued }"
        type="button"
        :title="`去 ${r.name || '这个项目'} 的${r.label}页`"
        @click="go(r)"
      >
        <AppIcon :name="r.icon" :size="13" />
        <span class="jb__kind">{{ r.label }}</span>
        <span class="jb__name truncate">{{ r.name }}</span>
        <!-- **没有集号也要占住这一格。** `.jb__row` 是写死的七列网格
             （auto auto 1fr auto 56px auto 1.2fr），少一个子元素后面就
             全体左移一列：项目级的那些作业（照故事定妆、参考图）本来就
             没有 episode_id，于是进度条落进 auto 那一列——它没有内容撑，
             直接塌成零宽，而那正是跑十几分钟、最需要看进度的几件活；
             消息那一格也跟着挪到 auto 上，长消息不缩，把整行顶出弹层。
             下面那对空 <span/> 是同一件事（没有步数可报时补齐两格），
             这儿漏了。 -->
        <span v-if="r.episode_id" class="jb__ep">{{ r.episode_id }}</span>
        <span v-else />
        <!-- 短活多半没有进度（出图头十几秒在读权重，一次回调都没有）。
             那时候不画空进度条、也不写"0/—"：一个永远停在 0 的进度条
             看着像卡住了，而它只是没有步数可报。 -->
        <template v-if="r.total > 0">
          <span class="jb__bar"><i :style="{ width: r.pct + '%' }" /></span>
          <span class="jb__num numeric">{{ r.current }}/{{ r.total }}</span>
        </template>
        <template v-else><span /><span /></template>
        <!-- 这一句是引擎现说的（「正在给 ep02 出分镜」「显存不够，排队中」），
             一行放不下就截断——全文挂 title 上，不然最要紧的那半句正好在
             外面。 -->
        <span class="jb__msg truncate" :title="r.message">{{ r.message }}</span>
      </button>
      <!-- **这块牌子只有一行的地方，任务页面有一整页。** 排着的那几件、
           已经用了多久、刚才那几件花了多少、大模型想了什么，都在那儿
           （用户 2026-09-17）。 -->
      <RouterLink class="jb__all tiny" to="/tasks" @click="open = false">
        全部任务 · 排队和做完的
      </RouterLink>
    </div>
  </div>
</template>

<style scoped>
.jb {
  position: relative;
  flex: none;
}
.jb__all {
  display: block;
  padding: 7px 10px;
  border-top: 1px solid var(--line);
  color: var(--text-2);
  text-decoration: none;
}
.jb__all:hover {
  background: var(--bg-2);
  color: var(--text);
}
.jb__btn {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 3px 9px;
  border: 1px solid var(--accent);
  border-radius: 999px;
  background: var(--accent-soft);
  color: var(--accent);
  font-size: var(--fs-xs);
  cursor: pointer;
}
/* 一个会喘气的点。顶栏上不该有第二个进度条跟系统表抢眼睛——
   有没有在跑，一个点就够了；细节鼠标放上去再说。 */
.jb__dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: currentColor;
  animation: jb-pulse 1.4s ease-in-out infinite;
}
@keyframes jb-pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.25; }
}
.jb__btn.is-open {
  background: var(--accent);
  color: var(--bg);
}
.jb__n {
  font-variant-numeric: tabular-nums;
  opacity: 0.8;
}
.jb__q {
  font-variant-numeric: tabular-nums;
  opacity: 0.65;
}
/* 排队的那几行压下去一点：一眼能看出哪几件真在动。 */
.jb__row.is-queued {
  opacity: 0.62;
}

.jb__pop {
  position: absolute;
  top: calc(100% + 6px);
  right: 0;
  z-index: 60;
  min-width: 380px;
  display: grid;
  gap: 2px;
  padding: 4px;
  background: var(--surface);
  border: 1px solid var(--line);
  /* 没有 --r-md（是 --r-sm / --r / --r-lg）。作废之后这张浮层是方角的，
     而隔壁那张思考浮层写死 10px，正好是 --r。 */
  border-radius: var(--r);
  box-shadow: 0 6px 24px rgb(0 0 0 / 0.35);
}
/* **手机上这块弹层比屏幕还宽。**
 *
 * 上面那个 `min-width: 380px` 配 `right: 0`：弹层右边缘对齐角标，而角标在
 * 顶栏靠右（顶栏左右各留 16px）。375px 的手机上，380 的下限意味着左边要
 * 溢出 20 多个像素——不是被裁掉就是把整页顶出一条横向滚动，而这个库里
 * 别处都写着"整页不许横滚"。这个组件原来一条窄屏规则都没有。
 *
 * 下限去掉、上限按屏宽算：右边缘离屏幕右沿 16px，宽度取 100vw-24px，
 * 左边就还剩 8px。行里那两条 minmax(0, …) 本来就能缩，名字和消息上都有
 * .truncate，挤是挤一点，但看得见也点得动。 */
@media (max-width: 520px) {
  .jb__pop {
    min-width: 0;
    max-width: calc(100vw - 24px);
  }
}

.jb__row {
  display: grid;
  grid-template-columns: auto auto minmax(0, 1fr) auto 56px auto minmax(0, 1.2fr);
  align-items: center;
  gap: 8px;
  padding: 6px 8px;
  border: 0;
  border-radius: var(--r-sm);
  background: transparent;
  color: var(--text);
  font-size: var(--fs-xs);
  text-align: left;
  cursor: pointer;
}
.jb__row:hover {
  background: var(--accent-soft);
}
.jb__kind {
  color: var(--accent);
  flex: none;
}
.jb__ep {
  color: var(--text-3);
  font-variant-numeric: tabular-nums;
}
.jb__bar {
  height: 3px;
  border-radius: 2px;
  background: var(--line);
  overflow: hidden;
}
.jb__bar i {
  display: block;
  height: 100%;
  background: var(--accent);
}
.jb__num,
.jb__msg {
  color: var(--text-3);
}
</style>
