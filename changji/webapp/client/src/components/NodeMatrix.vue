<script setup>
/**
 * 「机器 × 能力」那张表。
 *
 * 一行一台机器（**本机也是一行，不是特例**），一列一个能力，格子四样：
 *
 *   淡虚框 干不了——缺模型、没编进去、没有 ffmpeg。悬停看原因
 *   深虚圈 能干，但**配置文件**里关掉的（`[[peer.nodes]]` 的 off），点不动
 *   深实圈 能干，你在这儿关的。点一下打开
 *   实心   参与自动调度
 *
 * 虚线一律是"点不动"，深浅分的是"能不能干"。**点得动和点不动必须一眼
 * 分得出**：长得一样的话，用户会在一个点不动的格子上反复点（引擎那头
 * `NodeState::off_locked` 的注释写的就是这句）。
 *
 * **三态是引擎算好的，界面不自己推。** 推的话迟早和调度器的判断对不上，
 * 而那种对不上表现为"表上说能派，跑起来说没有可用节点"。
 *
 * 格子能点：点一下关掉／打开。存在 `<项目库>/nodes.json`，不碰
 * config.toml——配置里那份 `off` 是**部署时定的**，界面上显示成锁着的
 * （`locked`），要改得去动配置文件。两处取并集，任一处关了就是关了。
 *
 * 展开一行能看那台的模型：缺哪几组、一键装成和本机同一套、下到哪儿了。
 * **同一套是要紧的**：种子跨机一致这件事挡不住模型不同——种子相同、
 * 模型不同，出来的就是两张脸，而那表现为一章里画风在某几镜跳一下。
 */
import { computed, onMounted, onUnmounted, ref } from 'vue'
import AppIcon from '@/components/AppIcon.vue'
import { api } from '@/api'
// 「这部电影挑了哪一档」记在项目里。下面那几处问模型状态都要带上它，
// 否则问到的是本机全局那一档——见 api.nodeSetup 那条注释。
import { useSession } from '@/stores/session'

const session = useSession()

const data = ref(null)
const error = ref('')
const loading = ref(false)
/** 正在提交的那个格子，`url|cap`。同一时刻只让点一个。 */
const pending = ref('')

/** 展开的那一行（节点地址），空 = 都收着。 */
const opened = ref('')
/** 展开那台的模型状态，key 是节点地址。 */
const setup = ref({})
/** 那台的下载进度，key 是节点地址。 */
const progress = ref({})
const busyNode = ref('')
/** 正在改的那一行（节点地址），空 = 没在改。 */
const editing = ref('')
const editUrl = ref('')
const editToken = ref('')
/** 点了删、还没确认的那一台（地址）。空 = 没在问。 */
const confirming = ref('')
/** 那一台的完整信息，弹窗里要拿它的名字。 */
const confirmNode = computed(
  () => (data.value?.nodes ?? []).find((n) => n.url === confirming.value) ?? null,
)
let timer = null
let pollTimer = null

/**
 * 顺手问一遍每台在不在下模型。
 *
 * **不能只在抽屉展开时问。** 原来的 pollProgress 第一句就是
 * `if (opened.value !== url) return`，而 `opened` 每次挂载都归零——
 * 于是刷新一下页面，那台正在下的 82 GB 就彻底没了踪影：行上不显示，
 * 汇总那几句也只说"一台都派不出去"，人没有任何办法知道它在下、下到哪儿了，
 * 除非恰好想起来去点那一行的「模型」。一趟下载是按小时算的，这中间
 * 任何一次刷新都会把它藏起来。
 *
 * 搭在 15 秒那一趟上，不另起轮询：那几台可能正在出片（见 onMounted 里
 * 那句注释），多一条读内存状态的请求是它能承受的，多一个 2 秒的轮询不是。
 */
async function refreshProgress(nodes) {
  await Promise.all(
    (nodes ?? [])
      .filter((n) => !n.local && n.online)
      .map(async (n) => {
        try {
          const p = await api.nodeSetupProgress(n.url)
          progress.value = { ...progress.value, [n.url]: p }
        } catch {
          // 问不到就当这台没在下，15 秒后再问。这一趟不该因为它报错——
          // 机器表本身是好的。
        }
      }),
  )
}

