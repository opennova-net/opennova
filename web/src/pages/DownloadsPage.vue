<template>
  <section class="page py-10 sm:py-14" aria-labelledby="downloads-title">
    <header class="max-w-3xl">
      <p class="eyebrow">Windows x64 / Pre-1.0</p>
      <h1 id="downloads-title" class="mt-3 text-4xl font-semibold tracking-tight text-ink-bright">Download OpenNova</h1>
      <p class="mt-4 leading-relaxed text-ink-muted">
        Games, the editor and tools. Pre-1.0 and experimental. Unzip a game build into an empty folder and run
        <code>opennova.exe</code>. Its first launch downloads that game's content. Playing Joint Operations needs
        your own purchased copy.
      </p>
      <p class="mt-4 text-sm text-ink-muted">
        Not sure where to start? <RouterLink to="/play" class="link">Find the right setup for your game</RouterLink>.
      </p>
    </header>

    <div class="mt-7 rounded-panel border border-border bg-panel p-4 text-sm text-ink-muted">
      <p v-if="manifest" data-test="downloads-version">
        Version {{ manifest.version }}{{ buildKind ? `, a ${buildKind}` : '' }}<template v-if="manifest.commit">
          from commit <a v-if="sourceUrl" :href="sourceUrl" class="link font-mono" :title="manifest.commit">{{ shortCommit }}</a><span v-else class="font-mono" :title="manifest.commit">{{ shortCommit }}</span></template>, published <time :datetime="isoTime(manifest.published)">{{ formatUtc(manifest.published) }}</time>.
      </p>
      <p v-else-if="loading" role="status">Checking available downloads...</p>
      <p v-else-if="downloadsHost" role="status" class="text-warn">The downloads manifest could not be loaded. Refresh this page to try again.</p>
      <p v-else>Downloads are not configured for this site.</p>
    </div>

    <h2 class="mt-10 text-xl font-semibold text-ink-bright">Games</h2>
    <ul class="mt-4 grid gap-4 lg:grid-cols-3">
      <li
        v-for="item in gameCards"
        :id="item.file.startsWith('opennova-game') ? 'opennova' : item.file.startsWith('jointops') ? 'jointops' : 'dfx2'"
        :key="item.file"
        class="card scroll-mt-24 flex flex-col p-6"
      >
        <h3 class="text-lg font-semibold text-ink-bright">{{ item.name }}</h3>
        <p class="mt-2 flex-1 text-sm leading-relaxed text-ink-muted">{{ item.body }}</p>
        <div class="mt-6 space-y-3">
          <a v-if="downloadUrl(item.file)" :href="downloadUrl(item.file)" class="btn-primary" :aria-label="'Download ' + item.name">Download</a>
          <span v-else class="btn-primary cursor-not-allowed opacity-50" aria-disabled="true">{{ unavailableLabel }}</span>
          <div class="min-w-0 break-words font-mono text-xs text-ink-muted" :data-test="'build-' + item.file">
            <p>{{ item.file }}</p>
            <p v-if="item.build">{{ item.build.details }}, SHA-256 <span :title="item.build.sha256">{{ item.build.sha256.slice(0, 12) }}</span></p>
          </div>
        </div>
      </li>
    </ul>

    <h2 id="tools" class="mt-12 scroll-mt-24 text-xl font-semibold text-ink-bright">Tools &amp; server software</h2>
    <ul class="mt-4 grid gap-4 sm:grid-cols-2 lg:grid-cols-4">
      <li v-for="item in toolCards" :key="item.file" class="card flex flex-col p-5">
        <h3 class="font-semibold text-ink-bright">{{ item.name }}</h3>
        <p class="mt-2 flex-1 text-sm leading-relaxed text-ink-muted">{{ item.body }}</p>
        <div class="mt-5 space-y-3">
          <a v-if="downloadUrl(item.file)" :href="downloadUrl(item.file)" class="btn-secondary" :aria-label="'Download ' + item.name">Download</a>
          <span v-else class="btn-secondary cursor-not-allowed opacity-50" aria-disabled="true">{{ unavailableLabel }}</span>
          <div class="min-w-0 break-words font-mono text-xs text-ink-muted" :data-test="'build-' + item.file">
            <p>{{ item.file }}</p>
            <p v-if="item.build">{{ item.build.details }}, SHA-256 <span :title="item.build.sha256">{{ item.build.sha256.slice(0, 12) }}</span></p>
          </div>
        </div>
      </li>
    </ul>
  </section>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { RouterLink } from 'vue-router';
