<script setup>
/**
 * 项目库。**常驻在右边，不在项目页里。**
 *
 * 换项目原来要先走到第一步那一页，挑完再走回来——而人在写故事、看镜头的
 * 时候想起来要对照另一部剧，这一来一回就把当前这一页的状态丢了。
 * 常驻之后：**点一下就换，人还停在原来那一页**。
 *
 * 每条只放两样：剧名，和到哪一步了。路径、画风、镜头数都不在这儿——
 * 那些是「这一部剧的详情」，归项目页；这条栏只回答「切到哪一部」。
 *
 * 2026-09-14 重排，按同一条规矩（每次打开都要回答的问题才留在页面上）：
 *
 *   · **按阶段排，同阶段按最近动过。** 原来只按 mtime 倒序，而 mtime
 *     界面上一个字都不显示——顺序有依据但看不见，而且建一个测试壳就把它
 *     顶到正在做的剧前面（实见：6 条里 4 条是 refresh-test / glm-demo /
 *     321 / smoke-tmp 这种残留）。按阶段排之后顺序有了看得见的理由：
 *     第二行那句话就是排序键，空壳自动沉底。
 *   · **每条能改名、能删。** 原来删一个空壳要先点中它（当前项目和集号
 *     一起被换掉）→ 走到项目页 → 删 → 打一遍名字 → session.clear() 之后
 *     一个项目都没选 → 再点回自己那部剧、再挑回那一集。四个空壳四轮。
 *   · **搜索框删了。** 门槛是 `count > 6`，而它对应的不是任何真实的东西
 *     （一屏装得下十几条才开始滚）。它还带着一个真 bug：keyword 是个裸
 *     ref，7 个项目时搜着搜着删掉一个，输入框被 v-if 卸载、过滤还在，
 *     列表一条不剩且没有复位入口，只能刷新页面。
 *   · **拖拽换边整套删了**（onGrab/onMove/onDrop/flip/fromControl/dockhint
 *     约 110 行）。靠哪边一辈子设一次，入口挪进设置页的「界面」。而且它
 *     在栏头空白处按一下鼠标、根本没拖，就会让整条栏变半透明、屏幕上闪出
 *     一条通高的虚线遮罩——`onGrab` 在 pointerdown 上就无条件置 dragging。
 *   · 栏头那个数字徽标、两条 toast（「已切到 X」「项目库挪到左边了」）、
 *     每条的 hover 提示都删了：说的都是眼睛已经看到的事。
 *
 * ⚠️ **每条是 div 不是 button。** 里面要放「⋯」，而按钮不能嵌按钮；
 * 键盘那条路靠 tabindex + role + keydown 自己补——这条栏是换项目唯一的
 * 入口，纯键盘用户丢了它就换不了项目。
 */
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue'
import { useRoute } from 'vue-router'

import AppIcon from '@/components/AppIcon.vue'
import AddProjectDialog from '@/components/AddProjectDialog.vue'
import { projectStage } from '@/composables/project-stage'
import { api } from '@/api'
import { useAction } from '@/composables/useAction'
import { useProjects } from '@/stores/projects'
import { readLocal, writeLocal } from '@/composables/local-storage'
import { useLongRunning, useSystemFeed } from '@/composables/useSystemFeed'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const store = useProjects()
const session = useSession()
const ui = useUi()
/**
 * 顶栏那份系统表：两秒一拍，每一行带着 project 和 kind。
 *
 * **这条栏该问的两件事（谁在跑、跑完了没有）都从它读。** 原来读的是
 * `useRun` / `useWriter` 那两个 store，而它们在这一页上没有人驱动——
 * 两处都栽过，理由分别写在 busyPaths 和 longRunning 上面。
 */
const sysFeed = useSystemFeed()
const { run, isBusy } = useAction()

