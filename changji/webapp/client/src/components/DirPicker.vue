<script setup>
/**
 * 挑一个文件夹。
 *
 * **为什么要有它**：模型动辄几十 GB，人挑的是"哪块盘还装得下"，而那个路径
 * 多半在另一个窗口里。原来那一格只能手敲——敲错一个字的后果不是报错，是
 * 下载落到一个你没在看的地方。
 *
 * **只列目录，不列文件**（引擎那条 `/api/fs/dirs` 就只回目录）：这儿唯一的
 * 用处是选一个放模型的地方，把几百个 .gguf 一起列出来只会把目录淹掉。
 *
 * **手敲那条路留着。** 这个浏览器走的是引擎那台的文件系统——引擎跑在别的
 * 机器上时（或者要填一个还不存在的目录），敲路径仍然是唯一的办法。所以它
 * 是那个输入框旁边的一颗按钮，不是它的替代品。
 */
import { ref, watch } from 'vue'

import { api } from '@/api'
import AppIcon from '@/components/AppIcon.vue'
import { useAction } from '@/composables/useAction'

const props = defineProps({
  open: { type: Boolean, default: false },
  /** 打开时从哪儿起步。空串就让引擎给几个起点。 */
  start: { type: String, default: '' },
})
const emit = defineEmits(['close', 'pick'])

const { run, isBusy } = useAction()

const here = ref('')
const parent = ref(null)
const entries = ref([])
const roots = ref([])
const error = ref('')

/**
 * 进一个目录。
 *
 * `soft` = 进不去就悄悄退回起点列表，不报错。**打开这个弹窗那一下必须是
 * soft 的**：起步路径用的是那个输入框里的值，而模型目录的默认值
 * （`<项目库>/models`）要等第一次下模型才会被建出来——第一次打开十有八九
 * 就是个不存在的路径。那时候把人挡在一句红字后面，等于这个浏览器没法用。
 * 人自己点进去的那些不 soft：敲错了得看得见。
 */
async function go(path, soft = false) {
  const r = await run(() => api.listDirs(path), { key: 'dirs', quiet: soft })
  if (!r) {
    if (soft && path) return go('', false)
    error.value = '进不去这个目录'
    return
  }
  error.value = ''
  here.value = r.path || ''
  parent.value = r.parent ?? null
  entries.value = r.entries ?? []
  roots.value = r.roots ?? []
}

watch(
  () => props.open,
  (on) => {
    if (on) go(props.start || '', true)
  },
  { immediate: true },
)
</script>

<template>
  <!-- **遮罩用全局那个 `.modal`。** base.css 里那段警告说的就是这个坑：
       scoped 里没写规则的类渲染出来是 position: static，整块落到文档最
       底下——不报错，只是看不见。我第一版写成 `.mask` 当场踩了。 -->
  <div v-if="open" class="modal" @click.self="emit('close')">
    <section class="dlg">
      <header class="dlg__head">
        <h2 class="dlg__t">挑一个文件夹</h2>
        <span class="spacer" />
        <button class="iconbtn" type="button" title="关掉" @click="emit('close')">
          <AppIcon name="close" :size="14" />
        </button>
      </header>

      <!-- 当前在哪儿。**整条路径都要看得见**：只显示最后一段的话，
           两个同名的 models 目录分不出来。 -->
      <div class="where mono tiny">
        <button
          class="btn btn--ghost btn--sm"
          type="button"
          :disabled="!parent || isBusy('dirs')"
          :title="parent ? '上一级' : '已经到头了'"
          @click="go(parent)"
        >
          ↑ 上一级
        </button>
        <span class="truncate">{{ here || '从下面挑一个起点' }}</span>
      </div>

      <p v-if="error" class="alert alert--bad tiny">{{ error }}</p>

      <div class="list">
        <template v-if="!here">
          <button
            v-for="r in roots"
            :key="r.path"
            class="row__btn"
            type="button"
            @click="go(r.path)"
          >
            <AppIcon name="folder" :size="13" />
            <span class="truncate">{{ r.name }}</span>
            <span v-if="r.why" class="tiny dim">{{ r.why }}</span>
          </button>
        </template>
        <template v-else>
          <button
            v-for="e in entries"
            :key="e.path"
            class="row__btn"
            type="button"
            @click="go(e.path)"
          >
            <AppIcon name="folder" :size="13" />
            <span class="truncate">{{ e.name }}</span>
          </button>
          <!-- **空目录也要说一声。** 一片空白和"还在读"长得一样。 -->
          <p v-if="!entries.length && !isBusy('dirs')" class="tiny dim pad">
            这里面没有子文件夹。就用它也行。
          </p>
        </template>
        <p v-if="isBusy('dirs')" class="tiny dim pad">读着…</p>
      </div>

      <footer class="dlg__foot">
        <span class="spacer" />
        <button class="btn btn--ghost btn--sm" type="button" @click="emit('close')">算了</button>
        <button
          class="btn btn--primary btn--sm"
          type="button"
          :disabled="!here"
          :title="here ? '把这个路径填进去' : '先进到一个文件夹里'"
          @click="emit('pick', here)"
        >
          用这个
        </button>
      </footer>
    </section>
  </div>
</template>

<style scoped>
.dlg {
  width: min(560px, 92vw);
  max-height: 80vh;
  display: flex;
  flex-direction: column;
  background: var(--surface);
  border: 1px solid var(--line);
  border-radius: var(--r);
  box-shadow: 0 20px 60px rgb(0 0 0 / 35%);
}
.dlg__head,
.dlg__foot {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: var(--s3) var(--s4);
}
.dlg__head {
  border-bottom: 1px solid var(--line);
}
.dlg__foot {
  border-top: 1px solid var(--line);
}
.dlg__t {
  margin: 0;
  font-size: var(--fs-md);
}
.where {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: var(--s2) var(--s4);
  color: var(--text-2);
  min-width: 0;
}
.list {
  flex: 1;
  overflow-y: auto;
  padding: var(--s2) var(--s3);
  min-height: 160px;
}
.row__btn {
  display: flex;
  align-items: center;
  gap: var(--s2);
  width: 100%;
  padding: var(--s2) var(--s3);
  border: 0;
  border-radius: var(--r-sm);
  background: transparent;
  color: inherit;
  font: inherit;
  text-align: left;
  cursor: pointer;
  min-width: 0;
}
.row__btn:hover {
  background: var(--surface-2);
}
.pad {
  padding: var(--s3);
}
</style>
