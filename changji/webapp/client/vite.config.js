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
      '/api': { target: 'http://127.0.0.1:5174', changeOrigin: true },
      '/bff': { target: 'http://127.0.0.1:5174', changeOrigin: true },
    },
  },
  build: { outDir: 'dist', chunkSizeWarningLimit: 900 },
})