const route = useRoute()
const adding = ref(false)
/** 哪一条的「⋯」开着。空串是都没开。 */
const menuFor = ref('')
/** 正在改名的那一条，和输入框里的字。 */
const renaming = ref('')
const newName = ref('')
/** 改名那个输入框。startRename 之后要自己聚焦，见那儿。 */
const box = ref(null)
/** 正在确认删除的那一条，和确认框里打的目录名。 */
const removing = ref('')
const confirmName = ref('')
/**
 * 正在跑的是哪几个项目（目录绝对路径）。
 *
 * **从顶栏那份系统表读。** `running_work()` 那份列表每一条都带着 project，
 * `/api/system` 和 "system" 频道推的就是它（`/api/run` 和
 * `/api/script/series` 那两份快照里没有 project——它们的字段数被单元测试
 * 钉死了，加不得）。那份表 `useSystemFeed` 两秒一拍地在拉，搭它的车就行。
 *
 * ⚠️ **三种情况要分开，不能都用空集表示。**
 *   `null`  —— 还不知道（表没回来、拉不到）。下面那道闸 fail-closed。
 *   空 Set  —— 表回来了，**谁都没在跑**。
 *   有内容  —— 就这几个在跑。
 *
 * ⚠️ **这儿原来是"开 ⋯ 菜单时现问一次"，而且前面挡着一句
 * `if (!(runner.running || writer.running)) { 空集; return }`。那一句是
 * 个 fail-open 的短路**：`useRun` 只有镜头页在驱动、`useWriter` 只有故事页
 * 和设定页那一格在驱动，而这条栏在**每一页**上。刚打开浏览器、或者压根没
 * 去过镜头页的时候，两个旗子都是假的——于是它不问就断言"谁都没在跑"，
 * 而引擎那头可能正在给这部剧写分镜。实测：假引擎报着「正在给 ep02 出分镜」，
 * 项目页上那条的「改名」「删掉」两颗全亮着，一句提示都没有。
 *
 * （反方向那个坑的记录留着：后两种都写成空集、`busyProject` 拿"集合是空的"
 * 当"不确定"的那一版，会因为旗子卡在真上而把每一条都变灰，提示还写着
 * 「正在跑，跑完再删」。两个方向都栽过，所以三态要分清。）
 */
const busyPaths = computed(() => {
  const jobs = sysFeed.stat.value?.jobs
  if (!jobs) return null
  return new Set(jobs.map((j) => j.project).filter(Boolean))
})

/** 开 / 关这一条的 ⋯。 */
function toggleMenu(p) {
  menuFor.value = menuFor.value !== p.path ? p.path : ''
}

/**
 * 收没收起，**故事页和别处各记各的**。
 *
 * 故事页默认收起：那一页是个编辑器，而这条栏列着另外几部剧——你正在写
 * 第一章，旁边摆着"你还可以去干的别的事"。别的页默认展开，换项目本来就是
 * 在那些页上干的事。两边分开记，在故事页展开过一次不会让别处也跟着变。
 */
const onStory = computed(() => route.meta?.step === 'story')
/**
 * 手机宽度。**只用来定"没设过的时候默认收不收起"。**
 *
 * 展开态那 10rem 在 375px 的屏上就是 160px——占掉 43%，正文只剩 215px，
 * 实测设置页的「重新体检」被裁成「重新体」、「保存」裁成「保」，项目页
 * 那行「读不到模型清单…」也断在半截。而这一栏又不能 display:none
 * （理由见样式里那段：861–900px 那一版就是这么把"换项目"整个弄没的）。
 *
 * 出路是这一栏自己早就有的那一个：**收起**。收起之后只剩一条窄边，
 * 上面那个箭头和加号都还在（加号是新建项目的唯一入口，样式注释里专门
 * 交代过收起也要留着），想换项目点一下就展开。
 *
 * 只动**默认值**，不动用户的选择：下面 collapsed 的 getter 里，
 * localStorage 有值就以它为准，这条只在"从来没手动收放过"时起作用。
 */
