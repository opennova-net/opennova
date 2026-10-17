import { describe, expect, it } from 'vitest';
import { pickCloud, pickOverview, pickTask } from './infra';
import { localOverview, overview } from '../test/infraFixtures';

// The reply as the server sends it: JSON, so a round trip through text.
const wire = (value: unknown): unknown => JSON.parse(JSON.stringify(value));

describe('the infrastructure view client', () => {
  it('reads a deployment and the local stack as the server sends them', () => {
    expect(pickOverview(wire(overview()))).toEqual(overview());
    expect(pickOverview(wire(localOverview()))).toEqual(localOverview());
  });

  it('refuses a reply that is not the view', () => {
    expect(pickOverview(null)).toBeNull();
    expect(pickOverview([])).toBeNull();
    expect(pickOverview({ ...overview(), at: 3 })).toBeNull();
    expect(pickOverview({ ...overview(), lobbies: 'none' })).toBeNull();
    expect(pickOverview({ ...overview(), machines: [{ id: 'x' }] })).toBeNull();
    expect(pickOverview({ ...overview(), hosting: null })).toBeNull();
    expect(pickOverview({ ...overview(), cloud: { unavailable: 'x' } })).toBeNull();
    const broken = wire(overview()) as { cloud: { services: unknown[] } };
    broken.cloud.services[0] = { name: 'gate' };
    expect(pickOverview(broken)).toBeNull();
  });

  it('fills what a section leaves out, and keeps null sections null', () => {
    const cloud = pickCloud({ configured: true, read_at: '2026-10-17T12:00:00Z', cluster: 'c', region: 'r', database: null, cache: null, release: null });
    expect(cloud).not.toBeNull();
    if (!cloud || !cloud.configured) return;
    expect(cloud.services).toEqual([]);
    expect(cloud.target_groups).toEqual([]);
    expect(cloud.database).toBeNull();
    expect(cloud.release).toBeNull();
    expect(cloud.release_none).toBe(false);
    expect(pickCloud({ configured: true, database: { identifier: 'x' } })).toBeNull();
    expect(pickCloud({ configured: false })).toEqual({ configured: false, unavailable: 'AWS is not read here.' });

    const task = pickTask({ id: 'abc', containers: [{ name: 'web', exit_code: 2 }] });
    expect(task?.health).toBe('UNKNOWN');
    expect(task?.on_release).toBeNull();
    expect(task?.containers[0]).toEqual({
      name: 'web',
      image: '',
      image_digest: null,
      last_status: '',
      health: 'UNKNOWN',
      exit_code: 2,
      reason: null,
    });
    expect(pickTask({ id: '' })).toBeNull();
  });
});
