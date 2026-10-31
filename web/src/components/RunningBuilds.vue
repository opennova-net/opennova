<template>
  <section class="running-builds space-y-3" aria-labelledby="running-builds-heading">
    <h2 id="running-builds-heading" class="section-heading">Running builds</h2>

    <div class="running-release card flex flex-wrap items-baseline gap-x-4 gap-y-1 p-4 text-sm">
      <template v-if="releaseLine.release">
        <span class="text-ink-muted">Release</span>
        <span class="release-id font-mono text-ink">{{ releaseLine.release.id }}</span>
        <span v-if="releaseLine.commit">
          <span class="text-ink-muted">Build</span> <span class="release-commit font-mono font-semibold text-ink">{{ releaseLine.commit }}</span>
        </span>
        <span v-if="releaseLine.release.created" class="text-xs text-ink-muted">Recorded <RelativeTime :value="releaseLine.release.created" :now="now" /></span>
      </template>
      <span v-else class="release-none" :class="releaseLine.tone === 'bad' ? 'text-danger' : 'text-ink-muted'">{{ releaseLine.text }}</span>
    </div>

    <div class="table-wrap">
      <table class="data-table">
        <caption class="sr-only">The build each program runs</caption>
        <thead>
          <tr>
            <th scope="col">Program</th>
            <th scope="col">Build</th>
            <th scope="col">Release</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="row in rows" :key="row.key" class="running-build" :data-part="row.key">
            <th scope="row" class="align-top font-normal">
              <RouterLink v-if="row.to" :to="row.to" class="entity-link">{{ row.name }}</RouterLink>
              <template v-else>{{ row.name }}</template>
              <span v-if="row.description" class="block text-xs text-ink-muted">{{ row.description }}</span>
            </th>
            <td class="align-top">
              <ul class="space-y-0.5">
                <li v-for="b in row.builds.builds" :key="`${b.build.version} ${b.build.commit}`" class="running-build-item">
                  <BuildText :build="b.build" />
                  <span v-if="many(row)" class="text-xs text-ink-muted"> ({{ countLabel(b.count, row.builds.unit) }})</span>
                </li>
                <li v-if="row.builds.unknown" class="text-xs text-warn">Build not known for {{ countLabel(row.builds.unknown, row.builds.unit) }}</li>
                <li v-if="!row.builds.builds.length && !row.builds.unknown" class="text-ink-muted">{{ row.idle }}</li>
              </ul>
            </td>
            <td class="align-top">
              <Badge v-if="row.onRelease === true" tone="ok">Matches the release</Badge>
              <Badge v-else-if="row.onRelease === false" tone="warn">Not the release's</Badge>
              <p v-if="row.onRelease !== true && row.releaseCommit" class="text-xs text-ink-muted">
                Release image <span class="font-mono">{{ row.releaseCommit }}</span>
              </p>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  </section>
</template>

<script setup lang="ts">
// The infrastructure page's headline: the release the deployment records (its
// id and build sha), then the build each part runs (utils/buildView.ts): the six
// programs (what their tasks report), the website (web's portal container, by
// its image's sha), hostd (what each machine's agent reports) and the serve
// image (the live hosted servers', by its image's sha). Whether it is the
// release's is a note beside it.
import { computed } from 'vue';
import { RouterLink } from 'vue-router';
import BuildText from './BuildText.vue';
import RelativeTime from './RelativeTime.vue';
import Badge from './ui/Badge.vue';
import type { InfraCloudRead, InfraOverview, InfraRelease } from '../types/infra';
import type { ProgramsResponse } from '../types/usage';
import {
  hostdBuilds,
  imagesOnRelease,
  isLiveMachine,
  portalBuilds,
  programBuilds,
  serveBuilds,
  type PartBuilds,
} from '../utils/buildView';
import { countLabel } from '../utils/format';

const props = defineProps<{
  overview: InfraOverview;
  programs: ProgramsResponse | null;
  // The programs in the page's order.
  names: string[];
  now: number;
}>();

interface Row {
  key: string;
  name: string;
  description?: string;
  to: string | null;
  builds: PartBuilds;
  // What a part with nothing running says.
  idle: string;
  onRelease: boolean | null;
  // The build sha of the release's image of it.
  releaseCommit: string | null;
}

const cloud = computed<InfraCloudRead | null>(() => {
  const c = props.overview.cloud;
  return c.configured && !c.error ? c : null;
});
const release = computed<InfraRelease | null>(() => cloud.value?.release ?? null);

const releaseLine = computed<{ release: InfraRelease | null; commit: string | null; text: string; tone: 'muted' | 'bad' }>(() => {
  const c = props.overview.cloud;
  const none = { release: null, commit: null, tone: 'muted' as const };
  if (!c.configured) return { ...none, text: 'The release is not read on this stack.' };
  if (c.error) return { ...none, text: 'The release is not known: AWS could not be read.', tone: 'bad' };
  if (c.release_error) return { ...none, text: `The release could not be read: ${c.release_error}`, tone: 'bad' };
  if (!c.release) return { ...none, text: 'No release is recorded yet.' };
  return { release: c.release, commit: c.release.revision ? c.release.revision.slice(0, 7) : null, text: '', tone: 'muted' };
});

const rows = computed<Row[]>(() => {
  const o = props.overview;
  const rel = release.value;
  const services = cloud.value?.services ?? [];
  const commitOf = (key: string) => rel?.image_commits[key] ?? null;
  const out: Row[] = props.names.map((name) => {
    const service = services.find((s) => s.name === name) ?? null;
    return {
      key: name,
      name,
      to: `/admin/programs/${name}`,
      builds: programBuilds(name, props.programs?.programs.find((p) => p.name === name) ?? null, service, rel),
      idle: 'No task runs',
      onRelease: service?.on_release ?? null,
      releaseCommit: commitOf(name),
    };
  });
  const web = services.find((s) => s.name === 'web') ?? null;
  if (web) {
    out.push({
      key: 'portal',
      name: 'portal',
      description: "The website, web's second container",
      to: null,
      builds: portalBuilds(web, rel),
      idle: 'No task runs',
      onRelease: imagesOnRelease(
        rel,
        'portal',
        web.tasks.map((t) => t.containers.find((c) => c.name === 'portal')?.image ?? null),
      ),
      releaseCommit: commitOf('portal'),
    });
  }
  const live = o.machines.filter(isLiveMachine);
  if (o.hosting.enabled || live.length || cloud.value?.fleet) {
    out.push({
      key: 'hostd',
      name: 'hostd',
      description: "Each machine's agent",
      to: '/admin/servers/machines',
      builds: hostdBuilds(o.machines),
      idle: 'No machine runs',
      onRelease: imagesOnRelease(
        rel,
        'hostd',
        live.map((m) => m.agent_image),
      ),
      releaseCommit: commitOf('hostd'),
    });
  }
  if (o.hosting.enabled || o.serve_images.length) {
    out.push({
      key: 'serve',
      name: 'serve',
      description: 'The hosted servers',
      to: '/admin/servers',
      builds: serveBuilds(o, rel),
      idle: 'No hosted server runs',
      onRelease: imagesOnRelease(
        rel,
        'serve',
        o.serve_images.map((s) => s.image),
      ),
      releaseCommit: commitOf('serve'),
    });
  }
  return out;
});

// Counts show where a part runs more than one build, or one not known.
function many(row: Row): boolean {
  return row.builds.builds.length > 1 || row.builds.unknown > 0;
}
</script>
