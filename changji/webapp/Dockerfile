# 场记 Web 平台。
#
# 只发前端和转发接口，不做任何推理，也不碰模型，所以镜像很小。
# 真正干活的是 changji 引擎（Python），这一层通过 CHANGJI_ENGINE_URL 找它。
#
# 分两段构建：第一段装依赖并打包前端，第二段只留运行时要的东西。
# 一起放的话 vite 和它那一堆 devDependencies 会跟着镜像走，白白多几百兆。

FROM node:22-alpine AS build

# 国内直连 npm 官方源装依赖经常传到一半断。换镜像站更稳。
# 境外机器把这个参数置空即可。
ARG NPM_MIRROR=https://registry.npmmirror.com
RUN if [ -n "$NPM_MIRROR" ]; then npm config set registry "$NPM_MIRROR"; fi

WORKDIR /app

# 先拷清单再拷源码，改代码时不必重装依赖
COPY package.json ./
COPY server/package.json ./server/
COPY client/package.json ./client/
RUN npm install --no-audit --no-fund

COPY . .
RUN npm run build


FROM node:22-alpine AS runtime

# 这是个中文软件，日志和报错都要能正常显示
ENV NODE_ENV=production \
    LANG=C.UTF-8 \
    HOST=0.0.0.0 \
    PORT=5174 \
    # 容器之间用服务名互访，不是 localhost
    CHANGJI_ENGINE_URL=http://changji:8080 \
    # 投递目标和引擎地址写在这里，挂出来才不会随容器重建丢掉
    CHANGJI_WEBAPP_CONFIG=/data/webapp/webapp.json

ARG NPM_MIRROR=https://registry.npmmirror.com
RUN if [ -n "$NPM_MIRROR" ]; then npm config set registry "$NPM_MIRROR"; fi

WORKDIR /app

# 运行时只要 server 那点依赖（express），前端已经是静态文件了
COPY package.json ./
COPY server/package.json ./server/
RUN npm install --omit=dev --workspace server --no-audit --no-fund \
    && npm cache clean --force

COPY server/ ./server/
COPY --from=build /app/client/dist ./client/dist

RUN mkdir -p /data/webapp

EXPOSE 5174

HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
    CMD wget -qO- http://127.0.0.1:5174/bff/health >/dev/null || exit 1

CMD ["node", "server/src/index.js"]
