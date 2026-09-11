/** 界面级别的杂事：提示条、主题、项目栏靠哪边。 */

import { defineStore } from 'pinia'
import { ref, watch } from 'vue'

let seq = 0

export const useUi = defineStore('ui', () => {
  const toasts = ref([])
  const theme = ref(localStorage.getItem('changji.theme') || 'system')

  /**
   * 项目栏靠哪边。可以拖到另一边去，**记在这台机器上**。
   *
   * 左右手习惯、外接屏幕的摆法、和别的工具的对齐方式，每个人不一样，
   * 而这是个一旦选定就不再动的偏好——不记住的话每次打开都要重拖一次。
   *
   * 认不出来的值一律当右边：localStorage 里可能是上一版写的东西，
   * 拿它去 CSS 里当类名会得到一个谁也不认识的类，界面上表现成栏不见了。
   */
  const railSide = ref(
    localStorage.getItem('changji.railSide') === 'left' ? 'left' : 'right',
  )
  watch(railSide, (value) => localStorage.setItem('changji.railSide', value))

  function applyTheme(value) {
    const root = document.documentElement
    if (value === 'system') root.removeAttribute('data-theme')
    else root.setAttribute('data-theme', value)
  }
  applyTheme(theme.value)
  watch(theme, (value) => {
    localStorage.setItem('changji.theme', value)
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

  return { toasts, theme, railSide, push, ok, info, warn, error, dismiss }
})
