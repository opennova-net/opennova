// What a list of servers or machines shows by default: everything live, and
// what ended without being replaced since. A start is a new server (a higher
// id) and a launch a new machine, so an old failure stays a row of its own
// after its owner started the server again or the fleet launched a machine
// that worked; those replaced rows are history, behind each list's "Show
// history".

interface ServerLike {
  id: number;
  live: boolean;
  settings: { game_name: string };
}

// The ended servers whose owner has started one of the same name after them.
// `ownerOf` names a server's owner on a list that spans accounts; another
// account's server of the same name never replaces one.
export function replacedServers<T extends ServerLike>(
  servers: readonly T[],
  ownerOf: (s: T) => number | null = () => null,
): Set<number> {
  const key = (s: T) => `${ownerOf(s) ?? ''}\n${s.settings.game_name.trim()}`;
  const newest = new Map<string, number>();
  for (const s of servers) newest.set(key(s), Math.max(newest.get(key(s)) ?? 0, s.id));
  return new Set(servers.filter((s) => !s.live && (newest.get(key(s)) ?? 0) > s.id).map((s) => s.id));
}

interface MachineLike {
  id: number | string;
  kind: string;
  state: string;
}

// The ended or failed machines of a kind that a later machine of that kind
// replaced: one launched after them that did not fail.
export function replacedMachines<T extends MachineLike>(machines: readonly T[]): Set<T['id']> {
  const newestLaunched = new Map<string, number>();
  for (const m of machines) {
    if (m.state !== 'failed') newestLaunched.set(m.kind, Math.max(newestLaunched.get(m.kind) ?? -1, Number(m.id)));
  }
  return new Set(
    machines
      .filter((m) => (m.state === 'ended' || m.state === 'failed') && Number(m.id) < (newestLaunched.get(m.kind) ?? -1))
      .map((m) => m.id),
  );
}