/** 下载的总进度。行上那颗小标和抽屉里那行读数共用一份算法。 */
function dlSummary(p) {
  if (p?.state !== 'running') return null
  const done = p.downloaded ?? 0
  const total = p.total ?? 0
  const mb = (n) => n / 1024 / 1024
  // **速度掉到近零时那个 eta 不能照印。**
  //
  // 引擎算的是 剩余字节 / 当前速度。速度是瞬时值，重启引擎、网络抖一下、
  // 一个文件刚下完还没接上下一个，它都会短暂地掉到接近 0——而分母一小，
  // 商就炸了。实测截到过一屏「还要 533374 小时 59 分」，那时速度显示
  // 0.0 MB/s。数字本身没算错，是这一格不该把它当成一句话说出来。
  //
  // 两道闸：速度小到没意义就不提剩余时间；算出来超过一天也不提——
  // 这套流水线的模型最大的一档也就几十 GB，真要下一天以上，那句
  // 「还要 N 小时」帮不上任何忙，只会让人以为程序算错了（它确实像）。
  const kSlow = 64 * 1024        // 64 KB/s 以下当作"这会儿没在动"
  const kTooLong = 24 * 3600     // 超过一天就不报了
  const usable =
    p.speedBps > kSlow && p.etaSeconds > 0 && p.etaSeconds < kTooLong
  const eta = usable ? Math.round(p.etaSeconds / 60) : 0
  return {
    pct: total ? Math.round((done / total) * 100) : 0,
    size: total ? `${(done / 1024 ** 3).toFixed(1)} / ${(total / 1024 ** 3).toFixed(1)} GB` : '',
    speed: p.speedBps > kSlow ? `${mb(p.speedBps).toFixed(1)} MB/s` : '',
    eta: !eta ? '' : eta < 60 ? `还要 ${eta} 分钟` : `还要 ${Math.floor(eta / 60)} 小时 ${eta % 60} 分`,
  }
}

/**
 * 装下这张新表，**形状不对就不装**。
 *
 * 引擎正常时这五条路（问一遍、点格子、加、改、删）回的都是整张表。而
 * api 那层在拿到 200 + 空响应体时回的是 `{}`（`return data ?? {}`，给不看
 * 返回值的调用方兜底）——引擎重启那一下正好撞得上。直接装进去的话，
 * 屏幕上那张表会凭空消失一轮；更早以前是整页崩掉（见模板里 `v-if` 那段）。
 *
 * 装不下就当这一趟没发生：上一份还摆在那儿，下一轮 15 秒后自己就对了。
 */
function setTable(next) {
  if (next?.nodes) data.value = next
}

/**
 * 每台那份读数，算一次。
 *
 * **不要在模板里到处写 `dlSummary(progress[n.url])`**：那样同一个对象
 * 每次渲染要重算九遍（行上那颗标三次、抽屉里那行六次），而它还要走
 * 除法和字符串拼接。进度是两秒一变的，这九遍每两秒重来一次。
 * 算在这儿，progress 不变就不重算。
 */
const dl = computed(() => {
  const out = {}
  for (const [url, p] of Object.entries(progress.value)) out[url] = dlSummary(p)
  return out
})

async function load() {
  loading.value = true
  try {
    setTable(await api.nodes())
    error.value = ''
    await refreshProgress(data.value?.nodes)
  } catch (err) {
    // 说清是"问这几台机器"这一趟砸了。光一句原始报错的话，它孤零零挂在
    // 标题底下，看着像整个设置页出了问题——这一块的别的动作（开关一个
    // 格子、装模型）共用同一行，那几条自己带上下文。
    error.value = `问不到这几台机器：${err.message}`
  } finally {
    loading.value = false
  }
}

onMounted(() => {
  load()
  // 十五秒一次。**别更勤**：每次都要去问别的机器，而那几台正在出片。
  timer = setInterval(load, 15000)
})
onUnmounted(() => {
  clearInterval(timer)
  clearInterval(pollTimer)
})

/**
 * 点一个格子。
 *
 * **回来的就是整张新表**，直接换掉——自己在前端推一遍"点了之后该长什么样"
 * 的话，迟早和引擎算的不一致，而那种不一致表现为"点完看着关了，跑起来
 * 还是派给它"。
 */
async function toggle(node, cap) {
  if (cap.locked || !cap.able) return
  if (pending.value) return
  pending.value = `${node.url}|${cap.cap}`
  try {
    setTable(await api.setNodeOff(node.url, cap.cap, !cap.off))
    error.value = ''
  } catch (err) {
    error.value = err.message
  } finally {
    pending.value = ''
  }
}

/** 展开／收起一行，顺手把那台的模型状态拉回来。 */
async function openRow(node) {
  if (opened.value === node.url) {
    opened.value = ''
    return
  }
  opened.value = node.url
  if (!setup.value[node.url]) await loadSetup(node.url)
  pollProgress(node.url)
}

async function loadSetup(url) {
  busyNode.value = url
  try {
    setup.value = {
      ...setup.value,
      [url]: await api.nodeSetup(url, session.projectPath),
    }
    error.value = ''
  } catch (err) {
    error.value = err.message
  } finally {
    busyNode.value = ''
  }
}

/**
 * 让这台装成和这部电影要的同一套。
 *
 * **标准是「这部电影挑的那一档」，不是「本机装着哪一档」。** 拿它的
 * selected 原样发过去——让用户在这儿再挑一遍的话，两台挑得不一样就是
 * 画风跳，而那要到成片才看得出来。
 *
 * 问本机那一份时带上项目：`[models.pick]` 记在项目里，不带的话拿到的是
 * 本机全局配着的那一档。派活时带给对面的是项目那一档（见 worker_pool
 * 的 pick），两者不同的话，给对面装的和真要用的就不是一个东西——
 * 而表现要到那一镜被对面拒了才看得出来。
 */
