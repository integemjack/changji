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
  {
    key: 'storyboard',
    path: '/storyboard',
    name: 'storyboard',
    phase: 'episode',
    title: '分镜',
    tagline: '把这一集拆成一个个镜头',
    icon: 'board',
    component: () => import('@/views/StoryboardView.vue'),
  },
  {
    key: 'production',
    path: '/production',
    name: 'production',
    phase: 'episode',
    title: '制作',
    tagline: '配音、首帧、草稿、成片',
    icon: 'gear',
    component: () => import('@/views/ProductionView.vue'),
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
  { path: '/:pathMatch(.*)*', redirect: '/project' },
]

export const router = createRouter({
  history: createWebHistory(),
  routes,
  scrollBehavior: () => ({ top: 0 }),
})

router.afterEach((to) => {
  document.title = to.meta?.title ? `${to.meta.title} · 场记` : '场记'
})
