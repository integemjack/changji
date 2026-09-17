<script setup>
/**
 * 「复制提示词」。
 *
 * 用户 2026-09-17：「每个用大语言模型的地方增加一个复制提示词的按钮，
 * 我可以复制放到别的地方生产后粘贴内容过来」。
 *
 * **拿的是真发出去的那一份。** 引擎那头在 `req` 拼好之后、调模型之前把它
 * 原样回来（见 cpp/src/http/prompt_peek.hpp）——不是另拼一份给人看的。
 * 另拼的话两边迟早只改一边，而那种错没有任何报错：他在别处跑出来的东西
 * 粘回来解析不了，或者更糟，解析得了但内容不是他要的。
 *
 * 统一开关在 ui.showCopyPrompt（同一次要求里的第二句：「后期可以直接
 * 关闭」）。关掉之后这个组件整个不渲染，一处改、全局生效。
 */
import { ref } from 'vue'

import { api } from '@/api'
import AppIcon from '@/components/AppIcon.vue'
import { useUi } from '@/stores/ui'

const props = defineProps({
  /** 这一步的接口地址，和真跑那次同一条。 */
  path: { type: String, required: true },
  /** 那一条接口本来要的入参。这儿会再加一个 peek。 */
  payload: { type: Object, default: () => ({}) },
  /**
   * 入参要现算的时候给这个（可以是 async）。回 null 就当放弃，不报错。
   *
   * 拆分镜那一步要的是这一集的剧本，而剧本得先去引擎取一趟——摆不进
   * 一个静态的 payload 里。
   */
  resolve: { type: Function, default: null },
  /** 按钮上那句提示。留空就用默认那句。 */
  title: { type: String, default: '' },
  /** 摆成一个只有图标的小按钮。 */
  compact: { type: Boolean, default: false },
})

const ui = useUi()
const busy = ref(false)

async function copy() {
  if (busy.value) return
  busy.value = true
  try {
    const payload = props.resolve ? await props.resolve() : props.payload
    if (!payload) return
    const res = await api.peekPrompt(props.path, payload)
    const text = res?.prompt ?? ''
    if (!text) {
      ui.warn('这一步没拼出提示词来')
      return
    }
    try {
      await navigator.clipboard.writeText(text)
      ui.ok(`提示词抄好了，${text.length} 字`)
    } catch {
      // 没有剪贴板权限（http 下的非 localhost 就没有）。这段字几千上万个
      // 字符，弹出来没法用——**存成文件让他自己拿**比一个点了没反应的
      // 按钮强。
      const blob = new Blob([text], { type: 'text/plain;charset=utf-8' })
      const a = document.createElement('a')
      a.href = URL.createObjectURL(blob)
      a.download = `提示词-${res?.stage || 'prompt'}.txt`
      a.click()
      URL.revokeObjectURL(a.href)
      ui.info('剪贴板用不了，存成文件了')
    }
  } catch (e) {
    ui.error(`拿不到提示词：${e?.message || e}`)
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <button
    v-if="ui.showCopyPrompt"
    class="btn btn--ghost btn--sm"
    type="button"
    :disabled="busy"
    :title="title || '把这一步真正要发给大模型的那段字抄走，可以拿到别处去跑'"
    @click="copy"
  >
    <AppIcon name="script" :size="13" />
    <span v-if="!compact">{{ busy ? '取着…' : '复制提示词' }}</span>
  </button>
</template>
