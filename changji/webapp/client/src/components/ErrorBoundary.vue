<script setup>
/**
 * 页面崩了要说出来。
 *
 * 起因是一次真事故：某个页面的 setup 里用了没导入的东西，Vite 编译得过，
 * 打开是一片空白，控制台里躺着一句 ReferenceError——页面上什么提示都没有。
 * 用户看到的就是「点了没反应」，而这是所有显示问题里最糟的一种：
 * 连「出错了」这三个字都没有。
 *
 * ESLint 现在能挡住「用了没导入」，但挡不住渲染时才炸的那些
 * （读了个 null 的属性、后端返回的形状变了）。所以这里再兜一层：
 * 崩了就把错误摆出来，并给一条能走的路——重试或者回第一步。
 */
import { onErrorCaptured, ref, watch } from 'vue'
import { useRoute } from 'vue-router'

import AppIcon from '@/components/AppIcon.vue'

const route = useRoute()
const failed = ref(null)

onErrorCaptured((err) => {
  failed.value = err
  // 拦下来，不让它继续往上冒——冒到根组件就是整个应用白屏
  return false
})

// 换一页就重试一次。上一页崩了不该把别的页也连坐。
watch(
  () => route.fullPath,
  () => {
    failed.value = null
  },
)

function retry() {
  failed.value = null
}
</script>

<template>
  <div v-if="failed" class="boom">
    <span class="boom__icon"><AppIcon name="warn" :size="22" /></span>
    <h2 class="boom__title">这一页没能画出来</h2>
    <p class="boom__hint">
      不是你的操作有问题，是界面自己出错了。已经保存的东西不受影响。
    </p>
    <pre class="boom__detail mono">{{ failed.message || String(failed) }}</pre>
    <div class="boom__acts">
      <button class="btn btn--primary" type="button" @click="retry">重试</button>
      <RouterLink to="/project" class="btn">回第一步</RouterLink>
      <button class="btn btn--ghost" type="button" @click="$router.go(0)">
        整页重载
      </button>
    </div>
  </div>
  <slot v-else />
</template>

<style scoped>
.boom {
  display: flex;
  flex-direction: column;
  align-items: center;
  text-align: center;
  gap: var(--s2);
  padding: var(--s10) var(--s5);
  border: 1px solid color-mix(in srgb, var(--danger) 35%, transparent);
  border-radius: var(--r-lg);
  background: color-mix(in srgb, var(--danger) 6%, var(--surface));
}
.boom__icon {
  display: grid;
  place-items: center;
  width: 44px;
  height: 44px;
  margin-bottom: var(--s2);
  border-radius: 14px;
  background: var(--danger-soft);
  color: var(--danger);
}
.boom__title {
  font-size: var(--fs-lg);
  font-weight: 600;
}
.boom__hint {
  max-width: 44ch;
  color: var(--text-2);
  font-size: var(--fs-base);
  line-height: 1.7;
}
.boom__detail {
  max-width: 100%;
  margin: var(--s3) 0 0;
  padding: var(--s3);
  border-radius: var(--r);
  background: var(--bg-sunken);
  border: 1px solid var(--line);
  color: var(--danger);
  text-align: left;
  white-space: pre-wrap;
  word-break: break-word;
  overflow-x: auto;
}
.boom__acts {
  display: flex;
  gap: var(--s2);
  flex-wrap: wrap;
  justify-content: center;
  margin-top: var(--s4);
}
</style>