const narrowQuery = window.matchMedia('(max-width: 560px)')
const narrow = ref(narrowQuery.matches)
const onNarrow = (e) => {
  narrow.value = e.matches
}
const foldKey = computed(() => (onStory.value ? 'changji.rail.story' : 'changji.rail'))
const foldTick = ref(0)
const collapsed = computed({
  get() {
    foldTick.value // 写 localStorage 之后靠它重算
    const v = readLocal(foldKey.value)
    // 没设过：故事页本来就默认收起（那一页要整屏写字），手机上一律收起
    return v === null ? onStory.value || narrow.value : v === '1'
  },
  set(v) {
    writeLocal(foldKey.value, v ? '1' : '0')
    foldTick.value++
  },
})

/**
 * 排序：**坏的最前、空壳垫底，中间按走到哪一步，同一档按最近动过**。
 *
 * 引擎那边已经按 mtime 倒序排过一轮（新的在前），这里在它上面再按档次
 * 排一次——`Array.prototype.sort` 是稳定的，所以同一档内仍然保持
 * 「最近动过的在上面」。
 *
 * 为什么不沿用纯 mtime：那个顺序的依据在界面上一个字都看不见，而且建一个
 * 测试壳就会把它顶到正在做的剧前面。
 *
 * ⚠️ **排序键是 rank 不是 percent。** percent 在 film 那一档是档内比例
 * （出了 3/12 集 = 25），拿它排的话正在发片的剧会沉到「还没分镜」（45）
 * 下面。见 project-stage.js 头上那段。
 */
const shown = computed(() =>
  [...store.items].sort((a, b) => projectStage(b).rank - projectStage(a).rank),
)

onMounted(() => {
  store.load()
  // 旋转屏幕、改窗口大小都会跨过那条线。addEventListener 而不是 onchange：
  // 后者只能挂一个，别处再挂就把这个顶掉了。
  narrowQuery.addEventListener('change', onNarrow)
})
onUnmounted(() => narrowQuery.removeEventListener('change', onNarrow))

/**
 * 跑完一集、写完一批之后，卡上那句「到哪一步了」要跟上。
 *
 * **盯的是"跑完"这个下降沿，不是 projectPath。** 原来那条 watch 盯的是
 * 换项目，而注释写着「建了、删了、跑完一集之后列表要跟上」——跑一集根本
 * 不改 projectPath，那句话代码从来没做到过：done_shots 从 0 变到 12 之后
 * 卡上仍然写着「12 镜待出片」，直到你切一次项目或者刷新页面。
 *
 * 反过来，换项目**不该**重拉：列表内容一个字都没变，而 store.load() 会把
 * 每个项目的 project.json + story.json 都重读一遍（引擎那边的注释自己
 * 写着 story.json「可能有几百 KB」）。建项目那条路 AddProjectDialog 已经
 * 自己 load 过，再 watch 一次就是连读两遍。
 */
/**
 * 长跑任务有没有在跑。**从那份系统表读，不看 `runner` / `writer` 那两个旗子。**
 *
 * ⚠️ 这一条原来写的是 `watch(() => runner.running || writer.running, …)`，
 * **而那两个 store 在这一页上没有人驱动**：`useRun` 只有镜头页在轮询、
 * `useWriter` 只有故事页和设定页那一格。这条栏却在每一页上——于是"跑完了
 * 重拉一次"这件事，恰恰在**卡片就摆在眼前的项目页**上一次都不会发生。
 * 实测：引擎那头一轮批量跑完，`/api/projects` 一次都没重拉，卡上那句
 * 「1/2 集已出片」原样挂着。上面那段话说的正是这个症状，它只是没等到人。
 *
 * 只认 run / write 两种（就是原来那两个槽的语义）：出参考图那种短活也在
 * 这份表里，一键出图一跑就是十几条，跟着它重拉等于把每个项目的
 * project.json + story.json 重读十几遍。
 */
const longRunning = useLongRunning()

