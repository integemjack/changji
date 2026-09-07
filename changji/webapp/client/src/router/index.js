/**
 * 路由即流程。
 *
 * 八步各占一条路径，顺序写死在 STEP_ROUTES 里。上一步 / 下一步按钮、
 * 侧边导航、进度条都从这一个数组推导，不各写一份。
 */

import { createRouter, createWebHistory } from 'vue-router'

export const STEP_ROUTES = [
  {
    key: 'project',
    path: '/project',
    name: 'project',
    title: '项目',
    tagline: '选一个项目，或者新建一个',
    icon: 'folder',
    component: () => import('@/views/ProjectView.vue'),
  },
  {
    key: 'script',
    path: '/script',
    name: 'script',
    title: '剧本大纲',
    tagline: '一句梗概，让大模型写出整集',
    icon: 'script',
    component: () => import('@/views/ScriptView.vue'),
  },
  {
    key: 'characters',
    path: '/characters',
    name: 'characters',
    title: '角色',
    tagline: '定下长相、服装和音色',
    icon: 'user',
    component: () => import('@/views/CharactersView.vue'),
  },
  {
    key: 'scenes',
    path: '/scenes',
    name: 'scenes',
    title: '场景',
    tagline: '定下空间、光线和色调',
    icon: 'scene',
    component: () => import('@/views/ScenesView.vue'),
  },
  {
    key: 'storyboard',
    path: '/storyboard',
    name: 'storyboard',
    title: '分镜',
    tagline: '把剧本拆成一个个镜头',
    icon: 'board',
    component: () => import('@/views/StoryboardView.vue'),
  },
  {
    key: 'production',
    path: '/production',
    name: 'production',
    title: '制作',
    tagline: '配音、首帧、草稿、成片',
    icon: 'gear',
    component: () => import('@/views/ProductionView.vue'),
  },
  {
    key: 'film',
    path: '/film',
    name: 'film',
    title: '成片',
    tagline: '看装配好的整集',
    icon: 'film',
    component: () => import('@/views/FilmView.vue'),
  },
  {
    key: 'publish',
    path: '/publish',
    name: 'publish',
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
