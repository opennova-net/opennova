import { mount } from '@vue/test-utils';
import { describe, expect, it } from 'vitest';
import MachineCard from './MachineCard.vue';
import { fleetServer, machine } from '../test/adminFixtures';
import { NOW } from '../test/usageFixtures';
import { testRouter } from '../test/router';
import type { FleetMachine } from '../types/hostedServer';

describe('MachineCard', () => {
  async function mountCard(m: FleetMachine, servers = [fleetServer(1)]) {
    const router = await testRouter('/admin/servers/machines');
    return mount(MachineCard, { props: { machine: m, servers, now: NOW }, global: { plugins: [router] } });
  }

  it('shows a healthy machine at a glance: its report, its use, its slots naming their servers, its address', async () => {
    const card = await mountCard(machine('3'));
    expect(card.get('h3').text()).toBe('Machine 3');
    expect(card.get('h3 a').attributes('href')).toBe('/admin/machines/3');
    expect(card.text()).toContain('Shared machine');
    expect(card.text()).toContain('Ready');
    expect(card.get('.machine-report').text()).toBe('Last report 4 s ago');
    expect(card.get('.machine-figures').text()).toContain('22 %');
    expect(card.get('.machine-figures').text()).toContain('2 GiB of 4 GiB');
    expect(card.findAll('.machine-flag')).toHaveLength(0);
    expect(card.text()).toContain('2/12 slots');
    const cells = card.findAll('.slot-cell');
    expect(cells).toHaveLength(12);
    // Slot 3 names server 1, its game and players; slot 5 its server by id.
    expect(cells[3].text()).toContain('Server 1');
    expect(cells[3].text()).toContain('JO · 5/32');
    expect(cells[3].get('a').attributes('href')).toBe('/admin/servers/1');
    expect(cells[5].text()).toContain('Starting');
    expect(cells[0].text()).toContain('Free');
    expect(card.get('.machine-address').text()).toContain('203.0.113.7');
    expect(card.get('.machine-address').text()).toContain('13 GB sent this month');
    expect(card.classes()).not.toContain('border-danger/40');
  });

  it('turns its last report red past 30 s', async () => {
    const card = await mountCard(machine('3', { last_report_at: '2026-10-18T11:59:15Z' }));
    const report = card.get('.machine-report');
    expect(report.text()).toBe('Last report 45 s ago: late');
    expect(report.classes()).toContain('text-danger');
    expect(card.classes()).toContain('border-danger/40');
  });

  it('says it is lost and held, held on a protocol mismatch, older than the release', async () => {
    const card = await mountCard(
      machine('3', {
        state: 'lost',
        lost_held: true,
        protocol_mismatch_since: '2026-10-18T11:58:00Z',
        agent_protocol: 2,
        outdated: true,
        agent_image: 'ghcr.io/opennova-net/opennova-hostd@sha256:2222',
      }),
    );
    expect(card.findAll('.machine-flag').map((f) => f.text())).toEqual([
      'Lost, held by the brake',
      'Agent protocol 2 mismatch, held 2 minutes',
      'Older than the release: hostd image',
    ]);
    expect(card.text()).toContain('Lost (no reports)');
  });

  it('names its static address', async () => {
    const card = await mountCard(machine('4', { kind: 'own', slots: 1, slots_used: 1, address: { id: 7, name: 'smoke' }, stats: null }), []);
    expect(card.get('.machine-address').text()).toContain('static smoke');
    expect(card.text()).toContain('Dedicated machine');
    expect(card.find('.machine-figures').exists()).toBe(false);
  });

  it('offers drain and end while it lives, and neither once it has ended', async () => {
    const card = await mountCard(machine('3'));
    const buttons = card.findAll('button');
    expect(buttons.map((b) => b.text())).toEqual(['Drain machine 3', 'End machine 3']);
    await buttons[0].trigger('click');
    await buttons[1].trigger('click');
    expect(card.emitted('drain')).toHaveLength(1);
    expect(card.emitted('end')).toHaveLength(1);

    const ended = await mountCard(machine('5', { state: 'ended', ended_at: '2026-10-18 11:00:00', end_reason: 'drained' }), []);
    expect(ended.findAll('button')).toHaveLength(0);
    expect(ended.find('.slot-strip').exists()).toBe(false);
    expect(ended.text()).toContain('Ended: drained');
  });
});