watch(longRunning, (now, before) => {
  if (before === true && now === false) store.load()
})

/**
 * 换项目时也补一次，**但只在离开一个项目之后**。
 *
 * 下降沿那条只盯 Run / Write 两个作业槽，而真正改动项目数据的路远不止
 * 它们：写一章正文、落成剧集、提人物、删章都是同步 POST，一个都不占槽。
 * 在故事页写满三章、切到别的项目再切回来，栏上那条要是还写着「还没写
 * 故事」，人会以为写的东西丢了。
 *
 * 代价是重读一遍每个项目的 project.json + story.json（引擎那边的注释写着
 * story.json「可能有几百 KB」），所以**只在真的换了项目时**走这一条：
 * 建项目那条路 AddProjectDialog 自己 load 过，这里跳过，不连读两遍。
 */
watch(
  () => session.projectPath,
  (now, before) => {
    if (before && now && before !== now) store.load()
  },
)

function pick(p) {
  // 别的行开着菜单/确认框时点这一行，先把那些收掉——留着的话列表下面
  // 会挂着一个和当前操作无关的输入框
  menuFor.value = ''
  removing.value = ''
  if (p.path === session.projectPath) return
  if (p.broken) {
    ui.error(`这个项目读不了：${p.broken}`)
    return
  }
  // **不跳转。** 人在哪一页就留在哪一页——换项目是换上下文，不是换任务。
  //
  // 也不弹 toast：那一条同一瞬间就变成高亮、顶栏的名字也跟着换了，
  // 再说一遍「已切到 X」是复述眼睛已经看到的事。
  session.selectProject(p.path)
}

/**
 * 回车/空格也能换项目。每条是 div，键盘那条路得自己补。
 *
 * ⚠️ **只认落在行本身上的那一下。** 不判这个的话，行里那个「⋯」按钮和
 * 改名输入框的键都会冒泡上来：
 *   · 在改名框里打空格 → preventDefault 把字符吞掉（打不出带空格的剧名），
 *     同时 pick(p) 把当前项目换成这一条；
 *   · Tab 到「⋯」按 Enter/空格 → preventDefault 取消了按钮的默认激活，
 *     菜单永远打不开，项目却被换了。
 * Vue 的 `@keydown.stop.enter` 编译成 withKeys(withModifiers(h,['stop']),
 * ['enter'])——**withKeys 在外层**，键不匹配时里面那个 stopPropagation
 * 根本不会执行，所以在输入框上逐个补 .stop 补不全。
 */
function onKey(p, event) {
  if (event.target !== event.currentTarget) return
  if (event.key === 'Enter' || event.key === ' ') {
    event.preventDefault()
    pick(p)
  }
}

/**
 * 点开改名。**必须自己聚焦**：菜单被卸载之后焦点落回 body，输入框是冷的，
 * 而清掉 renaming 的三条路（Enter / Esc / blur）全挂在这个框自己身上——
 * 没聚焦过就永远不会 blur，那一行会一直挂着编辑框而不是剧名，收起再展开
 * 也不复位（collapsed 只是把列表 v-if 掉，组件实例和 ref 都还在）。
 */
async function startRename(p) {
  menuFor.value = ''
  renaming.value = p.path
  newName.value = p.name
  await nextTick()
  // ⚠️ **这个 ref 挂在 v-for 里的元素上，Vue 收的是数组。**
  //
  // 编译产物里那一项带着 `ref_for: true`（`npx vue-tsc` 看不出来，要把
  // 模板编出来才看得见），于是 `box.value` 是 `[input]` 而不是 input。
  // 这儿原来写的是 `box.value?.focus()`——`?.` 只判 `box.value` 本身是不是
  // 空，数组是真值，照走下去调 `box.value.focus()`，**每点一次「改名」
  // 抛一个 TypeError**，聚焦和全选一件都没发生。
  //
  // 而上面那段注释里说的正是没聚焦的后果：清掉 renaming 的三条路
  // （Enter / Esc / blur）全挂在这个框身上，没聚焦过就永远不会 blur——
  // 点了改名又去点别的项目，那一行就一直挂着编辑框而不是剧名。
  const el = Array.isArray(box.value) ? box.value[0] : box.value
  el?.focus()
  el?.select()
}

