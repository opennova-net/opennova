// What a hosted server's waiting update brings, in the words its pages show it:
// the server page's callout (pages/servers/ServerOverviewTab.vue) and each
// servers table's row (pages/account/ServersPage.vue,
// pages/admin/servers/ServersTab.vue). Each reads a server whose
// update_available is set.

import type { HostedServer } from '../types/hostedServer';
import { versionText, type Build } from './buildView';

type Kinds = Pick<HostedServer, 'update_kinds'>;

// Newer data waits (a reply that names neither kind reads as data, the
// update's first kind).
export function contentUpdate(server: Kinds): boolean {
  return server.update_kinds.includes('content') || server.update_kinds.length === 0;
}

// A newer server build waits.
export function buildUpdate(server: Kinds): boolean {
  return server.update_kinds.includes('server_build');
}

// A build in a few words: ONE's version, else the commit.
function buildWords(build: Build): string {
  return versionText(build) ?? build.commit ?? '';
}

// "New server build: ONE 0.0.22.5 → ONE 0.0.22.9" when both builds are known
// (the commits where the versions read alike), "New server build: ONE 0.0.22.9"
// with only the new one, else "New server build available".
export function buildUpdateText(server: Pick<HostedServer, 'build' | 'build_available'>): string {
  const to = server.build_available;
  if (!to || !buildWords(to)) return 'New server build available';
  const from = server.build;
  if (!from || !buildWords(from)) return `New server build: ${buildWords(to)}`;
  const alike = buildWords(from) === buildWords(to) && from.commit && to.commit;
  return alike
    ? `New server build: ${buildWords(from)} ${from.commit} → ${buildWords(to)} ${to.commit}`
    : `New server build: ${buildWords(from)} → ${buildWords(to)}`;
}