async function matchLocal(url) {
  const mine =
    setup.value.local ?? (await api.nodeSetup('local', session.projectPath))
  setup.value = { ...setup.value, local: mine }
  const selections = mine?.selected ?? {}
  if (!Object.keys(selections).length) {
    error.value = '本机自己还没选定模型，先把本机那套配好'
    return
  }
  busyNode.value = url
  try {
    await api.nodeSetupDownload(url, selections)
    error.value = ''
    pollProgress(url)
  } catch (err) {
    error.value = err.message
  } finally {
    busyNode.value = ''
  }
}

/**
 * 那颗按钮到底照谁装。**同一颗按钮有两个意思，得说出来是哪一个。**
 *
 * 开着项目时照的是这部电影挑的那一档（`[models.pick]`，也正是派活时带给
 * 对面的那一档）；没开项目时照的是本机全局配着的那一档。两者可以不同，
 * 而装错了的表现要到那一镜被对面拒了才看得出来。
 */
const matchLabel = computed(() =>
  session.projectPath ? '装成这部电影要的那一套' : '装成和本机同一套',
)
const matchHint = computed(() =>
  session.projectPath
    ? '拿这部电影挑的那一档原样装过去——派活时带给对面的就是它。同一套模型是画风一致的前提'
    : '没开项目，照的是本机全局配着的那一档。同一套模型是画风一致的前提',
)

/**
 * 加一台别的机器。
 *
 * **在这之前这件事只能手改配置文件**——而这张表就摆在这儿，上面每台机器
 * 每个能力都能点，唯独"这张表从哪儿来"得去翻文档、找到配置文件、记住
 * `[[peer.nodes]]` 这个写法（用户 2026-09-15 提的）。
 */
const adding = ref(false)
const newUrl = ref('')
const newToken = ref('')

function openAdd() {
  adding.value = true
  newUrl.value = ''
  newToken.value = ''
  error.value = ''
}

async function addNode() {
  const url = newUrl.value.trim()
  if (!url) return
  busyNode.value = 'add'
  try {
    // 引擎那头加完会**当场问一遍**这台在不在、能干什么，回的就是整张表。
    // 所以这儿直接换上，不用再 load() 一次。
    setTable(await api.addNode(url, newToken.value.trim()))
    error.value = ''
    adding.value = false
  } catch (err) {
    // **不关那个框。** 地址填错是最常见的一种失败，关掉的话人得从头再敲
    // 一遍——而他要改的可能只是一个字符。
    error.value = err.message
  } finally {
    busyNode.value = ''
  }
}

function startEdit(node) {
  confirming.value = ''
  if (editing.value === node.url) {
    editing.value = ''
    return
  }
  editing.value = node.url
  editUrl.value = node.url
  // **口令不回填。** 引擎不会把它发到前端来（它在配置里，见
  // /api/nodes 那边只回 url），这里拿不到明文；留空提交时也不动它，
  // 想换才填。下面那句 placeholder 说的就是这件事。
  editToken.value = ''
}

async function saveEdit(node) {
  const next = { new_url: editUrl.value.trim() }
  // 只有真填了才带 token 这个键过去——不带就是"不动"，见 api/index.js。
  if (editToken.value.trim()) next.token = editToken.value.trim()
  busyNode.value = node.url
  try {
    setTable(await api.updateNode(node.url, next))
    if (opened.value === node.url) opened.value = ''
    editing.value = ''
    editToken.value = ''
    error.value = ''
  } catch (err) {
    error.value = err.message
  } finally {
    busyNode.value = ''
  }
}

async function removeNode(node) {
  busyNode.value = node.url
  try {
    setTable(await api.removeNode(node.url))
    if (opened.value === node.url) opened.value = ''
    if (editing.value === node.url) editing.value = ''
    confirming.value = ''
    error.value = ''
  } catch (err) {
    error.value = err.message
  } finally {
    busyNode.value = ''
  }
}

async function cancel(url) {
  try {
    await api.nodeSetupCancel(url)
    error.value = ''
    // **停完要再问一遍**，别让那一块继续写着「正在下 3 个文件」。
    // 轮询这会儿可能已经不在了——问不到三次就会放手（见 pollProgress）
    // ——那样的话点完停下什么都不会变。重新起一趟，它会拿到 cancelled
    // 然后自己收尾。
    pollProgress(url)
  } catch (err) {
    error.value = err.message
  }
}

/**
 * 连着问不到几次才算真断了。**一次不算**：那头在下几十 GB 的模型，
 * 一趟几分钟到几十分钟，中间引擎重启一下、网络抖一下都很正常。
 */
const kProgressMisses = 3

/** 下载进度。两秒一次，下完就停——**别一直问**，那几台正在出片。 */
function pollProgress(url) {
  clearInterval(pollTimer)
  let misses = 0
  const tick = async () => {
    if (opened.value !== url) {
      clearInterval(pollTimer)
      return
    }
    try {
      const p = await api.nodeSetupProgress(url)
      progress.value = { ...progress.value, [url]: p }
      misses = 0
      if (p?.state !== 'running') {
        clearInterval(pollTimer)
        // 下完了模型就变了，那台能干什么也跟着变
        if (p?.state === 'done') {
          await loadSetup(url)
          await load()
        }
      }
    } catch (err) {
      // **一次问不到不等于下载停了。** 这儿原来是一次失败就
      // `clearInterval` 而且一个字不说——之后那一格永远停在最后一次
      // 的进度上（「正在下 3 个文件 41%」），不动、不报错，看着像卡死，
      // 而那头多半还在好好地下。
      misses += 1
      if (misses < kProgressMisses) return
      clearInterval(pollTimer)
      error.value = `问不到这台的下载进度了：${err.message}。那头可能还在下，收起这一行再展开一次就重新问`
    }
  }
  tick()
  pollTimer = setInterval(tick, 2000)
}

