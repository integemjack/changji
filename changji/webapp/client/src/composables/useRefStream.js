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

let sock = null
let retry = null

function forget(target) {
  delete pct[target]
  delete preview[target]
  delete live[target]
}

function connect() {
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

export function useRefStream() {
  // **只连一次。** 两个 tab（角色、场景）都要用，而它们会来回切。
  if (!sock) connect()
  return { pct, preview, live, finished }
}
