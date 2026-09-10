/**
 * 首次运行那一页，原样转给引擎。
 *
 * **这一层什么都不判断。** "缺不缺模型"是引擎所在那台机器的事实：
 * 文件在不在盘上、显卡多大、配置指没指对，Node 这边一样都不知道。
 * 在这里再实现一遍的话，两套判断迟早分家——而分家的表现是
 * 界面说"已就绪"、点出片报"模型没配"。
 *
 * 那为什么不直接让前端打 `/api/*` 走通用转发？因为这四条不在 `/api` 下：
 * 下模型这件事引擎独有，Python 那边压根没有，放进 `/api` 就是一处
 * 破契约（见 cpp/src/http/setup_api.hpp 开头）。而 `/bff/*` 在两层架构里
 * 归 Node 管，所以它得自己把这几条接上。
 *
 * 下载超时要单独给：默认那个是给"出分镜"那类活儿的，而这几条本身
 * 是立刻返回的（真正的下载在引擎的后台线程里跑）。
 */

import { Router } from 'express'
import { callEngine } from '../engine.js'

export const setupRouter = Router()

/** 引擎报错原样带出去。状态码也要带——前端靠 409 认出"已经在下了"。 */
function forward(handler) {
  return async (req, res, next) => {
    try {
      res.json(await handler(req))
    } catch (err) {
      next(err)
    }
  }
}

setupRouter.get(
  '/state',
  forward(() => callEngine('/bff/setup/state', { timeoutMs: 30000 })),
)

setupRouter.post(
  '/download',
  forward((req) =>
    callEngine('/bff/setup/download', {
      method: 'POST',
      body: req.body,
      timeoutMs: 30000,
    }),
  ),
)

setupRouter.get(
  '/progress',
  // **超时给得短。** 前端一秒问一次，这条要是卡住十分钟，
  // 界面上的进度就冻在那一刻，而下载其实还在跑。
  forward(() => callEngine('/bff/setup/progress', { timeoutMs: 8000 })),
)

setupRouter.post(
  '/cancel',
  forward(() =>
    callEngine('/bff/setup/cancel', { method: 'POST', body: {}, timeoutMs: 15000 }),
  ),
)
