import { fileURLToPath, URL } from 'node:url'
import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// 开发时前端跑在 5173，**接口转给引擎的 8080**。
//
// 原来转给的是 Node 那层的 5174，而那一层 2026-09-12 删了（见 README）。
// 忘了改这一行的话，`npm run dev` 打开是一片空白加满屏 502，而引擎明明
// 跑得好好的——所以这个默认值必须跟着引擎走。
// 引擎起在别的端口上（`changji --port`）时改这两行。
//
// 两个接口前缀都转过去，前端代码里就不用区分开发和部署。
// 引擎在别的机器上（比如拿一台带显卡的服务器做全套测试）时不用改文件：
//   CHANGJI_ENGINE=http://43.110.148.239:8080 npm run dev
// 不给就是本机 8080。
const ENGINE = process.env.CHANGJI_ENGINE || 'http://127.0.0.1:8080'

export default defineConfig({
  plugins: [vue()],
  resolve: {
    alias: { '@': fileURLToPath(new URL('./src', import.meta.url)) },
  },
  server: {
    port: 5173,
    host: true, // 手机连同一个局域网就能打开，调竖屏布局用得上
    proxy: {
      // ws: true——/api/ws 那条 WebSocket 也要经过代理，不然开发时顶栏的
      // 负载表和编辑器的流式写入都连不上，只能退回一次性返回。
      '/api': { target: ENGINE, changeOrigin: true, ws: true },
      '/bff': { target: ENGINE, changeOrigin: true },
    },
  },
  build: { outDir: 'dist', chunkSizeWarningLimit: 900 },
})
