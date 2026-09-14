import { createApp } from 'vue'
import { createPinia } from 'pinia'

import App from './App.vue'
import { router } from './router'
import './styles/tokens.css'
import './styles/base.css'

const app = createApp(App)

/**
 * 兜住 ErrorBoundary 接不到的那些错。
 *
 * onErrorCaptured 只管子组件渲染时抛的错，事件回调、watch 里炸的会走到这儿。
 * 不接的话它们只在控制台里留一行——而没人会在用界面时开着控制台。
 */
app.config.errorHandler = (err, _instance, info) => {
  console.error('[界面出错]', info, err)
  // 这时候 pinia 已经装上了，但为了不引入循环依赖，直接找 DOM 里的提示区
  const text = `界面出错了（${info}）：${err?.message || err}`
  window.dispatchEvent(new CustomEvent('changji:error', { detail: text }))
}

/**
 * 没人接的 Promise 拒绝。
 *
 * 上面那个 errorHandler 只管 Vue 管得着的地方（渲染、watch、事件回调里
 * **同步**抛出来的）。而这套代码里反复出现的一种坏法它接不到：**裸的
 * await**——`await api.xxx()` 写在一个 async 函数里，没有 try，返回的
 * promise 也没人接。请求一失败就是一条没人接的拒绝，控制台里一行，界面上
 * 一个字都没有，点下去的那颗按钮看着就是"没反应"。
 *
 * 这半年一处一处修过好几个（模型弹窗里的「停下」、一键出图开头那一读…），
 * 每一处的注释都写着同一句"按钮点下去一点反应都没有，也不报错"。兜一层，
 * 下一处漏网的至少会说话。
 *
 * 主动取消的那种不算错：api 那层把 AbortError 原样抛出去，调用方本来就是
 * 故意不管它的。不 preventDefault——控制台里那条红要留着，它带着堆栈。
 */
window.addEventListener('unhandledrejection', (event) => {
  const err = event.reason
  if (err?.name === 'AbortError') return
  console.error('[没人接的错]', err)
  window.dispatchEvent(
    new CustomEvent('changji:error', {
      detail: `出错了：${err?.message || err}`,
    }),
  )
})

app.use(createPinia()).use(router).mount('#app')
