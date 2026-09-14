/**
 * 参考图正在画成什么样——**一条订上就不断的线**。
 *
 * 用户 2026-09-12：「我刷新了这个页面，正在生成的图就不会实时更新」。
 *
 * **为什么会这样。** 进度和预览原来只走"这一次点击"那条 stream：id 是点
 * 下去的时候浏览器随机生成的（`ref-a3f9b1c2`），刷新之后那串字就没了，
 * 页面回来时既不知道有活在跑，也没法订上它。而这一族活是几十秒一张、
 * 一排十几张——刷新期间正好在跑是常态，不是边角情况。
 *
 * 现在引擎往一条**名字固定**的频道（`refs`）上也播一份，每条带着 `target`
 * 说这是哪一格（`char_id_slot` / `location_id_empty`）。这儿订上它，
 * 谁发起的、什么时候发起的都不重要。
 *
 * **模块级一份，不是每个组件一份。** 角色那格和场景那格都要用，各开一条
 * 等于同一份预览图收两遍——那可是几十 KB 一张、一秒好几张。
 */
import { reactive, ref } from 'vue'

import { openJobSocket } from '@/composables/useJobSocket'

/** target → 画到百分之几。0 表示"在跑但还没有步数"（多半在读权重）。 */
const pct = reactive({})
/** target → 采样到一半那张小图的 data URL。画完就删。 */
const preview = reactive({})
/** target → 正在画。**刷新之后就靠它**：收到任何一条消息就说明这一格在跑。 */
const live = reactive({})
/** 画完一张加一。页面 watch 它去重新拉图。 */
const finished = ref(0)
/**
 * target → 这一格重画过几次。**缩略图的换代号。**
 *
 * 参考图写在固定路径上（`refs/<char_id>_<slot>.png`），重画是原地覆盖、
 * 地址一个字不变——而 `/api/media` 一个缓存头都不发，浏览器于是照旧拿缓存
 * 里那张。表现是：点了「重画」，等了几十秒，提示条说画好了，**墙上那张
 * 脸一点没变**，除非整页刷新。这一页的产出就是图，这一下等于功能没生效。
 *
 * 镜头墙早就为同一件事记了换代号（useShots 的 bust / bustOf），朗读和试听
 * 音色也各自加了时间戳，只有参考图这一族漏了。
 *
 * 按 target 记，不是记一个全局的：一键出图是一张接一张十几张，全局的话
 * 每画完一张就要把满墙的图全重拉一遍。
 */
const drawn = reactive({})
/**
 * 不走 ref_done 的那几下（传图、撤图、重新定妆）改过之后加一。
 *
 * 那几下可能一次换掉好多张（定妆会把整份资产库换掉），而且是人自己按的、
 * 一次一下，所以这里用一个全局的数，让所有图一起换代。
 */
const touched = ref(0)

let sock = null
let retry = null

function forget(target) {
  delete pct[target]
  delete preview[target]
  delete live[target]
}

function connect() {
  // ⚠️ **进来先把排着的那次重连掐掉。**
  //
  // 断线时这儿会排一个 5 秒后的 `connect`。而这 5 秒里只要有组件挂载
  // （换到设定页就会发生——三个格子都调 `useRefStream()`），那个
  // `if (!sock) connect()` 会当场再连一条；随后排着的那次照样到点，
  // **又连一条，而前一条既没关也没人记着**。
  //
  // 后果不是多一条闲连接：两条都订着 refs，引擎每画完一张图播一条
  // `ref_done`，于是 `finished` 一次加二——三个页面各把 /api/assets
  // 重拉两遍。再断一次就再多一条，越积越多。
  //
  // 实测过：断线 → 期间挂载 → 到点重连，三条 socket 里两条活着，
  // 一张图画完 finished 加 2。
  clearTimeout(retry)
  retry = null
  sock = openJobSocket(
    'refs',
    (msg) => {
      const t = msg.target
      if (!t) return
      if (msg.type === 'ref_progress') {
        live[t] = true
        pct[t] = msg.total > 0 ? Math.round((msg.current / msg.total) * 100) : 0
      } else if (msg.type === 'ref_preview') {
        live[t] = true
        preview[t] = msg.image ?? ''
      } else if (msg.type === 'ref_done') {
        forget(t)
        drawn[t] = (drawn[t] ?? 0) + 1
        finished.value += 1
      } else if (msg.type === 'ref_error') {
        forget(t)
      }
    },
    () => {
      // 断了就当没有活在跑。**留着旧的更糟**：那一格会永远显示"画着…"，
      // 而那张图可能早就画完了。
      sock = null
      for (const t of Object.keys(live)) forget(t)
      clearTimeout(retry)
      retry = setTimeout(connect, 5000)
    },
  )
}

/**
 * 资产库变了但不是画完一张（定妆、删人）——也让订着 `finished` 的页面重拉。
 * 复用同一个计数器而不是再开一个：三个格子都已经 watch 着它，
 * "资产库有变化去重拉"对它们来说是同一件事。
 */
/**
 * 把"正在画"那三张表全松开。**换项目时叫一次。**
 *
 * 它们按 target 索引，而 target 是 `char_id_slot`——**两部剧里出现同一个
 * id 不稀奇**（id 照名字生成，续集、复制出来的项目、同名角色都会撞，项目
 * 库那条栏的注释里也写着这件事）。不清的话，新这一部里同名那一格顶着上一
 * 部的「画着…」和那张采样中的小图，而这一部根本没在画。
 *
 * `drawn`（换代号）**不在这儿清**：它是单调加一的，清了之后回到上一部，
 * 那几张重画过的图会退回旧地址、又从缓存里拿到老图。撞上同名 id 至多多拉
 * 一次图，比那个便宜。
 *
 * 真正干净的做法是引擎在 refs 那条频道上带一个项目路径、界面按它过滤
 * （批量写作那条 `story_token` 就是这么修的）——那要动 ref_progress /
 * ref_preview / ref_done / ref_error 四处签名。先把这一半修了。
 */
function forgetAll() {
  for (const t of Object.keys(live)) forget(t)
}

function touch() {
  touched.value += 1
  finished.value += 1
}

/**
 * 这一格的图该用哪一代。地址后面挂上它，浏览器才会真去要新的那张。
 * 用法见角色墙和场景墙上的 `<img :src>`。
 */
function bustOf(target) {
  return touched.value + (drawn[target] ?? 0)
}

export function useRefStream() {
  // **只连一次。** 两个 tab（角色、场景）都要用，而它们会来回切。
  if (!sock) connect()
  return { pct, preview, live, finished, touch, bustOf, forgetAll }
}
