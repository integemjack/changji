/**
 * 路由即流程。
 *
 * 八步各占一条路径，顺序写死在 STEP_ROUTES 里。上一步 / 下一步按钮、
 * 侧边导航、进度条都从这一个数组推导，不各写一份。
 *
 * 八步分两段，段界在场景那里：
 *
 *   全剧（做一次）  项目 → 剧本大纲 → 角色
 *   分集（每集重复） 场景 → 分镜 → 制作 → 成片 → 上传
 *
 * 前三步的产物是整部剧共用的：剧本大纲是全剧讲什么、分几集，角色是
 * 全剧同一批人。后五步是对着某一集干活，顶部的集号决定在哪一集上操作。
 *
 * 角色和场景为什么都放在同一个全剧资产库里、而不是每集一份：
 * 分镜表里只存 id，外观描述由程序从库里拼接，逐字节相同——这是角色和
 * 场景跨镜头一致的唯一手段。每集一份拷贝的话，同一个安保室在第一集和
 * 第五集会长得不一样，而这正是这套系统要防的事。所以库是全剧的，
 * 场景这一步只是把镜头对准「这一集用到的那几个」。
 */

import { createRouter, createWebHistory } from 'vue-router'

import { shouldSetup } from '@/composables/useSetupGate'
import {
  clearReloadMark,
  isChunkLoadError,
  shouldAutoReload,
} from './chunk-error'

export const PHASES = {
  series: { title: '全剧', hint: '做一次，整部剧共用' },
  episode: { title: '分集', hint: '每一集重复走一遍' },
}

export const STEP_ROUTES = [
  {
    key: 'project',
    path: '/project',
    name: 'project',
    phase: 'series',
    title: '项目',
    // 别在这儿写「右边」——项目库能拖到左边去，写死了就有一半时候是错的
    tagline: '这一部剧的进度和设置。换一部在项目库里点',
    icon: 'folder',
    component: () => import('@/views/ProjectView.vue'),
  },
  // **故事在剧本前面。** 原来第二步就是「剧本大纲」，而那一页是拿一句
  // 梗概逐集续写：集数人填、上下文只带前三集，于是没有全局结构、写到
  // 第五集开始失忆、故事也没有终点。故事这一步先把完整故事和分集定下来。
  {
    key: 'story',
    path: '/story',
    name: 'story',
    phase: 'series',
    title: '故事',
    tagline: '讲什么、分几章、按每集时长切成几集',
    icon: 'script',
    component: () => import('@/views/StoryView.vue'),
  },
  // **角色和场景合成一步「设定」**（2026-09-11）。
  //
  // 它们本来就是同一件事：人和地方在同一个 assets.json 里，都从故事提，
  // 做的都是传参考图、抠外观词、试音色。分两页只是让人多点一次。
  //
  // 顺带修掉场景那一页的错位：它原来挂在「分集」阶段、标题写着「这一集
  // 在哪儿拍」，而**场景库是全剧共用的**——分镜表里只存 id，外观从库里
  // 拼接，这是跨镜头一致的唯一手段。
  {
    key: 'assets',
    path: '/assets',
    name: 'assets',
    phase: 'series',
    title: '设定',
    tagline: '给故事里的人和地方定妆，全剧共用一套',
    icon: 'user',
    component: () => import('@/views/AssetsView.vue'),
  },
  // **镜头、成片、上传合成一步「这一集」**（2026-09-11）。
  //
  // 2026-09-10 已经把分镜和制作合过一次，理由是「人的动作是看片子→改台词→
  // 重出，在同一镜上来回，换页就断了」。这一次是同一条理由再往外一层：
  // 人做的事是**对照着看**——对着这句台词看这一镜对不对，看完整集顺手发
  // 出去。分成四页，来回换页才知道这一镜出自哪句话。
  //
  // 剧本也搬了进来：全剧那半（梗概、分集、章节）在故事页，这里只剩
  // 「这一集的剧本」，和它的镜头摆在同一页上。
  //
  // 侧边栏因此从八格变六格。`/bff/flow` 那边同步合成一个 `episode`，
  // 判据取原来「成片」那条——装配出片子才算这一集做完了，发布是可选的
  // 收尾动作，没发也不该让这一格一直不打勾。
  {
    key: 'episode',
    path: '/episode',
    name: 'episode',
    phase: 'episode',
    title: '这一集',
    tagline: '剧本、镜头、成片、发布，都在这一集上',
    icon: 'board',
    component: () => import('@/views/EpisodeView.vue'),
  },
]

