<script setup>
/**
 * 设定。给故事里的人和地方定妆。
 *
 * **原来是两页**：「角色」（从剧本提人物，全剧同一批）和「场景」（这一集
 * 在哪儿拍）。合成一页的理由不是"它们都属于设定"，是**它们本来就是同一
 * 件事**——人和地方在同一个 assets.json 里，都从故事提，做的都是传参考图、
 * 抠外观词、试音色。分两页只是让人多点一次。
 *
 * 顺带修掉一处早就错位的：场景那一页挂在「分集」阶段，标题写着「这一集在
 * 哪儿拍」，而**场景库是全剧共用的**——分镜表里只存 id，外观从库里拼接，
 * 这是跨镜头一致的唯一手段，每集一份拷贝的话同一个房间在第一集和第五集
 * 会长得不一样。那一页自己的注释早就说过这件事了，只是标题没跟上。
 *
 * 关系摆在最上面。它是故事层新有的，而两页合并之前**界面上一个字都没有**
 * ——可它正是定妆的依据：前任和母女的眼神不一样。
 */
import { computed, onMounted, ref, watch } from 'vue'

import EmptyState from '@/components/EmptyState.vue'
import StepHeader from '@/components/StepHeader.vue'
import AssetCharacters from '@/views/assets/AssetCharacters.vue'
import AssetLocations from '@/views/assets/AssetLocations.vue'
import { api } from '@/api'
import { useSession } from '@/stores/session'

const session = useSession()
const story = ref(null)

const relations = computed(() => story.value?.relations ?? [])

async function loadStory() {
  if (!session.projectPath) {
    story.value = null
    return
  }
  try {
    const data = await api.getStory(session.projectPath)
    story.value = data.story ?? null
  } catch {
    // 没有故事的老项目走到这儿是正常的，关系那一块不显示就是了
    story.value = null
  }
}

onMounted(loadStory)
watch(() => session.projectPath, loadStory)
</script>

<template>
  <div class="stack stack--lg">
    <StepHeader />

    <EmptyState
      v-if="!session.hasProject"
      icon="folder"
      tone="warn"
      title="还没选项目"
      hint="设定挂在项目上。在项目库那条栏里点一个。"
    />

    <template v-else>
      <!-- 关系。**定妆的依据，不是装饰**——想复仇的人和想赎罪的人眼神
           不一样，而这件事只有故事层知道。 -->
      <section v-if="relations.length" class="card">
        <div class="card__head">
          <div>
            <div class="card__title">人物关系</div>
            <div class="card__sub">
              从故事里提的。定妆时照着它拿捏气质——只写「前任」不够，
              绷着的是什么才是要画出来的东西。
            </div>
          </div>
        </div>
        <div class="card__body">
          <div class="rels">
            <div v-for="(r, i) in relations" :key="i" class="rel">
              <b class="rel__who">{{ r.a }} — {{ r.b }}</b>
              <span v-if="r.kind" class="pill pill--neutral tiny nowrap">{{ r.kind }}</span>
              <span v-if="r.tension" class="rel__why small">{{ r.tension }}</span>
            </div>
          </div>
        </div>
      </section>

      <AssetCharacters />
      <AssetLocations />
    </template>
  </div>
</template>

<style scoped>
.rels {
  display: grid;
  gap: var(--s2);
}
.rel {
  display: flex;
  align-items: baseline;
  gap: var(--s3);
  flex-wrap: wrap;
}
.rel__who {
  flex: none;
}
.rel__why {
  color: var(--text-2);
  min-width: 0;
}
</style>
