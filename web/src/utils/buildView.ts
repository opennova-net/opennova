// What code each part of the deployment runs, for the infrastructure page
// (pages/admin/system/InfrastructureTab.vue) and a program's page: its build, the
// ONE version it carries ("ONE 0.0.22.5922") and the short sha of the commit it
// was built from ("b0c2b38"). The most truthful source first: what the running
// program says of itself (a program's tasks with their usage samples, hostd with
// every sync), else the commit its image's label names (the release manifest
// records it for each image it pins). Whether that is the release's is a note
// beside it.

import type { InfraContainer, InfraMachine, InfraOverview, InfraRelease, InfraService } from '../types/infra';
import type { ProgramUsage } from '../types/usage';

export interface Build {
  // ONE's version ("0.0.22.5922"); null where the source names none (an image's label).
  version: string | null;
  // The commit's short sha.
  commit: string | null;
}

// One build a part runs, and on how many of its tasks, machines or servers.
export interface BuildCount {
  build: Build;
  count: number;
  // Where it is known from: what the program reported, or its image's label.
  source: 'reported' | 'image';
}

export interface PartBuilds {
  // Most used first.
  builds: BuildCount[];
  // The ones whose build is not known (a task that never reported, an agent that
  // named none).
  unknown: number;
  // What a count counts.
  unit: 'task' | 'machine' | 'server';
}

const SHA = /^[0-9a-f]{7,40}$/;

// "ONE 0.0.22.5922", the product version string; null without a version.
export function versionText(build: Build): string | null {
  return build.version ? `ONE ${build.version}` : null;
}

// "ONE 0.0.22.5922 b0c2b38", "b0c2b38" with no version; '' for neither.
export function buildText(build: Build): string {
  return [versionText(build), build.commit].filter((t): t is string => !!t).join(' ');
}

function short(commit: string | null): string | null {
  return commit && SHA.test(commit) ? commit.slice(0, 7) : null;
}

// A build from a version and a commit as a sample names them; null for neither.
export function buildOf(version: string | null, commit: string | null): Build | null {
  const b = { version: version || null, commit: short(commit) };
  return b.version || b.commit ? b : null;
}

// hostd's agent_version: ONE's label ("ONE 0.0.22.123 (a1b2c3d, staging)"), or a
// bare commit (an agent built before it reported ONE's label); null for anything else.
export function parseAgentVersion(text: string | null): Build | null {
  if (!text) return null;
  const t = text.trim();
  if (SHA.test(t)) return { version: null, commit: t.slice(0, 7) };
  const m = /^ONE(?: ([0-9]+(?:\.[0-9]+){0,3}))?(?: \(([^)]*)\))?$/.exec(t);
  if (!m) return null;
  const commit = (m[2] ?? '')
    .split(',')
    .map((n) => n.trim())
    .find((n) => SHA.test(n));
  return buildOf(m[1] ?? null, commit ?? null);
}

// The commit the release's image `key` names, when `image` is that image.
export function releaseImageCommit(release: InfraRelease | null, key: string, image: string | null): string | null {
  if (!release || !image || release.images[key] !== image) return null;
  return short(release.image_commits[key] ?? null);
}

function tally(builds: { build: Build | null; source: BuildCount['source']; count?: number }[], unit: PartBuilds['unit']): PartBuilds {
  const out: BuildCount[] = [];
  let unknown = 0;
  for (const { build, source, count = 1 } of builds) {
    if (!build) {
      unknown += count;
      continue;
    }
    const same = out.find((b) => b.build.version === build.version && b.build.commit === build.commit);
    if (same) same.count += count;
    else out.push({ build, count, source });
  }
  out.sort((a, b) => b.count - a.count);
  return { builds: out, unknown, unit };
}

function container(containers: InfraContainer[], name: string): InfraContainer | null {
  return containers.find((c) => c.name === name) ?? null;
}

// A program's running tasks' builds: each task's own report (its newest
// sample's), else the commit its container's image label names when that image
// is the release's (a task that has not reported yet). The tasks are its
// reporting ones and the ones ECS runs.
export function programBuilds(
  name: string,
  usage: ProgramUsage | null,
  service: InfraService | null,
  release: InfraRelease | null,
): PartBuilds {
  const ids = new Set<string>();
  const rows: { build: Build | null; source: BuildCount['source'] }[] = [];
  for (const t of usage?.tasks ?? []) {
    ids.add(t.id);
    const reported = buildOf(t.one_version, t.one_commit);
    const ecs = service?.tasks.find((e) => e.id === t.id) ?? null;
    const image = ecs ? (container(ecs.containers, name)?.image ?? null) : null;
    const commit = releaseImageCommit(release, name, image);
    rows.push(reported ? { build: reported, source: 'reported' } : { build: commit ? { version: null, commit } : null, source: 'image' });
  }
  for (const t of service?.tasks ?? []) {
    if (ids.has(t.id)) continue;
    const commit = releaseImageCommit(release, name, container(t.containers, name)?.image ?? null);
    rows.push({ build: commit ? { version: null, commit } : null, source: 'image' });
  }
  return tally(rows, 'task');
}

// The website's builds: web's tasks' portal containers (nginx with the site),
// by their image's label. It reports nothing of itself.
export function portalBuilds(web: InfraService | null, release: InfraRelease | null): PartBuilds {
  return tally(
    (web?.tasks ?? []).map((t) => {
      const commit = releaseImageCommit(release, 'portal', container(t.containers, 'portal')?.image ?? null);
      return { build: commit ? { version: null, commit } : null, source: 'image' as const };
    }),
    'task',
  );
}

export function isLiveMachine(m: InfraMachine): boolean {
  return m.state !== 'ended' && m.state !== 'failed';
}

// hostd's builds: what each live machine's agent last reported.
export function hostdBuilds(machines: InfraMachine[]): PartBuilds {
  return tally(
    machines.filter(isLiveMachine).map((m) => ({ build: parseAgentVersion(m.agent_version), source: 'reported' as const })),
    'machine',
  );
}

// The serve image's builds: the live hosted servers' images, by the commit the
// release's image label names (an older image's is not known here).
export function serveBuilds(o: InfraOverview, release: InfraRelease | null): PartBuilds {
  return tally(
    o.serve_images.map((s) => {
      const commit = releaseImageCommit(release, 'serve', s.image);
      return { build: commit ? { version: null, commit } : null, source: 'image' as const, count: s.servers };
    }),
    'server',
  );
}

// "43" from "opennova-prod-web:43"; null for none.
export function revisionNumber(taskDefinition: string | null | undefined): string | null {
  if (!taskDefinition) return null;
  const colon = taskDefinition.lastIndexOf(':');
  return colon >= 0 && colon < taskDefinition.length - 1 ? taskDefinition.slice(colon + 1) : null;
}

// "ECS task revision 43": a task definition's revision, labelled.
export function revisionLabel(taskDefinition: string | null | undefined): string | null {
  const n = revisionNumber(taskDefinition);
  return n ? `ECS task revision ${n}` : null;
}

// Whether every one of `images` is the release's `key` image; null when the
// release names none or nothing runs.
export function imagesOnRelease(release: InfraRelease | null, key: string, images: (string | null)[]): boolean | null {
  const want = release?.images[key];
  const known = images.filter((i): i is string => !!i);
  if (!want || known.length === 0) return null;
  return known.every((i) => i === want);
}
