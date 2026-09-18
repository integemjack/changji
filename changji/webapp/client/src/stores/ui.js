/** 界面级别的杂事：提示条、主题、项目栏靠哪边。 */

import { defineStore } from 'pinia'
import { ref, watch } from 'vue'

import { readLocal, writeLocal } from '@/composables/local-storage'

let seq = 0

export const useUi = defineStore('ui', () => {
  const toasts = ref([])
  // 和 session 那边同一个理由：这几句跑在 App.vue 挂载之前，裸着用
  // localStorage 的话，不给用的浏览器上整个界面是一片白。
  // 见 composables/local-storage.js 开头那段。
  const theme = ref(readLocal('changji.theme') || 'system')

  /**
   * 项目栏靠哪边。在设置页的「界面」里换（没有拖拽），**记在这台机器上**。
   *
   * 左右手习惯、外接屏幕的摆法、和别的工具的对齐方式，每个人不一样，
   * 而这是个一旦选定就不再动的偏好——不记住的话每次打开都要重拖一次。
   *
   * 认不出来的值一律当右边：localStorage 里可能是上一版写的东西，
   * 拿它去 CSS 里当类名会得到一个谁也不认识的类，界面上表现成栏不见了。
   */
  const railSide = ref(
    readLocal('changji.railSide') === 'left' ? 'left' : 'right',
  )
  watch(railSide, (value) => writeLocal('changji.railSide', value))

  /**
   * 专注模式：把顶栏和项目库收起来，稿纸铺满整个窗口。
   *
   * **这一条是抄来的，不是想出来的。** 写作类编辑器的共识是"文档占据整个
   * 视野，工具召之即来"——Sudowrite 把 AI 收在侧栏里，"鼓励你专注在写本身，
   * 把 AI 当成外科手术式的介入，而不是一直杵在那儿"；更狠的那些干脆让界面
   * 整个消失，鼠标移到屏幕边缘才浮出一条最小的菜单。
   *
   * 对我们这一页来说，最大的干扰**不在编辑器里，在编辑器外面**：右边那条
   * 项目库列着另外四部电影——你正在写第一章，旁边摆着"你还可以去干的别的
   * 事"。顶栏同理。
   *
   * **记在这台机器上**：喜欢专注写的人每次打开都该直接是专注的，而"每次
   * 都要先按一下"本身就是那句"你花在调工具上的时间，工具就成了干扰"。
   */
  const focusMode = ref(readLocal('changji.focusMode') === '1')
  watch(focusMode, (v) => writeLocal('changji.focusMode', v ? '1' : '0'))

  /**
   * 每个用大模型的地方摆不摆那颗「复制提示词」。
   *
   * 用户 2026-09-17 要的：「我可以复制放到别的地方生产后粘贴内容过来」，
   * 外加「增加一个统一的控制开关，后期可以直接关闭」。
   *
   * **默认开着**——他现在就要用。关掉之后所有那些按钮一起消失，一处改、
   * 全局生效，不用一页页去关。
   *
   * 记在这台机器上，和上面几项一个规矩：它是"我这台机器上怎么用"，
   * 不是"这部电影怎么拍"。
   */
  const showCopyPrompt = ref(readLocal('changji.showCopyPrompt') !== '0')
  watch(showCopyPrompt, (v) =>
    writeLocal('changji.showCopyPrompt', v ? '1' : '0'),
  )

  function applyTheme(value) {
    const root = document.documentElement
    if (value === 'system') root.removeAttribute('data-theme')
    else root.setAttribute('data-theme', value)
  }
  applyTheme(theme.value)
  watch(theme, (value) => {
    writeLocal('changji.theme', value)
    applyTheme(value)
  })

  function push(kind, text, ttl) {
    const id = ++seq
    toasts.value.push({ id, kind, text })
    // 报错留久一点。一闪而过的错误提示等于没提示。
    const ms = ttl ?? (kind === 'error' ? 8000 : 3200)
    setTimeout(() => dismiss(id), ms)
    return id
  }
  const ok = (text) => push('ok', text)
  const info = (text) => push('info', text)
  const warn = (text) => push('warn', text)
  const error = (text) => push('error', text)

  function dismiss(id) {
    const at = toasts.value.findIndex((t) => t.id === id)
    if (at >= 0) toasts.value.splice(at, 1)
  }

  return {
    toasts, theme, railSide, focusMode, showCopyPrompt,
    push, ok, info, warn, error, dismiss,
  }
})
