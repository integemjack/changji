# 启动 ComfyUI（Docker）
docker compose -f "E:\AI短剧\docker-compose.yml" up -d
Write-Host ""
Write-Host "ComfyUI 正在启动，浏览器打开: http://localhost:8188"
Write-Host "查看日志: docker logs -f comfyui"
Write-Host "停止:     docker compose -f `"E:\AI短剧\docker-compose.yml`" down"
