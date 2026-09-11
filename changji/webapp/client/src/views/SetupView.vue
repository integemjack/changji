<script setup>
/**
 * 首次运行：把模型准备好。
 *
 * **为什么要有这一页。** 装好程序打开界面，用户看到的是一个能点的项目页；
 * 点到「出片」才发现什么都跑不了，报的是"本地模型一个都没配"。那时候他
 * 手上只有一句提示和一个配置文件路径，要自己去翻部署手册、找镜像地址、
 * 挑量化档、算显存装不装得下。这一页把那段路变成三件事：看一眼推荐、
 * 点下载、等。
 *
 * **挑和下那一大块在 ModelPicker 里**，设置页用的是同一个组件。
 * 这一页只多两样东西：一个抬头，和「先跳过 / 进入首页」这两个跳转。
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
    <header class="head">
      <span class="head__mark">场</span>
      <div class="head__text">
        <h1 class="head__title">先把模型准备好</h1>
        <p class="head__sub">
          这台机器上还缺出片要用的模型。下面每一组都已经按你的显卡选好了一项，
          直接点下载就行；想换成别的档位也可以。
        </p>
      </div>
    </header>

    <ModelPicker ref="picker">
      <template #actions>
        <button class="btn btn--ghost" type="button" :disabled="busy" @click="skip">
          先跳过
        </button>
      </template>
      <template #done="{ state }">
        <button
          v-if="state === 'done'"
          class="btn btn--primary btn--lg"
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
/* 不居中、不卡宽——所有页面一样，见 App.vue 里 .main__inner 那条。 */
.setup {
  padding: var(--s8) var(--s6) var(--s12);
  display: flex;
  flex-direction: column;
  gap: var(--s5);
}

.head {
  display: flex;
  align-items: flex-start;
  gap: var(--s4);
}
.head__mark {
  flex: none;
  display: grid;
  place-items: center;
  width: 40px;
  height: 40px;
  border-radius: 12px;
  background: linear-gradient(
    135deg,
    var(--accent),
    color-mix(in srgb, var(--accent) 60%, #d9534f)
  );
  color: var(--accent-text);
  font-weight: 700;
  font-size: 18px;
  box-shadow: var(--shadow-1);
}
.head__title {
  font-size: var(--fs-2xl);
  font-weight: 700;
  letter-spacing: -0.01em;
}
.head__sub {
  margin-top: 4px;
  color: var(--text-2);
  font-size: var(--fs-base);
  line-height: 1.7;
  max-width: 58ch;
}

@media (max-width: 640px) {
  .setup {
    padding: var(--s5) var(--s4) var(--s10);
  }
}
</style>
