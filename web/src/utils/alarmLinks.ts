// Alarm names from infra/aws/observability.tf. Only known suffixes choose a
// specific page; a new alarm still has a useful, fixed infrastructure fallback.
export function alarmDestination(name: string): { to: string; label: string } {
  if (/-(hosted-failed|servers-log-ingest)$/.test(name)) return { to: '/admin/servers', label: 'Open servers' };
  if (/-(machines-lost|machines-stuck|machine-disk)$/.test(name)) return { to: '/admin/servers/machines', label: 'Open machines' };
  if (/-launch-failures$/.test(name)) return { to: '/admin/system/jobs', label: 'Open background jobs' };
  if (/-(billing-sync-stale|webhook-rejects|limits-held)$/.test(name))
    return { to: '/admin/billing', label: 'Review subscriptions' };
  if (/-probes-unanswered$/.test(name)) return { to: '/admin/system', label: 'Review NovaWorld' };
  if (/-fleet-pass-age$/.test(name)) return { to: '/admin/programs/fleet', label: 'Open fleet program' };
  if (/-db-(cpu|cpu-credits|connections|free-storage)$/.test(name)) return { to: '/admin/resources/rds', label: 'Open database' };
  if (/-cache-\d{3}-(evictions|cpu-credits)$/.test(name)) return { to: '/admin/resources/cache', label: 'Open cache' };
  if (/-(gate|lobby|http|https|update)-unhealthy$/.test(name)) return { to: '/admin/resources/nlb', label: 'Open load balancer' };
  const program = name.match(/-(gate|lobby|web|fleet|worker|data)-(heartbeat-missing|crash-loop|running|cpu|memory)$/)?.[1];
  if (program) return { to: '/admin/programs/' + program, label: 'Open ' + program + ' program' };
  if (/-healthz$/.test(name)) return { to: '/admin/programs/web', label: 'Open web program' };
  return { to: '/admin/system/infrastructure', label: 'Open infrastructure' };
}
