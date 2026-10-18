import { mount } from '@vue/test-utils';
import { describe, expect, it } from 'vitest';
import ProgramCard from './ProgramCard.vue';
import { NOW, service } from '../test/infraFixtures';
import { program } from '../test/usageFixtures';
import { testRouter } from '../test/router';

describe('ProgramCard', () => {
  async function mountCard(props: Record<string, unknown>, slot = '') {
    const router = await testRouter('/admin/infrastructure');
    return mount(ProgramCard, {
      props: { name: 'web', service: service('web'), usage: program('web'), cloudRead: true, now: NOW, ...props },
      slots: slot ? { default: slot } : {},
      global: { plugins: [router] },
    });
  }

  it("shows ECS's view and what its tasks use, opening its page", async () => {
    const card = await mountCard({ to: '/admin/infrastructure/programs/web' });
    expect(card.get('h3 a').attributes('href')).toBe('/admin/infrastructure/programs/web');
    expect(card.text()).toContain('Steady');
    expect(card.text()).toContain('1 of 1 running');
    expect(card.get('.infra-program-usage').text()).toBe('web: 41 % CPU of 0.5 vCPU, 612 MiB of 1 GiB, 300 kB/s in, 1.2 MB/s out.');
    expect(card.findAll('li.infra-task')).toHaveLength(1);
    expect(card.findAll('a').map((a) => a.text())).toContain('Its usage and logs: web');
  });

  it("says ECS runs no service of it, and shows its reporting tasks alone where AWS is not read", async () => {
    const missing = await mountCard({ service: null });
    expect(missing.text()).toContain('ECS runs no service of it.');
    const local = await mountCard({ service: null, cloudRead: false });
    expect(local.text()).not.toContain('ECS runs no service');
    expect(local.text()).toContain('1 task reporting');
    expect(local.find('h3 a').exists()).toBe(false);
  });

  it('holds what the page adds, and no usage for a program that samples none', async () => {
    const hostd = await mountCard({ name: 'hostd', usage: null, to: '/admin/fleet', linkLabel: 'Its machines, on the Fleet page' }, '<p class="extra">the machines</p>');
    expect(hostd.get('.extra').text()).toBe('the machines');
    expect(hostd.find('.infra-program-usage').exists()).toBe(false);
    expect(hostd.findAll('a').map((a) => a.text())).toContain('Its machines, on the Fleet page: hostd');
  });
});
