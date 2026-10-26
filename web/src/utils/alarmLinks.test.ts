import { describe, expect, it } from 'vitest';
import { alarmDestination } from './alarmLinks';

describe('alarm destinations', () => {
  it.each([
    ['opennova-prod-machines-lost', '/admin/fleet'],
    ['opennova-stage-launch-failures', '/admin/jobs'],
    ['opennova-prod-billing-sync-stale', '/admin/billing'],
    ['opennova-prod-probes-unanswered', '/admin/novaworld'],
    ['opennova-prod-fleet-pass-age', '/admin/infrastructure/programs/fleet'],
    ['opennova-prod-worker-heartbeat-missing', '/admin/infrastructure/programs/worker'],
    ['opennova-prod-data-memory', '/admin/infrastructure/programs/data'],
    ['opennova-prod-db-cpu-credits', '/admin/infrastructure/resources/rds'],
    ['opennova-prod-cache-002-evictions', '/admin/infrastructure/resources/cache'],
    ['opennova-prod-cache-001-cpu-credits', '/admin/infrastructure/resources/cache'],
    ['opennova-prod-lobby-unhealthy', '/admin/infrastructure/resources/nlb'],
    ['opennova-prod-healthz', '/admin/infrastructure/programs/web'],
  ])('links %s to the existing diagnostic page', (name, destination) => {
    expect(alarmDestination(name).to).toBe(destination);
  });

  it.each(['future-alarm', 'opennova-prod-db-cpu-extra', 'https://example.com', 'javascript:alert(1)'])('gives an unknown alarm a fixed fallback: %s', (name) => {
    expect(alarmDestination(name)).toEqual({ to: '/admin/infrastructure', label: 'Open infrastructure' });
  });
});
