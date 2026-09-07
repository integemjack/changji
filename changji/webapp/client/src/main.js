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

app.use(createPinia()).use(router).mount('#app')