const routes = [
  { path: '/', redirect: '/project' },
  ...STEP_ROUTES.map((s) => ({
    path: s.path,
    name: s.name,
    component: s.component,
    meta: { step: s.key, title: s.title },
  })),
  {
    path: '/settings',
    name: 'settings',
    component: () => import('@/views/SettingsView.vue'),
    meta: { title: '设置' },
  },
  // 首次运行：把模型下下来。
  //
  // **不在 STEP_ROUTES 里**，所以侧边栏和「上一步/下一步」都看不见它——
  // 它不是这八步中的一步，是八步开始之前的一次性准备。
  //
  // `chrome: false` 让 App.vue 把顶栏、侧边栏、集号条全收起来：
  // 那些东西这时候一个都点不动（还没有项目、引擎也还没模型），
  // 摆在那儿只会让人以为哪里没加载出来。
  {
    path: '/setup',
    name: 'setup',
    component: () => import('@/views/SetupView.vue'),
    meta: { title: '初始化', chrome: false },
  },
  // 老路径。合并之前它们是两页，收藏夹里可能还留着。
  // 「剧本大纲」那一页 2026-09-11 删了：全剧那半（梗概、分集、章节）
  // 在故事页，单集那半在「这一集」的剧本视图，预告片和手动加一集收进了
  // 故事页的折叠区。一页同时干六件事，那一页就不会有重点。
  { path: '/script', redirect: '/story' },
  { path: '/characters', redirect: '/assets' },
  { path: '/scenes', redirect: '/assets' },
  { path: '/storyboard', redirect: '/episode?view=shots' },
  { path: '/production', redirect: '/episode?view=shots' },
  { path: '/shots', redirect: '/episode?view=shots' },
  { path: '/film', redirect: '/episode?view=film' },
  { path: '/publish', redirect: '/episode?view=publish' },
  { path: '/:pathMatch(.*)*', redirect: '/project' },
]

export const router = createRouter({
  history: createWebHistory(),
  routes,
  scrollBehavior: () => ({ top: 0 }),
})

/**
 * 缺模型就先去初始化页。
 *
 * **只拦一次，而且拦不住就放行。** 判据在引擎那边（见 useSetupGate），
 * 问不到时一律放行——引擎没起来的时候把人钉在初始化页上，
 * 他连"引擎没起来"这件事都看不到，那一页自己也读不到清单。
 */
router.beforeEach(async (to) => {
  if (to.name === 'setup' || to.name === 'settings') return true
  return (await shouldSetup()) ? { name: 'setup' } : true
})

/**
 * 每一页都是 `() => import(...)` 懒加载的，那就有一种必然会发生的坏法：
 *
 *   换了一版 → 浏览器手里还是旧的 index.html（缓存）→ 里面写的资源名
 *   已经不在包里了 → 动态 import 失败 → **路由导航直接中止**。
 *
 * 中止之后什么都不渲染，用户看到的就是白屏。而 `app.config.errorHandler`
 * **接不到这个**——它只管组件内部抛的错，导航失败不走那条路。所以不接的话
 * 页面全白、控制台一行、界面上一个字都没有。
 *
 * 服务端那边已经改成「带哈希的资源找不到就回 404」并给 index.html 发
 * no-cache（见 webapp_cache_control）。但那两条只对**之后**拿到新 html
 * 的浏览器有效；手里已经攥着一份旧 html 的，还得靠这里整页重载一次。
 *
 * **重载要防死循环**：真的是资源丢了、重载也拿不回来的话，会一直刷。
 * 用 sessionStorage 记一笔，一次会话只自动重载一次，第二次就老实报错。
 */

router.onError((err, to) => {
  if (!isChunkLoadError(err)) {
    // 别的导航错误照样要说出来，不能又是一片白。
    window.dispatchEvent(
      new CustomEvent('changji:error', {
        detail: `打开页面失败：${err?.message || err}`,
      }),
    )
    return
  }
  if (!shouldAutoReload(globalThis.sessionStorage)) {
    window.dispatchEvent(
      new CustomEvent('changji:error', {
        detail: '页面资源加载不出来。刷新一次还是这样的话，多半是这一版没部署完整。',
      }),
    )
    return
  }
  // 整页重载（不是 router.push）：要的就是**重新去要一份 index.html**。
  window.location.assign(to.fullPath)
})

router.afterEach((to) => {
  document.title = to.meta?.title ? `${to.meta.title} · 场记` : '场记'
  // 进得来就说明资源是好的，把"重载过一次"那一笔清掉——
  // 不清的话这一会话里下次真遇到换版，就不会自动重载了。
  clearReloadMark(globalThis.sessionStorage)
})
