<script setup>
/**
 * 这一部剧。
 *
 * **这一页只显示答案，改东西一律在弹窗里。** 2026-09-14 重排的规矩：
 * 每次打开这一页都要回答的问题，才配留在页面上；其余三条出路——改的时候
 * 才要的进弹窗，出问题才要的条件出现，别处已经说了的删掉。
 *
 * 按这条过完，原来那 41 件东西剩 5 件：这是哪部剧（logline）、到哪一步了
 * （进度条）、这部片子长什么样（一行答案）、拿哪几个模型跑（四个名字）、
 * 删掉它。整页从 1826px 压到一屏以内。
 *
 * 砍掉的里面值得记的几条：
 *   · `0 集 0 镜 0 成片` —— 进度条和阶段文字已经说完了，而且还没到那一步
 *     时三个 0 是噪音不是信息；
 *   · `19 小时前` —— 没有任何决定依赖它；
 *   · 完整路径 —— 一年用一次（找文件），却天天占半行，换成一个 📁；
 *   · 「写实线」标签 —— 画风那段文字自己就写着"实拍摄影"。
 *
 * **模型那一行不在 hasProject 分支里**：一台刚装好的机器上一个项目都没有，
 * 而那正是最需要挑模型的时候。初始化页 2026-09-14 删了（用户：「不需要
 * 初始化页面」），这一行就是它的替代品——没有项目时整页只有它和"新建项目"，
 * 看不见反而难。
 */
