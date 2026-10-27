import { mount } from '@vue/test-utils';
import { describe, expect, it } from 'vitest';
import AdminCrumbs from './AdminCrumbs.vue';
import { testRouter } from '../test/router';

async function crumbs(path: string, trail?: { label: string; to?: string }[]) {
  const router = await testRouter(path);
  return mount(AdminCrumbs, { props: trail ? { trail } : {}, global: { plugins: [router] } });
}

describe('AdminCrumbs', () => {
  it("shows none on a section's own page", async () => {
    const page = await crumbs('/admin/servers');
    expect(page.find('nav').exists()).toBe(false);
  });

  it('leads from a page under a section back to it', async () => {
    const machine = await crumbs('/admin/machines/3');
    expect(machine.text()).toBe('Servers');
    expect(machine.get('a').attributes('href')).toBe('/admin/servers');
    const player = await crumbs('/admin/players/7');
    expect(player.get('a').attributes('href')).toBe('/admin/players');
    const program = await crumbs('/admin/programs/web');
    expect(program.get('a').attributes('href')).toBe('/admin/system');
  });

  it("adds the steps a page names, a link or the page's own words", async () => {
    const page = await crumbs('/admin/jobs/77', [{ label: 'Mail jobs', to: '/admin/system/jobs?tab=jobs&kind=mail.send' }, { label: 'Job 77' }]);
    expect(page.findAll('a').map((a) => [a.text(), a.attributes('href')])).toEqual([
      ['System', '/admin/system'],
      ['Mail jobs', '/admin/system/jobs?tab=jobs&kind=mail.send'],
    ]);
    expect(page.text()).toBe('System/Mail jobs/Job 77');
  });

  it('shows none without a router (a page mounted alone)', () => {
    expect(mount(AdminCrumbs).find('nav').exists()).toBe(false);
  });

  it('shows none on the Overview', async () => {
    const page = await crumbs('/admin');
    expect(page.find('nav').exists()).toBe(false);
  });
});