async function commitRename(p) {
  const title = newName.value.trim()
  if (!title || title === p.name) {
    renaming.value = ''
    return
  }
  const r = await run(
    () => api.renameProject({ project: p.path, title }),
    { key: 'rename:' + p.path },
  )
  renaming.value = ''
  if (!r) return
  await store.load()
  // 改的是当前这一部的话，顶栏那个名字也要跟上
  if (p.path === session.projectPath) await session.refresh()
}

/**
 * 删一条。**就地确认，要打的是目录名。**
 *
 * 目录名：引擎两个名字都收（见 post_delete_project 的闸三），但界面一律
 * 说目录名——它是磁盘上的身份，也是唯一一个不会被改名改掉的串。
 *
 * 就地而不是 `prompt()`：原生对话框在同一页里弹第二次会带「阻止此页面创建
 * 更多对话框」的勾，勾上之后 prompt() 直接返回 null、代码当成「取消」，
 * 点删掉就是什么都不发生且这一整个会话都恢复不了。而这个入口存在的理由
 * 恰恰是**连着删好几个空壳**。
 */
function askRemove(p) {
  menuFor.value = ''
  removing.value = p.path
  confirmName.value = ''
}

async function remove(p) {
  const dir = p.dir || p.name
  if (confirmName.value.trim() !== dir) return
  const done = await run(
    () => api.deleteProject({ path: p.path, confirm_name: dir }),
    { key: 'delete:' + p.path, success: `「${p.name}」删掉了` },
  )
  if (!done) return
  removing.value = ''
  confirmName.value = ''
  // 删的是当前这一部才清上下文——删别的项目时人还在自己那部剧里干活。
  if (p.path === session.projectPath) session.clear()
  await store.load()
}

/**
 * 这一条正在跑吗。正在跑的项目删不掉、改名也会被工作线程整份写回时盖掉
 * （引擎两处都会回 409），**在人动手之前就说**，别等他一字不差打完目录名
 * 才弹一条红字。
 */
function busyProject(p) {
  // 不知道在跑哪个就一律当成"可能是它"——和引擎那道闸同一个方向（fail-closed）。
  // **只有真·问不到才算不知道**，问出来是空的不算，见 busyPaths 上面那段。
  if (busyPaths.value === null) return true
  return busyPaths.value.has(p.path)
}
</script>

