import { mount } from '@vue/test-utils';
import { describe, expect, it } from 'vitest';
import ProgramCard from './ProgramCard.vue';
import { NOW, release, service } from '../test/infraFixtures';
import { program, programTask } from '../test/usageFixtures';
import { testRouter } from '../test/router';

describe('ProgramCard', () => {
  async function mountCard(props: Record<string, unknown>, slot = '') {
    const router = await testRouter('/admin/system/infrastructure');
    return mount(ProgramCard, {
      props: { name: 'web', service: service('web'), usage: program('web'), cloudRead: true, now: NOW, ...props },
      slots: slot ? { default: slot } : {},
      global: { plugins: [router] },
    });
  }

  it("shows ECS's view and what its tasks use, opening its page", async () => {
    const card = await mountCard({ to: '/admin/programs/web' });
    expect(card.get('h3 a').attributes('href')).toBe('/admin/programs/web');
    expect(card.text()).toContain('Steady');
    expect(card.text()).toContain('1 of 1 running');
    expect(card.get('.infra-program-usage').text()).toBe('web: 41 % CPU of 0.5 vCPU, 612 MiB of 1 GiB, 300 kB/s in, 1.2 MB/s out.');
    expect(card.findAll('li.infra-task')).toHaveLength(1);
    expect(card.findAll('a').map((a) => a.text())).toContain('Usage and logs: web');
    // Its build heads it, with no ECS task revision beside it.
    expect(card.get('.infra-program-build').text()).toBe('ONE 0.0.22.5922 b0c2b38');
    expect(card.text()).not.toContain('revision');
    // On the release's revision and with no task gone, it says nothing of either.
    expect(card.text()).not.toContain("Not the release's");
    expect(card.find('.infra-program-restarts').exists()).toBe(false);
  });

  it('marks a revision off the release and the tasks gone in 24 hours', async () => {
    const card = await mountCard({ service: service('web', { on_release: false }), usage: program('web', { restarts_24h: 2 }) });
    expect(card.get('.infra-program-off-release').text()).toBe("Not the release's");
    expect(card.get('.infra-program-build').text()).toBe('ONE 0.0.22.5922 b0c2b38');
    expect(card.get('.infra-program-restarts').text()).toBe('2 tasks gone in 24 hours');
  });

  it("names a task that has not reported by its image's commit, else says its build is not known", async () => {
    const quiet = program('web', { tasks: [programTask('web0000aaaa1111aaaa1111aaaa11', { reporting: false, one_version: null, one_commit: null })] });
    const known = await mountCard({ usage: quiet, release: release() });
    expect(known.get('.infra-program-build').text()).toBe('abc1234');
    expect(known.get('li.infra-task').text()).toContain('abc1234');
    // No release to name its image's commit.
    const unknown = await mountCard({ usage: quiet });
    expect(unknown.get('.infra-program-build').text()).toBe('build not known for 1 task');
  });

  it('says ECS runs no service of it, and shows its reporting tasks alone where AWS is not read', async () => {
    const missing = await mountCard({ service: null });
    expect(missing.text()).toContain('No ECS service');
    const local = await mountCard({ service: null, cloudRead: false });
    expect(local.text()).not.toContain('No ECS service');
    expect(local.text()).toContain('1 task reporting');
    expect(local.find('h3 a').exists()).toBe(false);
  });

  it('holds what the page adds, and no usage for a program that samples none', async () => {
    const hostd = await mountCard({ name: 'hostd', usage: null, to: '/admin/servers/machines' }, '<p class="extra">the machines</p>');
    expect(hostd.get('.extra').text()).toBe('the machines');
    expect(hostd.find('.infra-program-usage').exists()).toBe(false);
  });
});
