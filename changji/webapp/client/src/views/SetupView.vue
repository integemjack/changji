<script setup>
/**
 * 首次运行：把模型准备好。
 *
 * 装好程序打开界面，点到「出片」才发现什么都跑不了，报的是"本地模型一个
 * 都没配"。这一页把那段路变成三件事：看一眼推荐、点下载、等。
 *
 * **挑和下那一大块在 ModelPicker 里**，设置页用的是同一个组件。
 * 这一页只多一行标题和「先跳过 / 进入首页」两个跳转。
 */
import { computed, ref } from 'vue'
import { useRouter } from 'vue-router'

import AppIcon from '@/components/AppIcon.vue'
import ModelPicker from '@/components/ModelPicker.vue'
import { markSetupHandled } from '@/composables/useSetupGate'

const router = useRouter()
const picker = ref(null)

const busy = computed(() => Boolean(picker.value?.running))

function enter() {
  router.push('/project')
}

function skip() {
  // 跳过只在这台浏览器上生效，**不写进引擎的配置**：模型缺不缺是机器的
  // 事实，不该因为有人点了一次「先不下」就当它配好了。
  markSetupHandled()
  router.push('/project')
}
</script>

<template>
  <section class="setup">
    <div class="toolbar">
      <span class="brand">场</span>
      <h1 class="setup__t">先把模型准备好</h1>
    </div>

    <ModelPicker ref="picker">
      <template #actions>
        <button class="btn btn--ghost btn--sm" type="button" :disabled="busy" @click="skip">
          先跳过
        </button>
      </template>
      <template #done="{ state }">
        <button
          v-if="state === 'done'"
          class="btn btn--primary"
          type="button"
          @click="enter"
        >
          进入首页
          <AppIcon name="arrowRight" :size="16" />
        </button>
      </template>
    </ModelPicker>
  </section>
</template>

<style scoped>
/* 不居中、不卡宽——所有页面一样。 */
.setup {
  padding: var(--s3) var(--s3) var(--s8);
  display: flex;
  flex-direction: column;
  gap: var(--s3);
}
.brand {
  display: grid;
  place-items: center;
  width: 28px;
  height: 28px;
  border-radius: 8px;
  background: var(--accent);
  color: var(--accent-text);
  font-weight: 700;
  font-size: 14px;
}
.setup__t {
  margin: 0;
  font-size: var(--fs-md);
  font-weight: 600;
}
</style>