<template>
  <aside
    class="rail"
    :class="{ 'rail--collapsed': collapsed, 'rail--left': ui.railSide === 'left' }"
  >
    <div class="rail__head">
      <button
        class="btn btn--ghost btn--sm rail__fold"
        type="button"
        :title="collapsed ? '展开项目库' : '收起项目库'"
        @click="collapsed = !collapsed"
      >
        <AppIcon
          :name="(ui.railSide === 'left') === collapsed ? 'arrowRight' : 'arrowLeft'"
          :size="15"
        />
      </button>
      <template v-if="!collapsed">
        <span class="rail__title">项目库</span>
        <span class="spacer" />
      </template>
      <!-- **一个项目都没有的时候，收起也要留着这个加号。**
           它是新建项目的唯一入口（AddProjectDialog 只从这儿开），而收起
           状态是记在 localStorage 里的：删掉最后一个项目之后，项目页那个
           空状态写着「点栏头的加号建一个」，栏头却只有一个箭头——提示指着
           一个不在屏幕上的东西。
           不是无条件常显：有项目时收起就是为了省地方，那时候列表和加号
           一起收走是对的；而列表为空时收起本来也省不下什么。 -->
      <button
        v-if="!collapsed || !store.count"
        class="btn btn--ghost btn--sm"
        type="button"
        title="加一个项目"
        @click="adding = true"
      >
        <AppIcon name="plus" :size="15" />
      </button>
    </div>

    <template v-if="!collapsed">
      <div class="rail__list">
        <p v-if="store.loading && !store.loaded" class="rail__note tiny dim">读取中…</p>
        <p v-else-if="store.error" class="rail__note tiny warn-text">{{ store.error }}</p>
        <p v-else-if="!store.count" class="rail__note tiny dim">
          还没有项目。点上面那个加号建一个。
        </p>

        <div
          v-for="p in shown"
          :key="p.path"
          class="item"
          :class="{
            'is-on': p.path === session.projectPath,
            'is-broken': p.broken,
            'is-menu': menuFor === p.path,
          }"
          role="button"
          tabindex="0"
          @click="pick(p)"
          @keydown="onKey(p, $event)"
        >
          <!-- 改名就地改：弹窗为一个输入框开一整个窗口不值当 -->
          <input
            v-if="renaming === p.path"
            ref="box"
            v-model="newName"
            class="input item__rename"
            @click.stop
            @keydown.stop.enter="commitRename(p)"
            @keydown.stop.esc="renaming = ''"
            @blur="commitRename(p)"
          />
          <span v-else class="item__name truncate">{{ p.name }}</span>

          <!-- **坏了的时候引擎是说了原因的，别丢掉。** `broken` 和
               `story_broken` 回的都是那一句异常原文（读不到就是空串），而
               栏上只画得下「读不了」「故事文件坏了」四五个字。没有这一行的
               话，人看到的是一句判决加一个没有下文——而那句原话往往就是
               答案（哪个文件、第几行、权限还是格式）。
               没坏的时候挂的是这一格自己那句话：窄栏上它常常被截断
               （「10 集里落成 3 集，还没分镜」）。 -->
          <span
            class="item__stage tiny truncate"
            :class="`item__stage--${projectStage(p).tone}`"
            :title="p.broken || p.story_broken || projectStage(p).label"
          >
            {{ projectStage(p).label }}
          </span>

          <!-- 这一条自己的那几件事。**@click.stop**：不拦的话点「删掉」
               会先冒泡到 pick()，把当前项目换成正要删的那个。 -->
          <button
            class="item__more"
            type="button"
            title="改名、删掉"
            @click.stop="toggleMenu(p)"
          >
            ⋯
          </button>
          <template v-if="menuFor === p.path">
            <div class="menu__veil" @click.stop="menuFor = ''" />
            <div class="menu__pop" :class="{ 'menu__pop--up': shown.indexOf(p) >= shown.length - 2 }" @click.stop>
              <button
                class="menu__item"
                type="button"
                :disabled="busyProject(p)"
                :title="busyProject(p) ? '正在跑，改完会被跑的那一头盖回去' : ''"
                @click="startRename(p)"
              >
                改名
              </button>
              <button
                class="menu__item menu__item--danger"
                type="button"
                :disabled="busyProject(p) || isBusy('delete:' + p.path)"
                :title="busyProject(p) ? '正在跑，跑完再删' : ''"
                @click="askRemove(p)"
              >
                删掉
              </button>
            </div>
          </template>

          <!-- 就地确认删除。**不用原生 prompt**，理由见 remove() 上面那段。 -->
          <div v-if="removing === p.path" class="item__confirm" @click.stop>
            <span class="tiny">照着打一遍目录名 <b class="mono">{{ p.dir || p.name }}</b> 才能删</span>
            <input
              v-model="confirmName"
              class="input item__rename"
              :placeholder="p.dir || p.name"
              @keydown.stop.enter="remove(p)"
              @keydown.stop.esc="removing = ''"
            />
            <div class="row">
              <button
                class="btn btn--danger btn--sm"
                type="button"
                :disabled="confirmName.trim() !== (p.dir || p.name) || isBusy('delete:' + p.path)"
                @click="remove(p)"
              >
                永久删除
              </button>
              <button class="btn btn--ghost btn--sm" type="button" @click="removing = ''">
                算了
              </button>
            </div>
          </div>
        </div>
      </div>
    </template>

    <AddProjectDialog :open="adding" @close="adding = false" />
  </aside>