import { downloadsBase, fetchDownloadsManifest, type DownloadsManifest } from '../api/downloads';
import { formatBytes, formatUtc, isoTime } from '../utils/format';
import { GITHUB_URL } from '../utils/links';
const games = [
  {
    name: 'OpenNova',
    body: 'Our own game: downloads OP: Acid Gambit on its first launch, and plays the Joint Operations you own in place of Jointops.exe.',
    file: 'opennova-game-windows.zip',
  },
  {
    name: 'OpenNova for Joint Operations',
    body: 'The same game, set up for Joint Operations: unzip it into an empty folder and its first launch downloads Joint Operations and its Escalation expansion.',
    file: 'jointops-game-windows.zip',
  },
  {
    name: 'OpenNova for Delta Force Xtreme 2',
    body: 'The same game, set up for Delta Force Xtreme 2: unzip it into an empty folder and its first launch downloads Delta Force Xtreme 2.',
    file: 'dfx2-game-windows.zip',
  },
];

const tools = [
  {
    name: 'OpenNova Editor',
    body: 'The editor that made our game, ready to run on its own.',
    file: 'opennova-editor-windows.zip',
  },
  {
    name: 'Blender add-on',
    body: 'Imports and exports .3di models and their animations in Blender 5.',
    file: 'opennova-blender-addon-windows.zip',
  },
  {
    name: 'Tools and dedicated server',
    body: 'The command-line tools and opennova-serve, the server our hosted servers run. Retail and OpenNova players both join it.',
    file: 'opennova-apps-windows.zip',
  },
  {
    name: 'OpenNova Switcher',
    body: "Only for the original dfx2.exe, whose NovaLogic NovaWorld is down: a tray app that points it at our NovaWorld or an address of your own. OpenNova and the original Jointops.exe don't need it.",
    file: 'opennova-switcher-windows.zip',
  },
];

// This deployment's downloads host (downloads.<the site's hostname>), and what its manifest
// says: the version, the build and its commit, and each file's version, size and SHA-256. A
// site with no host (a local stack) offers no download.
const downloadsHost = downloadsBase(window.location.hostname);
const manifest = ref<DownloadsManifest | null>(null);

// The build on this deployment's downloads host once its manifest lists it; none before (a
// deployment before its first publish, a manifest that failed to load), and never elsewhere.
function downloadUrl(file: string): string | undefined {
  return downloadsHost && manifest.value?.files[file] ? `${downloadsHost}/${file}` : undefined;
}

// "release build", "debug build for staging": what the set is, when the manifest says.
const buildKind = computed(() => {
  const m = manifest.value;
  if (!m?.build) return '';
  return m.build !== 'release' && m.talks_to ? `${m.build} build for ${m.talks_to}` : `${m.build} build`;
});

const shortCommit = computed(() => manifest.value?.commit?.slice(0, 7) ?? '');

// Where the commit links: the CI run a pull request's set came from, or for a release (whose source
// is its GitHub release page, which the site never links) the commit itself.
const sourceUrl = computed(() => {
  const m = manifest.value;
  const source = m?.source;
  if (!source || !/^https:\/\//i.test(source)) return undefined;
  if (/\/releases(\/|$)/.test(source)) return m?.commit ? `${GITHUB_URL}/commit/${m.commit}` : undefined;
  return source;
});

// A build's line under its file name: its version when its name carried one, the set's commit and,
// for a build that is no release, the deployment it talks to, as the runtime's main menu names its
// build ("v0.0.20 (705c774, staging), 53 MB"), then its size and SHA-256; none before the manifest
// lists it.
function buildInfo(file: string): { details: string; sha256: string } | undefined {
  const m = manifest.value;
  const entry = m?.files[file];
  if (!m || !entry) return undefined;
  const notes = [shortCommit.value, m.build && m.build !== 'release' ? m.talks_to || m.build : '']
    .filter(Boolean)
    .join(', ');
  const name = entry.version ? `v${entry.version}` : '';
  const which = name && notes ? `${name} (${notes})` : name || notes;
  return { details: which ? `${which}, ${formatBytes(entry.size)}` : formatBytes(entry.size), sha256: entry.sha256 };
}

const gameCards = computed(() => games.map((item) => ({ ...item, build: buildInfo(item.file) })));
const toolCards = computed(() => tools.map((item) => ({ ...item, build: buildInfo(item.file) })));

const loading = ref(Boolean(downloadsHost));
const unavailableLabel = computed(() => loading.value ? 'Checking availability…' : manifest.value ? 'Not published yet' : 'Unavailable');
onMounted(async () => {
  if (!downloadsHost) return;
  try { manifest.value = await fetchDownloadsManifest(downloadsHost); }
  catch { manifest.value = null; }
  finally { loading.value = false; }
});
</script>
