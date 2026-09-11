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
 * **搭系统表那趟车，不另开轮询。** 引擎两秒推一条 `system`，里面顺带带着
 * 此刻在跑的那几件（`jobs`）——要的就是这个节奏，另开一条等于为同一件事
 * 做两遍功。
 *
 * ⚠️ **"排队中"现在没有。** 引擎里长跑任务一种只有一个槽，同种再点会当场
 * 被拒（409「已经在写了」），不是排到后面。所以这儿列的都是真在跑的；
 * 等哪天做了真排队，再在同一个列表里往下接。写在这儿是免得下一个人以为
 * 漏了。
 */
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { useRouter } from 'vue-router'

import AppIcon from '@/components/AppIcon.vue'
import { openJobSocket } from '@/composables/useJobSocket'
import { useProjects } from '@/stores/projects'
import { useSession } from '@/stores/session'

const router = useRouter()
const session = useSession()
const projects = useProjects()

const jobs = ref([])
const open = ref(false)
const root = ref(null)
let sock = null
let retry = null
let watchdog = null
let lastAt = 0
let gone = false

/** 和系统表同一条规矩：八秒没动静就当断了。见 SysMeter 里那段。 */
const kStaleMs = 8000

function connect() {
  if (gone) return
  lastAt = Date.now()
  sock = openJobSocket(
    'system',
    (msg) => {
      if (msg.type !== 'system') return
      lastAt = Date.now()
      jobs.value = msg.jobs ?? []
    },
    () => {
      sock = null
      jobs.value = []
      clearTimeout(retry)
      retry = setTimeout(connect, 5000)
    },
  )
}

function sweep() {
  if (gone || !sock) return
  if (Date.now() - lastAt < kStaleMs) return
  // 断了就当没有任务在跑。**留着旧的更糟**：顶栏说"正在出片"，而那一轮
  // 可能早跑完了，点进去什么都没有。
  jobs.value = []
  const dead = sock
  sock = null
  dead.close()
  connect()
}

/** 点别处关掉。划过就开那版靠 mouseleave，点开这版得自己收。 */
function onDocClick(e) {
  if (!open.value) return
  if (root.value && !root.value.contains(e.target)) open.value = false
}
function onEsc(e) {
  if (e.key === 'Escape') open.value = false
}

onMounted(() => {
  connect()
  watchdog = setInterval(sweep, 3000)
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
  gone = true
  clearInterval(watchdog)
  clearTimeout(retry)
  document.removeEventListener('click', onDocClick, true)
  document.removeEventListener('keydown', onEsc)
  sock?.close()
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
  write: { label: '写正文', page: '/story', icon: 'sparkle' },
  write_one: { label: '写这一章', page: '/story', icon: 'sparkle' },
  revise: { label: '改稿', page: '/story', icon: 'sparkle' },
  outline: { label: '出大纲', page: '/story', icon: 'sparkle' },
  analyze: { label: '读故事', page: '/story', icon: 'sparkle' },
  image: { label: '出参考图', page: '/assets', icon: 'image' },
  say: { label: '朗读', page: '/story', icon: 'sparkle' },
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
    return {
      ...j,
      label: k.label,
      page: k.page,
      icon: k.icon,
      name: nameOf(j.project),
      pct: j.total > 0 ? Math.min(100, Math.round((j.current / j.total) * 100)) : 0,
    }
  }),
)

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
      <span v-if="rows.length > 1" class="jb__n">{{ rows.length }}</span>
      <AppIcon :name="open ? 'arrowLeft' : 'arrowRight'" :size="11" />
    </button>

    <div v-if="open" class="jb__pop">
      <button
        v-for="(r, i) in rows"
        :key="i"
        class="jb__row"
        type="button"
        :title="`去 ${r.name || '这个项目'} 的${r.label}页`"
        @click="go(r)"
      >
        <AppIcon :name="r.icon" :size="13" />
        <span class="jb__kind">{{ r.label }}</span>
        <span class="jb__name truncate">{{ r.name }}</span>
        <span v-if="r.episode_id" class="jb__ep">{{ r.episode_id }}</span>
        <!-- 短活多半没有进度（出图头十几秒在读权重，一次回调都没有）。
             那时候不画空进度条、也不写"0/—"：一个永远停在 0 的进度条
             看着像卡住了，而它只是没有步数可报。 -->
        <template v-if="r.total > 0">
          <span class="jb__bar"><i :style="{ width: r.pct + '%' }" /></span>
          <span class="jb__num numeric">{{ r.current }}/{{ r.total }}</span>
        </template>
        <template v-else><span /><span /></template>
        <span class="jb__msg truncate">{{ r.message }}</span>
      </button>
    </div>
  </div>
</template>

<style scoped>
.jb {
  position: relative;
  flex: none;
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
  border-radius: var(--r-md);
  box-shadow: 0 6px 24px rgb(0 0 0 / 0.35);
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
  color: var(--text-1);
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
