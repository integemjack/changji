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
import { nextTick, ref } from 'vue'

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
   * 拆分镜那一步要的是这一章的剧本，而剧本得先去引擎取一趟——摆不进
   * 一个静态的 payload 里。
   */
  resolve: { type: Function, default: null },
  /** 按钮上那句提示。留空就用默认那句。 */
  title: { type: String, default: '' },
  /** 摆成一个只有图标的小按钮。 */
  compact: { type: Boolean, default: false },
  /**
   * 连「粘回结果」一起摆。
   *
   * 复制出去和粘回来是同一件事的两半——用户的原话是"复制放到别的地方生产后
   * 粘贴内容过来"。只给复制不给粘，那半程就走不完。
   */
  pasteable: { type: Boolean, default: false },
  /** 粘贴框里那句额外的说明。拆分镜要说清"每一场之间留着那行分隔头"。 */
  pasteHint: { type: String, default: '' },
})

const emit = defineEmits(['done'])

const ui = useUi()
const busy = ref(false)
/** 粘贴框开着没有。 */
const pasteOpen = ref(false)
const pasted = ref('')
const sending = ref(false)
const box = ref(null)

function openPaste() {
  pasted.value = ''
  pasteOpen.value = true
  nextTick(() => box.value?.focus())
}

async function submitPaste() {
  const raw = pasted.value.trim()
  if (!raw || sending.value) return
  sending.value = true
  try {
    const payload = props.resolve ? await props.resolve() : props.payload
    if (!payload) return
    // **走这一步自己那条接口**，只是把"找模型要那段原文"换成这段。
    // 解析和守卫一道不少：整章没对白、正文复读那几道照样拦，粘回来的
    // 东西和模型自己写的走同一条路。
    const res = await api.applyPrompt(props.path, payload, raw)
    ui.ok('收下了')
    pasteOpen.value = false
    emit('done', res)
  } catch (e) {
    // 引擎那头的话原样给他看——「正文只写出 18 个字，至少要 600 个」
    // 这种正是他需要知道的，换成"粘贴失败"等于把理由吃掉。
    ui.error(e?.message || String(e))
  } finally {
    sending.value = false
  }
}

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
  <button
    v-if="ui.showCopyPrompt && pasteable"
    class="btn btn--ghost btn--sm"
    type="button"
    title="把在别处跑出来的那段结果粘回来。解析和守卫一道不少，和模型自己写的走同一条路"
    @click="openPaste"
  >
    <AppIcon name="upload" :size="13" />
    <span v-if="!compact">粘回结果</span>
  </button>

  <Teleport to="body">
    <div v-if="pasteOpen" class="modal" @click.self="pasteOpen = false">
      <section class="card modal__panel paste">
        <h2 class="h3">把结果粘回来</h2>
        <p class="tiny dim">
          贴模型吐出来的那段 JSON 原文，整段贴，别删花括号。
          它走的是和真跑同一条解析和守卫——不合格一样会被打回，理由照说。
        </p>
        <p v-if="pasteHint" class="tiny dim">{{ pasteHint }}</p>
        <textarea
          ref="box"
          v-model="pasted"
          class="textarea mono paste__box"
          rows="14"
          placeholder="{ … }"
        />
        <div class="row">
          <button
            class="btn btn--primary"
            type="button"
            :disabled="!pasted.trim() || sending"
            @click="submitPaste"
          >
            {{ sending ? '收着…' : '收下这一份' }}
          </button>
          <button class="btn btn--ghost" type="button" @click="pasteOpen = false">
            取消
          </button>
          <span class="spacer" />
          <span class="tiny dim numeric">{{ [...pasted].length }} 字</span>
        </div>
      </section>
    </div>
  </Teleport>
</template>

<style scoped>
.paste {
  display: grid;
  gap: var(--s3);
  width: min(52rem, 92vw);
}
.paste__box {
  /* 贴进来的是几千字的 JSON，框小了等于让人在一条缝里对花括号 */
  min-height: 18rem;
  resize: vertical;
}
</style>
