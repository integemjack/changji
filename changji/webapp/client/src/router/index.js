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
    tagline: '选一个项目，或者新建一个',
    icon: 'folder',
    component: () => import('@/views/ProjectView.vue'),
  },
  {
    key: 'script',
    path: '/script',
    name: 'script',
    phase: 'series',
    title: '剧本大纲',
    tagline: '全剧讲什么、分几集、每集写什么',
    icon: 'script',
    component: () => import('@/views/ScriptView.vue'),
  },
  {
    key: 'characters',
    path: '/characters',
    name: 'characters',
    phase: 'series',
    title: '角色',
    tagline: '从剧本提人物，全剧同一批',
    icon: 'user',
    component: () => import('@/views/CharactersView.vue'),
  },
  {
    key: 'scenes',
    path: '/scenes',
    name: 'scenes',
    phase: 'episode',
    title: '场景',
    tagline: '这一集在哪儿拍',
    icon: 'scene',
    component: () => import('@/views/ScenesView.vue'),
  },
  // **分镜和制作合成一步「镜头」**（2026-09-10）。
  //
  // 原来是两页：一页排镜头（AI 出分镜、改台词、调顺序），一页跑镜头
  // （出片、看进度、重出）。而人的动作是"看片子 → 改台词 → 重出"，
  // 在同一镜上来回——换页就断了，还得记住自己刚才看的是第几镜。
  //
  // 合成一页之后侧边栏也只留一格：两格指向同一个地方只会让人以为点错了。
  // `/bff/flow` 那边同步改成一个 `shots`，判据取原来「制作」那条
  // （每镜都出到成片）——光有分镜表不算做完，那时候一帧画面都还没有。
  {
    key: 'shots',
    path: '/shots',
    name: 'shots',
    phase: 'episode',
    title: '镜头',
    tagline: '拆镜头、改镜头、把它们拍出来',
    icon: 'board',
    component: () => import('@/views/ShotsView.vue'),
  },
  {
    key: 'film',
    path: '/film',
    name: 'film',
    phase: 'episode',
    title: '成片',
    tagline: '看装配好的这一集',
    icon: 'film',
    component: () => import('@/views/FilmView.vue'),
  },
  {
    key: 'publish',
    path: '/publish',
    name: 'publish',
    phase: 'episode',
    title: '上传至平台',
    tagline: '带上标题和话题投递出去',
    icon: 'upload',
    component: () => import('@/views/PublishView.vue'),
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
  { path: '/storyboard', redirect: '/shots' },
  { path: '/production', redirect: '/shots' },
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

router.afterEach((to) => {
  document.title = to.meta?.title ? `${to.meta.title} · 场记` : '场记'
})