import { computed, onMounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import ModelDialog from '@/components/ModelDialog.vue'
import ShowDialog from '@/components/ShowDialog.vue'
import { api } from '@/api'
import { qualitySize } from '@/api/labels'
import { projectStage } from '@/composables/project-stage'
import { useAction } from '@/composables/useAction'
import { useModels } from '@/stores/models'
import { useProjects } from '@/stores/projects'
import { pickProjectHint } from '@/composables/pick-project-hint'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const store = useProjects()
const session = useSession()
const models = useModels()
const ui = useUi()
const { run, isBusy } = useAction()

const removing = ref(false)
const confirmName = ref('')
const showOpen = ref(false)
const modelKey = ref('') // 打开的是哪一组的弹窗，空串是没开

/** 项目库里这一条。统计和阶段都从它来，和右边那条栏读的是同一份。 */
const me = computed(() => store.byPath(session.projectPath))
const stage = computed(() => (me.value ? projectStage(me.value) : null))
/**
 * 目录名。删除确认按它——见模板里那段。
 *
 * ⚠️ **不能回落到 project_id。** 那是个 slug（`text::project_slug` 生成），
 * 和目录名是两个字段，语料里就有不相等的例子（目录「项目_雨夜天台」、
 * project_id「yuye-tiantai」）。拿它冒充目录名，等于把刚修好的那个死结
 * 挪到 fallback 分支上：照着提示打完，引擎照样 400。
 * 列表还没读到时（me 为空）就从路径末段取。
 */
const dirName = computed(() => {
  if (me.value?.dir) return me.value.dir
  const path = session.projectPath || ''
  return path.split(/[\\/]+/).filter(Boolean).pop() || ''
})

/** 「这部片子」那一行的答案：画幅 · 尺寸 · 画风开头一句。 */
const show = ref(null)
/** show 里那份是哪部剧读回来的。换剧时用来决定要不要先擦掉。 */
let shownFor = null

async function loadShow() {
  const want = session.projectPath
  // **换了一部剧，先把上一部的答案擦掉。**
  //
  // 下面那句 want !== projectPath 只挡住了"回来晚了别乱写"，挡不住这段
  // 空当里屏幕上印着什么：这一行只在 show 为空时才写「读取中…」，不擦的
  // 话从 A 点到 B 的一两秒里，B 的行上**明晃晃写着 A 的画幅和画风**，而
  // 且没有任何标记说它是旧的——和上面 orientation 那段是同一条道理：
  // 宁可说"还不知道"，不能理直气壮地报一个别处的值。
  //
  // 弹窗保存后的那一趟（@saved）want 和 shownFor 相等，不擦，不闪。
  if (want !== shownFor) {
    show.value = null
    shownFor = want
  }
  if (!want) return
  // 这一趟是给哪部剧读的。在项目库里连着点两部，两趟都在路上，回来的顺序
  // 不保证——资产库（style 从那儿来）比 video 那一趟大得多，慢的那趟后
  // 落地就把**上一部**的画幅和画风写在这一部的行上，而这一行是这一页仅剩
  // 的五件东西之一，错了没有别处对得出来。
  const [v, a] = await Promise.allSettled([
    api.projectVideo(want),
    api.assets(want),
  ])
  if (want !== session.projectPath) return
  const video = v.status === 'fulfilled' ? v.value : null
  const style = a.status === 'fulfilled' ? a.value.style : null
  show.value = {
    // **读不出来就别说「竖屏」。**
    //
    // 这里原来是 `video?.orientation === 'landscape' ? '横屏' : '竖屏'`：
    // 接口挂了、项目在别处被删了的时候 `video` 是 null，这个三目稳稳落到
    // "竖屏"——于是这一行**信誓旦旦地报了一个它根本没读到的值**，而在横屏
    // 项目上那就是一句假话。资产库那边同一件事写着「显示一个跑的时候根本
    // 不会用的值，比不显示更糟」。
    orientation: video
      ? video.orientation === 'landscape'
        ? '横屏'
        : '竖屏'
      : '画幅读不出来',
    size: video ? qualitySize(video.quality, video.orientation) : '',
    look: style?.global_style || '',
  }
}

async function remove() {
  const path = session.projectPath
  const done = await run(
    () => api.deleteProject({ path, confirm_name: dirName.value }),
    { key: 'delete', success: '已删除' },
  )
  if (!done) return
  removing.value = false
  confirmName.value = ''
  session.clear()
  await store.load()
}

/**
 * 把项目目录的路径抄到剪贴板。
 *
 * 原来这儿是页面上印着的一长串路径——一年用一次（找文件），却天天占半行。
 * **不做"在文件管理器里打开"**：那要引擎加一个接口，而为一个图标去开一条
 * 能执行 shell 的路不值当。抄走自己去粘，够用。
 */
async function copyPath() {
  const path = session.projectPath
  try {
    await navigator.clipboard.writeText(path)
    ui.ok('路径抄好了')
  } catch {
    // 没有剪贴板权限（http 下的非 localhost 就没有）。把路径显示出来，
    // 让他自己选——总比一个点了没反应的图标强。
    ui.info(path)
  }
}

onMounted(() => {
  if (!store.loaded) store.load()
  models.load()
})

watch(() => session.projectPath, loadShow, { immediate: true })
</script>

<template>
  <!-- 根元素的类名不能叫 .proj：子组件的根会带上父组件的 scoped 属性，
       App.vue 里给顶栏项目按钮写的 .proj { max-width: 11rem } 会套到这儿，
       整页被压成 176px 宽。栽过一次。 -->
  <div class="pj">
    <template v-if="session.hasProject">
      <!-- 这是哪部剧 -->
      <div class="head">
        <!-- 没写故事时这一行直接不渲染。原来兜底印一句「还没写故事」，
             而栏里那条和下面进度条说的是同一件事，空话不占一行。 -->
        <p v-if="me?.logline" class="lead">{{ me.logline }}</p>
        <span v-else class="spacer" />
        <button class="btn btn--ghost btn--sm" type="button" title="抄走路径" @click="copyPath">
          <AppIcon name="folder" :size="14" />
        </button>
        <button
          class="btn btn--ghost btn--sm"
          :class="{ 'danger-text': !removing }"
          type="button"
          @click="removing = !removing"
        >
          {{ removing ? '算了' : '删掉' }}
        </button>
      </div>

      <div v-if="removing" class="row row--wrap confirm-row">
        <!-- ⚠️ **要打的是目录名，不是剧名。** 引擎比的是目录名
             （post_delete_project 的闸三），而这儿原来比的是 title——
             一个目录叫 convenience-store、剧名叫「深夜便利店」的项目，
             按钮要你打剧名才解锁，打完提交引擎回 400 要目录名，这条路
             彻底堵死。引擎现在两个都收，界面统一说目录名：它是磁盘上的
             身份，也是唯一不会被改名改掉的那个串。 -->
        <input
          v-model="confirmName"
          class="input confirm"
          :placeholder="'照着打一遍目录名「' + dirName + '」'"
        />
        <button
          class="btn btn--danger btn--sm"
          type="button"
          :disabled="confirmName !== dirName || isBusy('delete')"
          @click="remove"
        >
          永久删除
        </button>
      </div>

      <!-- 到哪一步了。
           **只剩进度条，那句话删了**：项目库那条栏里高亮的这一条写的是
           同一个函数、同一份数据算出来的同一句，两处在 /project 上左右
           并排。空壳项目上更难看——栏里四行「还没写故事」，这儿一句，
           logline 兜底再一句，一屏三遍。 -->
      <div v-if="stage" class="progress" :title="stage.label">
        <div class="bar">
          <div
            class="bar__fill"
            :class="`bar__fill--${stage.tone}`"
            :style="{ width: stage.percent + '%' }"
          />
        </div>
      </div>

      <!-- 这部片子长什么样：一行答案，✎ 开弹窗 -->
      <button class="line" type="button" @click="showOpen = true">
        <span class="line__k">这部片子</span>
        <span class="line__v truncate">
          <template v-if="show">
            <!-- 分隔符跟着后一段走：读不出画幅时 size 是空的，写死的
                 「 · 」会在行尾留一个没有下文的点。 -->
            {{ show.orientation }}<template v-if="show.size"> · {{ show.size }}</template><template v-if="show.look"> · {{ show.look }}</template>
          </template>
          <span v-else class="dim">读取中…</span>
        </span>
        <AppIcon name="wand" :size="14" class="line__go" />
      </button>
    </template>

    <!-- 拿哪几个模型跑。**没有项目也显示**，理由见文件开头。

         **这一行一半是剧的、一半是机器的**，所以标「这部剧」。
         挑哪一档（`[models.pick]`）记进这部剧的 changji.toml，跟着项目
         目录走，派到别的机器上也照这份来；而那一档的**文件在这台机器的
         哪儿**是机器的属性，走全局配置——项目模板里那句话说的是后者：
         「机器的属性（模型文件、显存…）在全局配置里，不要写到这儿——
         否则项目目录拷到另一台机器就跑不起来」。
         （2026-09-15 这一行先标过「（本机）」，那时候挑的那一档确实只写
         全局。现在挑的那一半跟项目走了，标签跟着改。） -->
    <div class="line line--static">
      <span
        class="line__k"
        title="挑哪一档是这部剧的设置，记在项目里、跟着项目目录走，派到别的机器上也照这份来；那一档的文件放在这台机器的哪儿是机器的设置，走全局配置"
        >模型<span class="tiny dim scope">（这部剧）</span></span
      >
      <span class="line__v names">
        <button
          v-for="m in models.inUse"
          :key="m.key"
          class="name"
          :class="{ 'name--missing': m.missing }"
          type="button"
          :title="m.title + (m.missing ? '（还没下全）' : '')"
          @click="modelKey = m.key"
        >
          {{ m.name }}<span v-if="m.missing" class="warn">⚠</span>
        </button>
        <!-- **读不到就得说读不到。** `models.error` 一直没人读（和早先
             `session.error` 同一个毛病），于是 /bff/setup/state 一挂——引擎
             在重启、端口不对、回了 500——这一行就永远停在「读取中…」上，
             一个字都不解释。而没有项目时整页只有这一行（见文件开头：它是
             初始化页删掉之后的替代品），一台刚装好的机器上正好撞见。 -->
        <span v-if="models.error" class="tiny danger-text truncate" :title="models.error">
          读不到模型清单：{{ models.error }}
        </span>
        <span v-else-if="!models.inUse.length" class="dim tiny">读取中…</span>
      </span>
    </div>

    <EmptyState
      v-if="!session.hasProject"
      icon="folder"
      tone="warn"
      title="还没选项目"
      :hint="pickProjectHint(store)"
    />

    <ShowDialog :open="showOpen" @close="showOpen = false" @saved="loadShow" />
    <ModelDialog :open="!!modelKey" :group-key="modelKey" @close="modelKey = ''" />
  </div>
</template>

<style scoped>
.pj {
  display: flex;
  flex-direction: column;
  gap: var(--s2);
  padding-bottom: var(--s4);
}

.head {
  display: flex;
  align-items: flex-start;
  gap: var(--s2);
}

.lead {
  flex: 1;
  margin: 0;
  font-size: var(--fs-md);
  line-height: 1.6;
}

.confirm-row {
  margin-top: calc(var(--s2) * -1);
}

.confirm {
  max-width: 22rem;
}

.progress {
  display: flex;
  align-items: center;
  gap: var(--s2);
}

.bar {
  flex: 1;
  height: 4px;
  border-radius: 999px;
  background: var(--line);
  overflow: hidden;
}

.bar__fill {
  height: 100%;
  background: var(--accent);
}
/* ⚠️ **五个 tone 都得有类。** 模板里拼的是 `bar__fill--${stage.tone}`，
   而这几个 modifier 2026-09-14 之前一个都没定义——拼出来是不存在的选择器，
   全落回上面那条橙。阶段那句话删掉之后（栏里已经写着同一句），颜色是这条
   进度条**唯一**还能区分"跑完了"和"读不了"的通道，不能再是同一个橙。

   **是五个不是四个**：`projectStage` 返回 ok / warn / accent / bad / dim
   （project-stage.js 里那六处 return 数一遍就有），项目栏那份
   `.item__stage--*` 五个都写齐了、注释也写的五个，这儿漏了 accent。
   漏了之所以一直看不出来，是因为上面 `.bar__fill` 的底色正好就是
   `var(--accent)`——**靠巧合对上的**。哪天底色一改，"正在做"那一档就
   跟着变成别的颜色，而且照旧不报错。补上，让这张表真的是满的。 */
.bar__fill--accent {
  background: var(--accent);
}
.bar__fill--ok {
  background: var(--ok);
}
.bar__fill--warn {
  background: var(--warn);
}
.bar__fill--bad {
  background: var(--danger);
}
.bar__fill--dim {
  background: var(--text-3);
}

/* 一行答案。整行可点——点哪儿都是"我要改这个"。 */
.line {
  display: flex;
  align-items: center;
  gap: var(--s2);
  width: 100%;
  padding: 8px 10px;
  border: 1px solid var(--line);
  border-radius: 8px;
  background: transparent;
  color: var(--text);
  text-align: left;
  cursor: pointer;
}

.line:hover {
  border-color: var(--accent);
}

/* 模型那行整行不可点——可点的是里面每一个名字 */
.line--static {
  cursor: default;
}

.line--static:hover {
  border-color: var(--line);
}

/* 「（本机）」这种范围标记：比键名淡一档，别抢它的位置。 */
.scope {
  margin-left: 2px;
  font-weight: 400;
}
.line__k {
  flex: 0 0 4.5rem;
  /* 没有 --text-dim（是 --text-2 / --text-3）。别处的 .field__label 用的
     就是 --text-2，这一行是同一类标签。 */
  color: var(--text-2);
  font-size: 12px;
}

.line__v {
  flex: 1;
  min-width: 0;
  font-size: 13px;
}

.line__go {
  flex: 0 0 auto;
  color: var(--text-3);
}

.names {
  display: flex;
  flex-wrap: wrap;
  gap: 4px 10px;
}

.name {
  padding: 2px 6px;
  border: 1px solid transparent;
  border-radius: 6px;
  background: transparent;
  color: var(--text);
  font-size: 13px;
  cursor: pointer;
}

.name:hover {
  border-color: var(--accent);
}

.name--missing {
  color: var(--warn, #f5a524);
}

.warn {
  margin-left: 3px;
}
</style>
