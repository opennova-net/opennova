import { mount } from '@vue/test-utils';
import { describe, expect, it } from 'vitest';
import AdminCrumbs from './AdminCrumbs.vue';
import { testRouter } from '../test/router';

async function crumbs(path: string, trail?: { label: string; to?: string }[]) {
  const router = await testRouter(path);
  return mount(AdminCrumbs, { props: trail ? { trail } : {}, global: { plugins: [router] } });
}

describe('AdminCrumbs', () => {
  it("names a section's page by its area alone", async () => {
    const page = await crumbs('/admin/fleet');
    expect(page.get('nav').attributes('aria-label')).toBe('Breadcrumb');
    expect(page.text()).toBe('Hosting');
    expect(page.findAll('a')).toHaveLength(0);
  });

  it('leads from a page under a section back to it', async () => {
    const machine = await crumbs('/admin/fleet/machines/3');
    expect(machine.text()).toBe('Hosting/Fleet');
    expect(machine.get('a').attributes('href')).toBe('/admin/fleet');
    const user = await crumbs('/admin/users/7');
    expect(user.text()).toBe('People and money/Users');
    const program = await crumbs('/admin/infrastructure/programs/web');
    expect(program.get('a').attributes('href')).toBe('/admin/infrastructure');
  });

  it("adds the steps a page names, a link or the page's own words", async () => {
    const page = await crumbs('/admin/jobs/77', [{ label: 'Mail jobs', to: '/admin/jobs?tab=jobs&kind=mail.send' }, { label: 'Job 77' }]);
    expect(page.findAll('a').map((a) => [a.text(), a.attributes('href')])).toEqual([
      ['Background jobs', '/admin/jobs'],
      ['Mail jobs', '/admin/jobs?tab=jobs&kind=mail.send'],
    ]);
    expect(page.text()).toBe('Operations/Background jobs/Mail jobs/Job 77');
  });

  it('shows none without a router (a page mounted alone)', () => {
    expect(mount(AdminCrumbs).find('nav').exists()).toBe(false);
  });

  it('shows none on the Overview', async () => {
    const page = await crumbs('/admin/overview');
    expect(page.find('nav').exists()).toBe(false);
  });
});