</template>

<style scoped>
.rail {
  flex: none;
  width: 14rem;
  display: flex;
  flex-direction: column;
  min-height: 0;
  border-left: 1px solid var(--line);
  background: color-mix(in srgb, var(--surface) 60%, transparent);
}
/* 靠左时用 order 把它排到内容前面，边框也换一边。
   **不用 row-reverse**：那会把主内容区里所有 flex 行的方向一起翻过来。 */
.rail--left {
  order: -1;
  border-left: 0;
  border-right: 1px solid var(--line);
}
.rail--collapsed {
  width: auto;
}

.rail__head {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: var(--s3);
  border-bottom: 1px solid var(--line);
  flex: none;
}
.rail--collapsed .rail__head {
  border-bottom: 0;
}
.rail__title {
  /* ⚠️ **三个字的标题不能竖着排。** 这一行是 flex，默认每一项都可以被
     压缩（flex-shrink: 1）；窄屏上栏一收窄，这个 span 就被压到一个字宽，
     「项目库」当场折成三行。窄屏实测（375px）撞到过。
     不换行、宁可截断：一个「项目…」比竖排的三个字好读得多。 */
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  font-size: var(--fs-sm);
  font-weight: 600;
  color: var(--text-2);
}
.rail__fold {
  width: 26px;
  padding: 0;
}
.rail__list {
  flex: 1;
  min-height: 0;
  overflow-y: auto;
  padding: var(--s3);
  display: grid;
  gap: 2px;
  align-content: start;
}
.rail__note {
  padding: var(--s2);
  line-height: 1.6;
}

.item {
  position: relative;
  display: grid;
  grid-template-columns: 1fr auto;
  gap: 1px var(--s2);
  width: 100%;
  padding: var(--s2) var(--s3);
  border-radius: 9px;
  text-align: left;
  cursor: pointer;
}
.item:hover,
.item.is-menu {
  background: var(--surface-2);
}
.item.is-on {
  background: var(--accent-soft);
}
.item:focus-visible {
  outline: 2px solid var(--accent);
  outline-offset: -2px;
}
.item.is-broken {
  /* **不再整条淡出。** 淡出读起来是"禁用"，而坏项目恰恰是最该先看见、
     而且还点得动（点了会说读不了）。标红交给底下那句 --bad。 */
  border: 1px solid color-mix(in srgb, var(--danger) 40%, transparent);
}
.item__name {
  font-size: var(--fs-sm);
  color: var(--text);
}
.item.is-on .item__name {
  color: var(--accent);
  font-weight: 600;
}
.item__rename {
  /* **要盖住 .input 的 height: 34px**，不然点一下改名，下面所有行一起
     往下错位十几个像素。 */
  height: 24px;
  padding: 1px 4px;
  font-size: var(--fs-sm);
  min-width: 0;
}
/* 就地确认删除那一块。跨两列铺满，别挤在名字那一格里。 */
.item__confirm {
  grid-column: 1 / -1;
  display: grid;
  gap: 4px;
  margin-top: 4px;
  padding-top: 4px;
  border-top: 1px dashed color-mix(in srgb, var(--danger) 40%, transparent);
  cursor: default;
}
.item__stage {
  grid-column: 1;
  color: var(--text-3);
}
/* ⚠️ **projectStage 会返回 ok / warn / accent / bad / dim 五种 tone，
   五个都得有类。** 少一个就是一个不存在的选择器，静默落回上面那条灰，
   而且不报错。2026-09-14 之前只写了 ok 和 warn，实测那天库里 6 个项目
   没有一个命中这两个——这套配色一次都没生效过。 */
