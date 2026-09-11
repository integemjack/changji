import { fileURLToPath, URL } from 'node:url'
import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// 开发时前端跑在 5173，Node 服务跑在 5174。
// 两个接口前缀都转过去，前端代码里就不用区分开发和部署。
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
      '/api': { target: 'http://127.0.0.1:5174', changeOrigin: true, ws: true },
      '/bff': { target: 'http://127.0.0.1:5174', changeOrigin: true },
    },
  },
  build: { outDir: 'dist', chunkSizeWarningLimit: 900 },
})
