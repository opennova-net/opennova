import { describe, expect, it } from 'vitest';
import { alarmDestination } from './alarmLinks';

describe('alarm destinations', () => {
  it.each([
    ['opennova-prod-hosted-failed', '/admin/servers'],
    ['opennova-prod-machines-lost', '/admin/servers/machines'],
    ['opennova-stage-launch-failures', '/admin/system/jobs'],
    ['opennova-prod-billing-sync-stale', '/admin/billing'],
    ['opennova-prod-probes-unanswered', '/admin/system'],
    ['opennova-prod-fleet-pass-age', '/admin/programs/fleet'],
    ['opennova-prod-worker-heartbeat-missing', '/admin/programs/worker'],
    ['opennova-prod-data-memory', '/admin/programs/data'],
    ['opennova-prod-db-cpu-credits', '/admin/resources/rds'],
    ['opennova-prod-cache-002-evictions', '/admin/resources/cache'],
    ['opennova-prod-cache-001-cpu-credits', '/admin/resources/cache'],
    ['opennova-prod-lobby-unhealthy', '/admin/resources/nlb'],
    ['opennova-prod-healthz', '/admin/programs/web'],
  ])('links %s to the existing diagnostic page', (name, destination) => {
    expect(alarmDestination(name).to).toBe(destination);
  });

  it.each(['future-alarm', 'opennova-prod-db-cpu-extra', 'https://example.com', 'javascript:alert(1)'])('gives an unknown alarm a fixed fallback: %s', (name) => {
    expect(alarmDestination(name)).toEqual({ to: '/admin/system/infrastructure', label: 'Open infrastructure' });
  });
});
