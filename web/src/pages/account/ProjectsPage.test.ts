import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Router } from 'vue-router';
import ProjectsPage from './ProjectsPage.vue';
import { fetchMyProjects } from '../../api/products';
import { myProjects, project } from '../../test/productFixtures';
import { testRouter } from '../../test/router';

const { refreshSession } = vi.hoisted(() => ({ refreshSession: vi.fn() }));

vi.mock('../../api/products', () => ({ fetchMyProjects: vi.fn() }));
vi.mock('../../composables/useSession', async () => {
  const { shallowRef } = await import('vue');
  return { useSession: () => ({ user: shallowRef(null), refresh: refreshSession }) };
});

const mine = vi.mocked(fetchMyProjects);

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
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('heads the page with its allowance as a line of facts, New project and Refresh', async () => {
    const page = await mountPage();
    expect(page.get('h1').text()).toBe('Projects');
    expect(page.get('.page-meta').findAll(':scope > span').map((s) => s.text())).toEqual(['2 of 3 projects', '·', '512 MiB of 10 GiB']);
    expect(page.get('#projects-storage').attributes('title')).toBe('212 MiB of it is release packages');
    expect(page.find('.allowance-meter').exists()).toBe(false);
    expect(page.find('.card').exists()).toBe(false);
    const start = page.findAll('a').find((a) => a.text() === 'New project');
    expect(start?.attributes('href')).toBe('/me/projects/new');
    expect(start?.attributes('aria-describedby')).toBe('projects-allowance');
    expect(page.findAll('button').some((b) => b.text() === 'Refresh')).toBe(true);
    expect(page.text()).not.toMatch(/pre-1\.0|experimental/i);
  });

  it('lists the projects in a table: name and key, what each is, who gets it, newest revision, published release, size', async () => {
    const page = await mountPage();
    expect(page.findAll('thead th').map((th) => th.text())).toEqual(['Project', 'What it is', 'Who gets it', 'Newest', 'Published', 'Stored']);
    const rows = page.findAll('tr.project-row');
    expect(rows.map((r) => r.get('th a').text())).toEqual(['Night Ops', 'Rangers']);
    expect(rows.map((r) => r.get('th a').attributes('href'))).toEqual(['/me/projects/NIGHTOPS', '/me/projects/RANGERS']);
    const cells = (i: number) => rows[i].findAll('td').map((td) => td.text());
    expect(rows[0].get('th').text()).toContain('NIGHTOPS');
    expect(cells(0)).toEqual(['A mod of Joint Operations nightops', 'Public', 'r3', '0.2.0', '300 MiB']);
    expect(cells(1)).toEqual(['A game for Delta Force Xtreme 2', 'Private', 'None', 'None', '0 B']);
    expect(rows[0].get('.badge').classes()).toContain('badge-ok');
    expect(page.text()).not.toContain('—');
  });

  it("names a mod's game from the reply's games, else by its key", async () => {
    mine.mockResolvedValue({
      ok: true,
      value: myProjects({
        projects: [project('MYMOD', { kind: 'expansion', base: 'RANGERS', expansion_code: 'mymod' }), project('OURMOD', { kind: 'expansion', base: 'ACIDGAMBIT' })],
        games: [{ key: 'ACIDGAMBIT', title: 'OP: Acid Gambit', game: 'jo' }],
      }),
    });
    const page = await mountPage();
    const kinds = page.findAll('tr.project-row').map((r) => r.findAll('td')[0].text());
    expect(kinds).toEqual(['A mod of RANGERS mymod', 'A mod of OP: Acid Gambit']);
  });

  it('offers no new project at the most an account may have, and says so in one line', async () => {
    mine.mockResolvedValue({ ok: true, value: myProjects({ usage: { projects: 3, bytes: 0, release_bytes: 0 } }) });
    const page = await mountPage();
    expect(page.findAll('a').some((a) => a.text() === 'New project')).toBe(false);
    expect(page.get('#projects-allowance').classes()).toContain('text-warn');
    expect(page.get('.projects-blocked').text()).toBe('You have 3 projects, the most an account may have.');
  });

  it('says when storage is full, and nothing while there is room', async () => {
    const roomy = await mountPage();
    expect(roomy.find('.projects-blocked').exists()).toBe(false);
    wrapper?.unmount();

    mine.mockResolvedValue({ ok: true, value: myProjects({ usage: { projects: 2, bytes: 10 * 1024 * 1024 * 1024, release_bytes: 0 } }) });
    const page = await mountPage();
    expect(page.get('.projects-blocked').text()).toContain('Your storage is full');
    expect(page.get('#projects-storage').classes()).toContain('text-warn');
    expect(page.findAll('a').some((a) => a.text() === 'New project')).toBe(true);
  });

  it('says when the account has none yet', async () => {
    mine.mockResolvedValue({ ok: true, value: myProjects({ projects: [], usage: { projects: 0, bytes: 0, release_bytes: 0 } }) });
    const page = await mountPage();
    expect(page.get('.table-state').text()).toBe('No projects yet.');
  });

  it('says why the projects could not be read, and sends an ended session to the login', async () => {
    mine.mockResolvedValue({ ok: false, error: { code: 'db_error', status: 500, field: '', message: 'The server hit a database error. Try again shortly.' } });
    const page = await mountPage();
    expect(page.get('[role="alert"]').text()).toBe('The server hit a database error. Try again shortly.');
    expect(page.get('.table-state').text()).toBe('Not loaded.');
    expect(page.findAll('a').some((a) => a.text() === 'New project')).toBe(false);

    mine.mockResolvedValue({ ok: true, value: myProjects() });
    await page.findAll('button').find((b) => b.text() === 'Refresh')!.trigger('click');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('');
    expect(page.findAll('tr.project-row')).toHaveLength(2);
    wrapper?.unmount();

    mine.mockResolvedValue({ ok: false, error: { code: 'unauthorized', status: 401, field: '', message: 'ended' } });
    await mountPage();
    expect(refreshSession).toHaveBeenCalled();
    expect(router.currentRoute.value.fullPath).toBe('/login?redirect=/me/projects');
  });
});