function gb(bytes) {
  if (!bytes) return ''
  return `${(bytes / 1024 / 1024 / 1024).toFixed(1)} GB`
}

/** 格子的样子。四样之外，离线那台整行压暗。 */
function cellClass(cap, node) {
  if (!cap.able) return 'cell cell--cant'
  // **配置关的和你关的要长得不一样。** 引擎特意分了这两种（NodeState 的
  // `off_locked`：「两种显示成一样的话，用户会在一个点不动的格子上反复
  // 点」），而这儿原来只拿它去 disable 按钮——屏幕上两种一模一样，差别
  // 只有鼠标形状和一句要等一会儿才冒出来的悬停提示。
  if (cap.off) return cap.locked ? 'cell cell--off cell--locked' : 'cell cell--off'
  if (!node.online) return 'cell cell--cant'
  return 'cell cell--on'
}

function cellTitle(cap, node) {
  if (!cap.able) return `${cap.label}：干不了。${cap.why || ''}`
  if (cap.locked) return `${cap.label}：配置文件里关掉的，要改得去动 [[peer.nodes]] 的 off`
  if (cap.off) return `${cap.label}：关着。点一下打开`
  if (!node.online) return `${cap.label}：这台连不上`
  return `${cap.label}：参与自动调度。点一下关掉`
}
</script>

