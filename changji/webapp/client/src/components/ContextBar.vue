<script setup>
/**
 * 当前上下文：在做哪个项目、哪一集。
 *
 * 常驻在内容区顶部。八步里有六步都是「对这一集做点什么」，
 * 每一页各放一个集号选择器的话，改一处忘一处，用户会在
 * 第三集的分镜页上点出第一集的成片。
 */
import { computed } from 'vue'
import { useRoute, useRouter } from 'vue-router'

import AppIcon from '@/components/AppIcon.vue'
import { STEP_ROUTES } from '@/router'
import { useSession } from '@/stores/session'

const session = useSession()
const router = useRouter()
const route = useRoute()

/**
 * 只有分集那几步才显示集号选择器。
 *
 * 项目、剧本大纲、角色是全剧的事，在那几页摆一个「当前集」会让人以为
 * 角色也要每集重出一遍。
 */
const phase = computed(
  () => STEP_ROUTES.find((s) => s.key === route.meta?.step)?.phase ?? 'series',
)
const perEpisode = computed(() => phase.value === 'episode')

const name = computed(
  () => session.project?.title || session.project?.project_id || '',
)
const shortPath = computed(() => {
  const p = session.projectPath
  if (!p) return ''
  const parts = p.split(/[\\/]/).filter(Boolean)
  return parts.length > 2 ? '…/' + parts.slice(-2).join('/') : p
})

function onPick(event) {
  session.selectEpisode(event.target.value)
}
</script>

<template>
  <div class="ctx" :class="{ 'ctx--empty': !session.hasProject }">
    <template v-if="session.hasProject">
      <AppIcon name="folder" :size="15" class="ctx__icon" />
      <button class="ctx__project" type="button" @click="router.push('/project')">
        <span class="ctx__name truncate">{{ name || '未命名项目' }}</span>
        <span class="ctx__path tiny dim truncate">{{ shortPath }}</span>
      </button>

      <template v-if="perEpisode">
        <span class="ctx__sep" />

        <label class="ctx__episode">
          <span class="tiny dim nowrap">当前集</span>
          <select
            class="select select--slim"
            :value="session.episodeId"
            :disabled="!session.episodes.length"
            @change="onPick"
          >
            <option v-if="!session.episodes.length" value="">还没有剧集</option>
            <option
              v-for="ep in session.episodes"
              :key="ep.episode_id"
              :value="ep.episode_id"
            >
              {{ ep.episode_id }} · {{ ep.title || '未命名' }}（{{ ep.shots }} 镜）
            </option>
          </select>
        </label>
      </template>
      <span v-else class="pill pill--neutral nowrap ctx__scope">
        全剧共用 · {{ session.episodes.length }} 集
      </span>

      <span class="spacer" />

      <span v-if="perEpisode && session.counters.shots" class="pill pill--neutral nowrap">
        {{ session.counters.produced }} / {{ session.counters.shots }} 镜已完成
      </span>

      <button
        class="btn btn--ghost btn--sm ctx__refresh"
        type="button"
        :disabled="session.loading"
        title="重新读一遍进度"
        @click="session.refresh()"
      >
        <AppIcon name="refresh" :size="14" :class="{ spin: session.loading }" />
      </button>
    </template>

    <template v-else>
      <AppIcon name="info" :size="15" class="ctx__icon" />
      <span class="small muted">还没选项目。先在第一步选一个，或者新建一个。</span>
      <span class="spacer" />
      <RouterLink to="/project" class="btn btn--sm btn--primary">去选项目</RouterLink>
    </template>
  </div>
</template>

<style scoped>
.ctx {
  flex: none;
  display: flex;
  align-items: center;
  gap: var(--s3);
  padding: var(--s2) var(--s6);
  min-height: 42px;
  border-bottom: 1px solid var(--line);
  background: var(--surface);
}
.ctx--empty {
  background: var(--warn-soft);
  border-bottom-color: color-mix(in srgb, var(--warn) 30%, transparent);
}
.ctx__icon {
  color: var(--text-3);
  flex: none;
}
.ctx--empty .ctx__icon {
  color: var(--warn);
}

.ctx__project {
  display: flex;
  flex-direction: column;
  align-items: flex-start;
  gap: 0;
  min-width: 0;
  max-width: 240px;
  padding: 2px var(--s2);
  border: none;
  background: none;
  border-radius: var(--r-sm);
  cursor: pointer;
  line-height: 1.25;
  text-align: left;
}
.ctx__project:hover {
  background: var(--surface-2);
}
.ctx__name {
  font-size: var(--fs-base);
  font-weight: 600;
  max-width: 100%;
}
.ctx__path {
  max-width: 100%;
  font-family: var(--font-mono);
}

.ctx__sep {
  width: 1px;
  height: 20px;
  background: var(--line);
  flex: none;
}

.ctx__episode {
  display: flex;
  align-items: center;
  gap: var(--s2);
  min-width: 0;
}
.select--slim {
  height: 28px;
  max-width: 280px;
  font-size: var(--fs-sm);
  background-position: calc(100% - 14px) 12px, calc(100% - 9px) 12px;
}

.ctx__refresh {
  width: 28px;
  padding: 0;
}
.spin {
  animation: spin 0.9s linear infinite;
}
@keyframes spin {
  to {
    transform: rotate(360deg);
  }
}

@media (max-width: 860px) {
  .ctx {
    padding: var(--s2) var(--s4);
    gap: var(--s2);
    overflow-x: auto;
    scrollbar-width: none;
  }
  .ctx::-webkit-scrollbar {
    display: none;
  }
  .ctx__path {
    display: none;
  }
  /* 「当前集」这三个字挤掉的正是项目名。下拉框本身已经说清楚是什么了 */
  .ctx__episode > .tiny {
    display: none;
  }
  .ctx__project {
    min-width: 5em;
  }
  .select--slim {
    max-width: 160px;
  }
}
</style>
