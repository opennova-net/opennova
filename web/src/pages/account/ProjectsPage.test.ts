import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Router } from 'vue-router';
import ProjectsPage from './ProjectsPage.vue';
import { fetchMyProjects, fetchProjects } from '../../api/products';
import { ACIDGAMBIT_PROJECT, NIGHTOPS_PROJECT, RANGERS_PROJECT, myProjects, project } from '../../test/productFixtures';
import { testRouter } from '../../test/router';

const { refreshSession } = vi.hoisted(() => ({ refreshSession: vi.fn() }));

vi.mock('../../api/products', () => ({ fetchMyProjects: vi.fn(), fetchProjects: vi.fn() }));
vi.mock('../../composables/useSession', async () => {
  const { shallowRef } = await import('vue');
  return { useSession: () => ({ user: shallowRef(null), refresh: refreshSession }) };
});

const mine = vi.mocked(fetchMyProjects);
const readable = vi.mocked(fetchProjects);

describe('ProjectsPage', () => {
  let wrapper: VueWrapper | null = null;
  let router: Router;

  async function mountPage() {
    router = await testRouter('/me/projects');
    wrapper = mount(ProjectsPage, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    mine.mockResolvedValue({ ok: true, value: myProjects() });
    readable.mockResolvedValue({ ok: true, value: [ACIDGAMBIT_PROJECT, NIGHTOPS_PROJECT, RANGERS_PROJECT] });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('lists the projects with what each is, who gets it, its newest revision and its size, and the room left', async () => {
    const page = await mountPage();
    expect(page.get('header').text()).toContain('Your mods and games on NovaWorld. Make one here or put a project on NovaWorld from the OpenNova Editor');
    expect(page.get('#projects-allowance').text()).toBe('2 of 10 projects used');
    expect(page.get('#projects-storage').text()).toBe('300 MiB of 4 GiB stored');
    const start = page.findAll('a').find((a) => a.text() === 'New project');
    expect(start?.attributes('href')).toBe('/me/projects/new');

    const cards = page.findAll('article.project-card');
    expect(cards.map((c) => c.get('h2 a').text())).toEqual(['Night Ops', 'Rangers']);
    expect(cards.map((c) => c.get('h2 a').attributes('href'))).toEqual(['/me/projects/NIGHTOPS', '/me/projects/RANGERS']);
    expect(cards[0].text()).toContain('Public');
    expect(cards[0].text()).toMatch(/NIGHTOPS, A mod of Joint Operations\s+\(short name nightops\)/);
    expect(cards[0].text()).toContain('Newest revision r3, 300 MiB stored');
    expect(cards[1].text()).toContain('Private');
    expect(cards[1].text()).toContain('RANGERS, A game for Delta Force Xtreme 2');
    expect(cards[1].text()).toContain('Nothing pushed yet, 0 B stored');
    expect(page.text()).not.toContain('—');
  });

  it("names a mod's game by its key when the games could not be read", async () => {
    readable.mockResolvedValue({ ok: false, error: { code: 'network', status: null, field: '', message: 'offline' } });
    mine.mockResolvedValue({ ok: true, value: myProjects({ projects: [project('MYMOD', { kind: 'expansion', base: 'RANGERS', expansion_code: 'mymod' })] }) });
    const page = await mountPage();
    expect(page.get('article.project-card').text()).toContain('MYMOD, A mod of RANGERS');
  });

  it('offers no new project at the most an account may have, and says so', async () => {
    mine.mockResolvedValue({ ok: true, value: myProjects({ usage: { projects: 10, bytes: 0 } }) });
    const page = await mountPage();
    expect(page.findAll('a').some((a) => a.text() === 'New project')).toBe(false);
    expect(page.text()).toContain('You have 10 projects, the most an account may have. Delete one to make another.');
  });

  it('says when the account has none yet', async () => {
    mine.mockResolvedValue({ ok: true, value: myProjects({ projects: [], usage: { projects: 0, bytes: 0 } }) });
    const page = await mountPage();
    expect(page.text()).toContain('You have no project on NovaWorld yet.');
  });

  it('says why the projects could not be read, and sends an ended session to the login', async () => {
    mine.mockResolvedValue({ ok: false, error: { code: 'db_error', status: 500, field: '', message: 'The server hit a database error. Try again shortly.' } });
    const page = await mountPage();
    expect(page.get('[role="alert"]').text()).toBe('The server hit a database error. Try again shortly.');
    wrapper?.unmount();

    mine.mockResolvedValue({ ok: false, error: { code: 'unauthorized', status: 401, field: '', message: 'ended' } });
    await mountPage();
    expect(refreshSession).toHaveBeenCalled();
    expect(router.currentRoute.value.fullPath).toBe('/login?redirect=/me/projects');
  });
});
