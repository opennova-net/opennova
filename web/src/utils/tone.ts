// The site's tones: one word for how a state reads (good, needs a look, bad,
// a pointer, nothing to say), and the badge and text classes that colour it.
// A kind of state (a server's, a job's, a release's) maps to a tone here or in
// its own view helper; the page draws <Badge :tone> or badgeClass(tone).

export type Tone = 'ok' | 'warn' | 'bad' | 'info' | 'muted';

export function badgeClass(tone: Tone): string {
  return tone === 'muted' ? 'badge' : `badge badge-${tone}`;
}

export function toneTextClass(tone: Tone): string {
  switch (tone) {
    case 'ok':
      return 'text-online';
    case 'warn':
      return 'text-warn';
    case 'bad':
      return 'text-danger';
    case 'info':
      return 'text-accent';
    default:
      return 'text-ink-muted';
  }
}

// A hosted server's state: running is good, failed bad, the steps between
// (and a stop) move; ended, or a state this site does not know, says nothing.
export function serverStateTone(state: string): Tone {
  switch (state) {
    case 'running':
      return 'ok';
    case 'failed':
      return 'bad';
    case 'stopping':
    case 'requested':
    case 'provisioning':
    case 'booting':
    case 'listing':
      return 'warn';
    default:
      return 'muted';
  }
}