<template>
  <div class="matrix">
    <div class="matrix__head">
      <h3 class="matrix__t">这几台机器能产什么</h3>
      <span class="spacer" />
      <!-- **这件事以前只能手改配置文件。** 表就在这儿，每台每个能力都能
           点，唯独"这张表从哪儿来"得去翻文档。 -->
      <button
        class="btn btn--ghost btn--sm"
        type="button"
        :disabled="loading || adding"
        @click="openAdd"
      >
        加一台机器
      </button>
      <button
        class="iconbtn"
        type="button"
        :disabled="loading"
        title="重新问一遍（每台最多等 3 秒）"
        @click="load"
      >
        <AppIcon name="refresh" :size="14" />
      </button>
    </div>

    <!-- 加一台。**摆在表上面而不是弹窗**：填完之后立刻要看那一行亮没亮，
         中间隔一层遮罩就得关掉再看。 -->
    <form v-if="adding" class="add" @submit.prevent="addNode">
      <input
        v-model="newUrl"
        class="input mono add__url"
        placeholder="http://192.168.1.20:9101"
        autofocus
      />
      <!-- 口令**不是必填**：对面 `[peer].token` 空着（只听回环）或者和本机
           那台配的是同一个时都不用填。填了就只给这一台用。 -->
      <input
        v-model="newToken"
        class="input mono add__token"
        type="password"
        placeholder="口令（对面 [peer].token；留空 = 用全局那个）"
      />
      <button class="btn btn--sm btn--primary" type="submit" :disabled="!newUrl.trim() || busyNode === 'add'">
        {{ busyNode === 'add' ? '连着…' : '加上' }}
      </button>
      <button class="btn btn--sm btn--ghost" type="button" @click="adding = false">
        算了
      </button>
      <p class="add__hint tiny dim">
        对面要以 <code>--worker --host 0.0.0.0</code> 起着，并且设了
        <code>[peer].token</code>。加上之后这张表会立刻去问它一遍，连不上也
        先记下来——那台开机之后自己就亮了。
      </p>
    </form>

    <p v-if="error" class="alert alert--bad">
      <AppIcon name="warn" :size="15" />
      {{ error }}
    </p>

    <!-- **「还没问完」和「没有别的机器」是两回事。** 这一问要挨个去连，
         每台最多等 3 秒，几台加起来十几秒是常事；这段时间里原来整块只剩
         一个标题和底下那句说明，看着就像"就本机一台、没别的"。而真到了
         那种时候，表里至少还有本机那一行。 -->
    <p v-if="loading && !data" class="tiny dim">问着…（每台最多等 3 秒）</p>

    <!-- 表单独装在一个能横滚的盒子里。**不这么做的话它会把整张设置页顶宽**
         ——六列中文表头加一颗按钮，min-content 357.5px，而 375px 上那一栏
         只有 282。溢出的部分被外层 `.main__scroll` 那个 overflow-x 吞掉：
         页面横着能推，屏幕上什么都不说，而「重新体检」「保存」这些都被推到
         侧边那条栏底下。 -->
    <!-- **判据是"有没有这份机器列表"，不是"有没有拿到响应对象"。**
         api 那层拿到一个 200 + 空响应体时回的是 `{}`（见 api/index.js 的
         `return data ?? {}`，那是给不看返回值的调用方兜底的）。`{}` 是真值，
         原来这儿写 `v-if="data"` 就放行，底下 `data.nodes[0]` 当场抛
         「Cannot read properties of undefined (reading '0')」，整张设置页
         被 ErrorBoundary 换成一张崩溃卡。
         **空响应不是假想的**：设置页每 15 秒问一次，引擎重启那一下正好
         落在这个窗口里，实测撞到过。 -->
    <div v-if="data?.nodes" class="matrix__scroll">
      <table class="matrix__grid">
      <thead>
        <tr>
          <th class="col-name">机器</th>
          <th v-for="c in data.nodes[0]?.capabilities ?? []" :key="c.cap">
            {{ c.label }}
          </th>
          <th class="col-act" />
        </tr>
      </thead>
      <tbody>
        <template v-for="n in data.nodes" :key="n.url">
          <tr :class="{ off: !n.online }">
            <td class="col-name">
              <!-- **名字和地址一样就不印两遍。** 连不上的那一行，引擎那边
                   `n.name` 回落成配置里的 url（连上了才换成它自报的名字），
                   于是同一格里地址上下各一份，下面那句真正要看的报错反而
                   被挤到第三行去。 -->
              <span class="nmrow">
                <span v-if="n.name && n.name !== n.url" class="nm">{{ n.name }}</span>
              <!-- 「本机」是身份，「连不上／忙」是状态，**两件事各走各的**。
                   原来三个串在一条 v-if/v-else-if 上，本机那一行永远停在第
                   一个分支——于是本机的「忙」一次都没亮过，而本机恰恰是最
                   常在跑的那一台（`local_exec().busy()`，引擎每次都算了给
                   过来）。 -->
              <span v-if="n.local" class="pill pill--neutral tiny">本机</span>
              <span v-if="!n.online" class="pill pill--warn tiny">连不上</span>
              <span v-else-if="n.busy" class="pill pill--ok tiny">忙</span>
              <!-- 一台多卡机同时收得下几件（它自己报的 `slots`：拉起了几个
                   子进程）。单卡机不印——满屏「1 张卡」没有信息量；印出来
                   是为了「只用了一张卡」这种事在表上一眼能看出是哪一环少了。 -->
              <span v-if="n.online && n.slots > 1" class="pill pill--neutral tiny"
                    title="这台同时收几件活：一张卡一个子进程，派活按这个数开位">{{ n.slots }} 张卡</span>
              </span>
              <!-- **地址单独一行。** 和名字挤在一行的时候，机器名一长
                   （真主机名二十几个字符是常事）这一列就把右边那几颗
                   按钮顶出可视区，而地址本身也只能省略号收尾。 -->
              <span class="url mono tiny" :title="n.url">{{ n.url }}</span>
              <span v-if="n.error" class="err tiny">{{ n.error }}</span>
            </td>
            <td v-for="c in n.capabilities" :key="c.cap" class="col-cap">
              <button
                type="button"
                class="cellbtn"
                :class="{ 'cellbtn--locked': c.locked || !c.able }"
                :disabled="!c.able || c.locked || pending !== ''"
                :title="cellTitle(c, n)"
                @click="toggle(n, c)"
              >
                <span :class="cellClass(c, n)" />
              </button>
            </td>
            <td class="col-act">
              <button
                class="btn btn--ghost btn--sm icon"
                :class="{ 'is-on': opened === n.url }"
                type="button"
                :disabled="!n.online"
                :title="
                  !n.online
                    ? '连不上，看不了'
                    : opened === n.url
                      ? '收起'
                      : '看这台装了哪些模型，也从这儿装'
                "
                @click="openRow(n)"
              >
                <AppIcon name="download" :size="15" />
              </button>
              <!-- **收着的时候也要看得见它在下。** 见 refreshProgress 上面
                   那段：一趟下载按小时算，而抽屉默认是收着的。 -->
              <span
                v-if="dl[n.url]"
                class="pill pill--warn tiny"
                :title="`正在下模型：${dl[n.url].size} · ${dl[n.url].speed} · ${dl[n.url].eta}`"
              >
                下 {{ dl[n.url].pct }}%
              </span>
              <!-- **下砸了也要在这一行看得见。**
                   「正在下」那颗 2026-09-15 加了，失败这一档当时漏了——
                   提示只写在抽屉里，而抽屉默认是收着的。实测：Qwen-Image
                   基础版下到 102 GB 报 failed，这一行什么都不显示，
                   `下 99%` 那颗也一起消失了，屏幕上看起来就像下完了。

                   **怎么办那句不要自己再写一遍**：引擎那条 error 里已经带了
                   （「重来一次会从断点接着下」），拼上去就是同一句话在一个
                   气泡里出现两次，中间还多一个句号。这儿只补引擎不知道的
                   那一半——去哪儿看明细。 -->
              <span
                v-else-if="progress[n.url]?.state === 'failed'"
                class="pill pill--bad tiny"
                :title="`下载失败：${progress[n.url].error || ''} 点「模型」那一格看是哪几个文件。`"
              >
                下载失败
              </span>
              <!-- **本机没有这两颗**：它不是配置里加进来的一台，改不了也删不掉。 -->
              <button
                v-if="!n.local"
                class="btn btn--ghost btn--sm icon"
                :class="{ 'is-on': editing === n.url }"
                type="button"
                :disabled="busyNode === n.url"
                title="改这台的地址或口令"
                @click="startEdit(n)"
              >
                <AppIcon name="pencil" :size="15" />
              </button>
              <!-- **删要二次确认，而且确认就在这一行里问。** 删掉之后这台
                   的地址和口令都要重填一遍，而这颗按钮就挨着「改」——
                   点错一格的代价不该是"没了"。 -->
              <button
                v-if="!n.local"
                class="btn btn--ghost btn--sm icon icon--danger"
                type="button"
                :disabled="busyNode === n.url"
                title="从配置里删掉这台"
                @click="confirming = n.url"
              >
                <AppIcon name="trash" :size="15" />
              </button>
            </td>
          </tr>

          <!-- 改地址／口令。**摆在这一行底下而不是弹窗**，理由同上面那个
               「加一台」：改完要立刻看这一行亮没亮。 -->
          <tr v-if="editing === n.url" class="drawer">
            <td :colspan="(n.capabilities?.length ?? 5) + 2">
              <form class="edit" @submit.prevent="saveEdit(n)">
                <label class="edit__f">
                  <span class="tiny dim">地址</span>
                  <input
                    v-model="editUrl"
                    class="input mono"
                    placeholder="http://192.168.1.20:9101"
                  />
                </label>
                <label class="edit__f">
                  <span class="tiny dim">口令</span>
                  <input
                    v-model="editToken"
                    class="input mono"
                    type="password"
                    placeholder="留空 = 不改"
                  />
                </label>
                <button
                  class="btn btn--primary btn--sm"
                  type="submit"
                  :disabled="busyNode === n.url || !editUrl.trim()"
                >
                  {{ busyNode === n.url ? '存着…' : '存下' }}
                </button>
                <button
                  class="btn btn--ghost btn--sm"
                  type="button"
                  @click="editing = ''"
                >
                  取消
                </button>
              </form>
            </td>
          </tr>

          <tr v-if="opened === n.url" class="drawer">
            <td :colspan="(n.capabilities?.length ?? 5) + 2">
              <div v-if="busyNode === n.url" class="tiny dim">问着…</div>
              <div v-else-if="setup[n.url]" class="setup">
                <div class="setup__groups">
                  <span
                    v-for="g in setup[n.url].groups ?? []"
                    :key="g.key"
                    class="pill tiny"
                    :class="g.satisfied ? 'pill--ok' : 'pill--warn'"
                    :title="g.purpose"
                  >
                    {{ g.title }}{{ g.satisfied ? '' : ' 缺' }}
                  </span>
                  <span class="tiny dim">
                    盘上还剩 {{ gb(setup[n.url].diskFreeBytes) }}
                  </span>
                </div>

                <div v-if="progress[n.url]?.state === 'running'" class="dl">
                  <span class="tiny">
                    正在下
                    {{ (progress[n.url].items ?? []).filter((i) => i.state === 'running').length }}
                    个文件
                  </span>
                  <!-- **总数、速度、还要多久。** 引擎一直在发
                       downloaded / total / speedBps / etaSeconds 这四个，
                       而这儿原来一个都没读——只有每个文件各自的百分比，
                       于是"还要多久"这个唯一真正想知道的事，屏幕上没有答案。 -->
                  <span class="tiny dim">
                    {{ dl[n.url].size }}
                    （{{ dl[n.url].pct }}%）
                    <template v-if="dl[n.url].speed">
                      · {{ dl[n.url].speed }}
                    </template>
                    <template v-if="dl[n.url].eta">
                      · {{ dl[n.url].eta }}
                    </template>
                  </span>
                  <button class="btn btn--ghost btn--sm" type="button" @click="cancel(n.url)">
                    停下
                  </button>
                  <ul class="dl__items">
                    <li
                      v-for="it in (progress[n.url].items ?? []).filter((i) => i.state === 'running')"
                      :key="it.name"
                      class="tiny mono"
                    >
                      {{ it.name }}
                      {{ it.total ? Math.round((it.downloaded / it.total) * 100) : 0 }}%
                    </li>
                  </ul>
                </div>

                <div v-else class="setup__acts">
                  <button
                    v-if="!n.local"
                    class="btn btn--sm"
                    type="button"
                    :disabled="busyNode === n.url"
                    :title="matchHint"
                    @click="matchLocal(n.url)"
                  >
                    {{ matchLabel }}
                  </button>
                  <span v-if="progress[n.url]?.state === 'failed'" class="tiny warn-text">
                    上次下载失败：{{ progress[n.url].error }}
                  </span>
                  <span v-else-if="progress[n.url]?.state === 'done'" class="tiny">
                    下完了
                  </span>
                </div>
              </div>
            </td>
          </tr>
        </template>
        </tbody>
      </table>
    </div>

    <ul v-if="data?.summary" class="sum">
      <li v-for="s in data.summary" :key="s.cap" :class="{ bad: s.count === 0 }">
        <b>{{ s.label }}</b>
        <span v-if="s.count > 0">{{ s.count }} 台可用</span>
        <span v-else class="why">{{ s.why }}</span>
      </li>
    </ul>

    <!-- 这是那张表的读法。**表不在就别摆**——读不出来的时候它孤零零挂在
         一句报错底下，讲的是一个屏幕上根本没有的东西。 -->
    <!-- **图例要跟着格子的样子改。** 2026-09-15 把「干不了」从一个几乎
         看不见的淡虚线圈换成了一道短横，这段话原来写的是"虚线的都点不动、
         淡的那种是干不了"——照着找的人会在屏幕上找不到那种圈。 -->
    <p v-if="data?.nodes" class="tiny dim">
      点格子关掉或打开。<b>一道短横</b>是那台干不了这一步——
      <b>能不能干是它自己量出来的</b>，要去装模型或者换一份编进了 sd.cpp
      的二进制；<b>虚线圈</b>是配置文件里关掉的（<code>[[peer.nodes]]</code>
      的 <code>off</code>），改它得去动那个文件。这两种都点不动。
      实心是参与调度，空心圈是你在这儿关掉的。
    </p>
  </div>

  <!-- 删之前问一句。**做成弹窗而不是行内**：这一行里已经有五个能点的
       格子加三颗按钮，确认条塞进去会把「机器」那一列挤窄、整张表横着
       溢出去；而删掉之后地址和口令都得重填一遍，值得盖住别的东西问一次。 -->
  <div v-if="confirmNode" class="mask" @click.self="confirming = ''">
    <section class="dlg dlg--ask" role="alertdialog" aria-modal="true">
      <header class="dlg__head">
        <h2 class="dlg__t">删掉这台机器？</h2>
      </header>
      <div class="dlg__body">
        <p class="ask__who">
          <b>{{
            confirmNode.name && confirmNode.name !== confirmNode.url
              ? confirmNode.name
              : '这台'
          }}</b>
          <span class="mono tiny dim">{{ confirmNode.url }}</span>
        </p>
        <p class="tiny dim">
          它会从配置里去掉，地址和口令都不再留着——再要用得重新加一遍。
          那台上已经下好的模型不会动。
        </p>
      </div>
      <footer class="dlg__foot">
        <span class="spacer" />
        <button class="btn btn--ghost" type="button" @click="confirming = ''">取消</button>
        <button
          class="btn btn--danger"
          type="button"
          :disabled="busyNode === confirmNode.url"
          @click="removeNode(confirmNode)"
        >
          {{ busyNode === confirmNode.url ? '删着…' : '删除' }}
        </button>
      </footer>
    </section>
  </div>
