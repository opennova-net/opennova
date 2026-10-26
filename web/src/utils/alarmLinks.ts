// Alarm names from infra/aws/observability.tf. Only known suffixes choose a
// specific page; a new alarm still has a useful, fixed infrastructure fallback.
export function alarmDestination(name: string): { to: string; label: string } {
  if (/-(hosted-failed|machines-lost|machines-stuck|machine-disk|servers-log-ingest)$/.test(name))
    return { to: '/admin/fleet', label: 'Open fleet' };
  if (/-launch-failures$/.test(name)) return { to: '/admin/jobs', label: 'Open background jobs' };
  if (/-(billing-sync-stale|webhook-rejects|limits-held)$/.test(name))
    return { to: '/admin/billing', label: 'Review subscriptions' };
  if (/-probes-unanswered$/.test(name)) return { to: '/admin/novaworld', label: 'Review NovaWorld' };
  if (/-fleet-pass-age$/.test(name)) return { to: '/admin/infrastructure/programs/fleet', label: 'Open fleet program' };
  if (/-db-(cpu|cpu-credits|connections|free-storage)$/.test(name))
    return { to: '/admin/infrastructure/resources/rds', label: 'Open database' };
  if (/-cache-\d{3}-(evictions|cpu-credits)$/.test(name))
    return { to: '/admin/infrastructure/resources/cache', label: 'Open cache' };
  if (/-(gate|lobby|http|https|update)-unhealthy$/.test(name))
    return { to: '/admin/infrastructure/resources/nlb', label: 'Open load balancer' };
  const program = name.match(/-(gate|lobby|web|fleet|worker|data)-(heartbeat-missing|crash-loop|running|cpu|memory)$/)?.[1];
  if (program) return { to: '/admin/infrastructure/programs/' + program, label: 'Open ' + program + ' program' };
  if (/-healthz$/.test(name)) return { to: '/admin/infrastructure/programs/web', label: 'Open web program' };
  return { to: '/admin/infrastructure', label: 'Open infrastructure' };
}