.item__stage--ok {
  color: var(--ok);
}
.item__stage--warn {
  color: var(--warn);
}
.item__stage--accent {
  color: var(--accent);
}
.item__stage--bad {
  color: var(--danger);
}
.item__stage--dim {
  color: var(--text-3);
}

/* 「⋯」。**只在悬停、聚焦或菜单开着时出现**——常驻的话六行右边挂六个
   点，而这条栏九成时间是拿来点一下换项目的。 */
.item__more {
  grid-row: 1 / span 2;
  grid-column: 2;
  align-self: center;
  width: 20px;
  padding: 0;
  border: 0;
  border-radius: 6px;
  background: transparent;
  color: var(--text-3);
  font-size: 15px;
  line-height: 1;
  cursor: pointer;
  opacity: 0;
}
.item:hover .item__more,
.item:focus-within .item__more,
.item.is-menu .item__more {
  opacity: 1;
}
.item__more:hover {
  color: var(--text);
  background: var(--surface);
}

.menu__veil {
  position: fixed;
  inset: 0;
  z-index: 60;
}
.menu__pop {
  position: absolute;
  right: var(--s2);
  top: calc(100% - 4px);
  z-index: 61;
  min-width: 6rem;
  padding: 4px;
  border: 1px solid var(--line);
  border-radius: 8px;
  background: var(--surface);
  box-shadow: 0 10px 30px rgb(0 0 0 / 30%);
  display: grid;
  gap: 2px;
}
/* **最后两条往上翻。** 列表是 overflow-y: auto 的滚动容器，而菜单是
   .item 的绝对定位子元素——向下开的话会被列表底边裁掉，偏偏按 rank 排序
   之后垫底的正是那几个空壳，也就是这个菜单最该用得上的地方。 */
.menu__pop--up {
  top: auto;
  bottom: calc(100% - 4px);
}
.menu__item {
  padding: 5px 8px;
  border: 0;
  border-radius: 6px;
  background: transparent;
  color: var(--text-2);
  font-size: var(--fs-sm);
  text-align: left;
  cursor: pointer;
}
.menu__item:hover:not(:disabled) {
  background: var(--surface-2);
  color: var(--text);
}
.menu__item--danger:not(:disabled) {
  color: var(--danger);
}
/* 禁用就变灰，别只是调淡——调淡的那一版看着像"能点，只是颜色浅"。
   和这一条菜单里「删掉」那种红字拉开距离靠的也是它。 */
.menu__item:disabled {
  color: var(--text-3);
  cursor: not-allowed;
}

/* 窄屏上收窄，但**不能 display:none**。
   原来是整条 `display: none`，而顶栏那个项目名在 860px 以下也
   `display: none`——两条注释各自把出路推给对方（这边写「走顶栏的项目名
   进项目页」，那边写「项目在第一步那一页」），而项目页根本没有项目列表。
   结果是 900px 以下完全换不了项目，861–900px 之间更尴尬：栏没了，只剩
   一个点过去没用的按钮。
   收窄就够：列表照常能点，人嫌占地方可以自己收起（那个箭头还在）。 */
@media (max-width: 900px) {
  .rail:not(.rail--collapsed) {
    width: 10rem;
  }
}
/* **10rem 已经是"展开"的下限了，手机上不能再往下收。**
   栏头那一行（折叠箭头 + 「项目库」 + 加号）连边距一共要 160px 上下，
   正好就是 10rem——再窄一点 flex 就开始压缩里面的项，实测 7.75rem 时
   折叠箭头和标题一起被压到 17px，「项目库」当场折成三行（标题那条
   white-space: nowrap 就是为这个加的，至少不再竖排）。
   所以手机上的出路不是把展开态改窄，是**默认就收起来**——见上面
   `collapsed` 那段。收起之后正文拿回整屏，而列表和加号一个都没少。 */
</style>