</template>

<style scoped>
.matrix {
  display: flex;
  flex-direction: column;
  gap: 8px;
  /* 让它能缩到比里面那张表窄。少了这一句，min-content 会一路往上顶，
     上面那个 overflow-x 就永远轮不到。 */
  min-width: 0;
}
/* 横着放不下就在这儿滚，别把整页顶宽。 */
.matrix__scroll {
  min-width: 0;
  overflow-x: auto;
}
.matrix__head {
  display: flex;
  align-items: center;
  gap: 8px;
}
/* 加一台那一行。挤不下就折——地址框本来就长，再加口令和两颗按钮，
   窄一点的设置页上一行放不下。 */
.add {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--s2);
  padding: var(--s3);
  border: 1px dashed var(--line-strong);
  border-radius: var(--r);
  background: var(--bg-sunken);
}

.add__url {
  flex: 2 1 15rem;
  min-width: 0;
}

.add__token {
  flex: 1 1 11rem;
  min-width: 0;
}

.add__hint {
  flex-basis: 100%;
  margin: 0;
  line-height: 1.6;
}

.matrix__t {
  font-size: 14px;
  margin: 0;
}
.matrix__grid {
  border-collapse: collapse;
  width: 100%;
  font-size: 13px;
}
.matrix__grid th {
  text-align: center;
  font-weight: 500;
  padding: 4px 6px;
  opacity: 0.7;
  /* **两个字的表头不许折。** 表格是 auto 布局，而「机器」那一格里的地址
     不可断行——机器名一长（2026-09-15 起那里放的是真主机名，不再是
     一律「未命名」），这一列就撑到 349px，五个能力列各剩 32px，
     于是「写文」竖着排成「写／文」，整排表头高一倍。
     钉住不折之后，让位的是「机器」那一列，它底下本来就能换行。 */
  white-space: nowrap;
}
.matrix__grid th.col-name,
.matrix__grid td.col-name {
  text-align: left;
  width: 36%;
}
.matrix__grid td {
  padding: 6px;
  border-top: 1px solid var(--line, #e5e5e5);
  vertical-align: middle;
}
.matrix__grid tr.off {
  opacity: 0.55;
}
.col-cap {
  text-align: center;
}
.col-act {
  text-align: right;
  white-space: nowrap;
}
/* 名字那一行：名字 + 「本机」「连不上」「忙」几个标，地址另起一行。 */
.nmrow {
  display: flex;
  align-items: center;
  gap: 6px;
  flex-wrap: wrap;
}
.nm {
  font-weight: 500;
}
/* 三颗图标按钮。**只留图标**，说明走 title。 */
.icon {
  width: 30px;
  padding: 0;
  display: inline-flex;
  align-items: center;
  justify-content: center;
}
.icon--danger:hover:not(:disabled) {
  color: var(--danger);
  border-color: color-mix(in srgb, var(--danger) 40%, transparent);
}
.pill--bad {
  color: var(--danger);
  border-color: color-mix(in srgb, var(--danger) 45%, transparent);
  background: var(--danger-soft);
}
.mask {
  position: fixed;
  inset: 0;
  z-index: 80;
  display: grid;
  place-items: center;
  padding: var(--s3, 10px);
  background: rgb(0 0 0 / 45%);
}
.dlg--ask {
  width: min(420px, 100%);
  display: flex;
  flex-direction: column;
  border: 1px solid var(--line);
  border-radius: 12px;
  background: var(--surface);
  box-shadow: 0 20px 60px rgb(0 0 0 / 35%);
}
.dlg--ask .dlg__head,
.dlg--ask .dlg__foot {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 10px 14px;
}
.dlg--ask .dlg__head {
  border-bottom: 1px solid var(--line);
}
.dlg--ask .dlg__foot {
  border-top: 1px solid var(--line);
}
.dlg--ask .dlg__t {
  margin: 0;
  font-size: var(--fs-md);
  font-weight: 600;
}
.dlg--ask .dlg__body {
  padding: 14px;
  display: flex;
  flex-direction: column;
  gap: 8px;
}
.ask__who {
  margin: 0;
  display: flex;
  flex-direction: column;
  gap: 2px;
}
.edit {
  display: flex;
  align-items: flex-end;
  gap: var(--s3, 10px);
  flex-wrap: wrap;
}
.edit__f {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 12rem;
  flex: 1 1 12rem;
}
.url {
  opacity: 0.5;
  margin-left: 6px;
  /* **挤的时候让地址先省略，别把右边那两颗按钮顶出去。** 地址不可断行，
     而机器名从 2026-09-15 起是真主机名（可能二十几个字符），两个加起来
     把「机器」那一列撑到把「模型」「不用了」推出可视区——那两颗是这一行
     仅有的操作，而外面那层是 overflow-x，推出去就得先横滚才点得到。
     名字本身留全：现在它才是认人的那一半，地址鼠标悬停看得到。 */
  display: block;
  margin-left: 0;
  margin-top: 1px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.err {
  display: block;
  color: var(--warn, #b45309);
  margin-top: 2px;
}
.cellbtn {
  background: none;
  border: 0;
  padding: 4px;
  cursor: pointer;
  line-height: 0;
}
.cellbtn--locked,
.cellbtn:disabled {
  cursor: not-allowed;
}
.cell {
  display: inline-block;
  width: 12px;
  height: 12px;
  border-radius: 50%;
}
/* 干不了：一道短横，就是"这一格没有这回事"。
   **原来是 `1px dashed var(--line)` 再加 opacity .5 的空圈，看不见。**
   `--line` 本来就是整套里最淡的那个分隔线色，再打对折，12px 的圈落在
   深色底上几乎是一片空白——而这一格要说的是一件正经事（这台干不了这一步），
   一整列都这样的时候，人看到的是"这里什么都没有"，不是"都干不了"。
   靠深浅区分没有余量了（再淡就没有，再深就和下面那两档撞），所以改成
   **换形状**：横杠=没这回事，圈=有这回事但关着。虚线这个语汇于是只剩
   「配置文件关的、你点不动」一个意思，比原来清楚。 */
.cell--cant {
  width: 10px;
  height: 2px;
  border-radius: 1px;
  background: var(--text-3, #888);
  opacity: 0.75;
}
/* 能干但关着：空心圈 */
.cell--off {
  border: 1.5px solid var(--text-2, #555);
  opacity: 0.85;
}
/* 同上，但是**配置文件**关的，点不动：改成虚线。
   这张表里虚线一律是"点不动"（干不了那一档也是虚线），深浅分的是
   "能不能干"——所以配置关的是**深色虚线**：能干，但你在这儿改不了它。 */
.cell--locked {
  border-style: dashed;
}
/* 参与调度：实心 */
.cell--on {
  background: var(--ok, #16a34a);
}
.drawer td {
  background: var(--bg-soft, rgba(127, 127, 127, 0.06));
}
.setup {
  display: flex;
  flex-direction: column;
  gap: 8px;
}
.setup__groups {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px;
}
.setup__acts {
  display: flex;
  align-items: center;
  gap: 10px;
}
.dl {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px;
}
.dl__items {
  list-style: none;
  margin: 0;
  padding: 0;
  flex-basis: 100%;
  opacity: 0.8;
}
.sum {
  list-style: none;
  padding: 0;
  margin: 0;
  display: flex;
  flex-wrap: wrap;
  gap: 4px 14px;
  font-size: 12px;
}
.sum li {
  opacity: 0.8;
}
.sum li b {
  margin-right: 4px;
}
.sum li.bad {
  opacity: 1;
  color: var(--warn, #b45309);
  flex-basis: 100%;
}
.why {
  opacity: 0.9;
}
</style>
