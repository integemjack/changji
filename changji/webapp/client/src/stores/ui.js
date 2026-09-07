/** 界面级别的杂事：提示条、主题、侧栏开合。 */

import { defineStore } from 'pinia'
import { ref, watch } from 'vue'

let seq = 0

export const useUi = defineStore('ui', () => {
  const toasts = ref([])
  const railOpen = ref(false) // 手机上的抽屉
  const theme = ref(localStorage.getItem('changji.theme') || 'system')

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

  return { toasts, railOpen, theme, push, ok, info, warn, error, dismiss }
})
