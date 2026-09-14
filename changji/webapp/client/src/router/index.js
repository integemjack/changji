/**
 * 路由即流程。
 *
 * 四步各占一条路径，顺序写死在 STEP_ROUTES 里。顶栏那排导航、上面的对勾、
 * 「下一步在哪儿」那个点，都从这一个数组推导，不各写一份。
 *
 * 四步分两段，段界在「这一集」前面：
 *
 *   全剧（做一次）   项目 → 故事 → 设定
 *   分集（每集重复）  这一集
 *
 * 前三步的产物是整部剧共用的：故事是全剧讲什么、分几集，设定是全剧同一批
 * 人和地方。最后一步是对着某一集干活，顶栏的集号决定在哪一集上操作——
 * `phase` 这个字段就为这个留着（App.vue 的 perEpisode）：全剧那几步摆一个
 * 「当前集」，会让人以为角色和场景也要每集重做一遍。
 *
 * ⚠️ **原来是八步**：剧本大纲、角色、场景、分镜、制作、成片、上传各占一页，
 * 底下还有一条「上一步 / 下一步」。2026-09-11 起按"人做的事是对照着看"
 * 一路合并（每一次合并的理由记在下面各自那一条上），那条 stepbar 也删了。
 * 这段开头以前还写着八步和那两个按钮，照着找会找不到东西。
 *
 * 角色和场景为什么都放在同一个全剧资产库里、而不是每集一份：
 * 分镜表里只存 id，外观描述由程序从库里拼接，逐字节相同——这是角色和
 * 场景跨镜头一致的唯一手段。每集一份拷贝的话，同一个安保室在第一集和
 * 第五集会长得不一样，而这正是这套系统要防的事。所以库是全剧的，
 * 场景这一步只是把镜头对准「这一集用到的那几个」。
 */

import { createRouter, createWebHistory } from 'vue-router'

import { store } from '@/composables/local-storage'

import {
  clearReloadMark,
  isChunkLoadError,
  shouldAutoReload,
} from './chunk-error'

export const STEP_ROUTES = [
  {
    key: 'project',
    path: '/project',
    name: 'project',
    phase: 'series',
    title: '项目',
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
    icon: 'script',
    // **宽页**：页面本身不滚，滚的是编辑器那一格；外壳不加最大宽度和内边距。
    // 这一页自己排三栏（章节、正文、对话），边距是它自己的事。
    wide: true,
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
    meta: { step: s.key, title: s.title, wide: s.wide === true },
  })),
  {
    path: '/settings',
    name: 'settings',
    component: () => import('@/views/SettingsView.vue'),
    meta: { title: '设置' },
  },
  // 老路径。合并之前它们是两页，收藏夹里可能还留着。
  // 「剧本大纲」那一页 2026-09-11 删了：全剧那半（梗概、分集、章节）
  // 在故事页，单集那半在「这一集」的剧本视图，预告片和手动加一集收进了
  // **设定页「分集」那一格**的折叠区（AssetEpisodes.vue，标题
  // 「预告片 · 手动加一集」）。一页同时干六件事，那一页就不会有重点。
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
  if (!shouldAutoReload(store('session'))) {
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
  // ⚠️ **这一句每次导航都跑。** 原来写的是 `globalThis.sessionStorage`——
  // 而在不给用存储的浏览器上，那个**属性访问本身**就抛，于是每跳一次页
  // 抛一次，抛出来又进 onError、那儿再访问一次同一个属性。chunk-error 里
  // 那两个函数早就把 storage 当参数收着（「隐私模式下它会抛」），漏的是
  // 取它的这一下。
  clearReloadMark(store('session'))
})
