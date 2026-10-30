import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import InfrastructureTab from './InfrastructureTab.vue';
import { fetchInfrastructure } from '../../../api/infra';
import { fetchPrograms } from '../../../api/usage';
import { NOW, SERVE_IMAGE, ago, cloud, localOverview, machine, overview, release, service } from '../../../test/infraFixtures';
import { programsResponse } from '../../../test/usageFixtures';
import { testRouter } from '../../../test/router';

vi.mock('../../../api/infra', () => ({ fetchInfrastructure: vi.fn() }));
vi.mock('../../../api/usage', () => ({ fetchPrograms: vi.fn() }));

const read = vi.mocked(fetchInfrastructure);
const programs = vi.mocked(fetchPrograms);

describe('InfrastructureTab', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountSection() {
    const router = await testRouter('/admin/system/infrastructure');
    wrapper = mount(InfrastructureTab, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  function tiles(page: ReturnType<typeof mount>) {
    return Object.fromEntries(page.findAll('dl.infra-tiles > div').map((tile) => [tile.get('dt').text(), tile.findAll('dd').map((d) => d.text())]));
  }

  // Each part's row of the running builds: its build cell's and its release cell's text.
  function builds(page: ReturnType<typeof mount>) {
    return Object.fromEntries(
      page.findAll('tr.running-build').map((row) => {
        const cells = row.findAll('td');
        const words = (el: Element) =>
          Array.from(el.children)
            .map((c) => c.textContent?.trim() ?? '')
            .filter(Boolean)
            .join(' ');
        return [row.attributes('data-part'), { build: words(cells[0].get('ul').element), release: words(cells[1].element) }];
      }),
    );
  }

  function card(page: ReturnType<typeof mount>, name: string) {
    const found = page.findAll('article.infra-service').find((a) => a.get('h3').text() === name);
    if (!found) throw new Error(`no card for ${name}`);
    return found;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
    read.mockResolvedValue({ ok: true, value: overview() });
    programs.mockResolvedValue({ ok: true, value: programsResponse() });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    vi.useRealTimers();
  });

  it('leads with the release and the build each part runs, whether it is the release\'s a note', async () => {
    const page = await mountSection();
    // The headline comes before the warnings and the tiles.
    const html = page.html();
    expect(html.indexOf('running-builds')).toBeLessThan(html.indexOf('infra-warnings'));
    expect(html.indexOf('running-builds')).toBeLessThan(html.indexOf('infra-tiles'));
    expect(page.get('.release-id').text()).toBe('20261017T120000Z-abc1234');
    expect(page.get('.release-commit').text()).toBe('abc1234');
    expect(page.get('.running-release').text()).toContain('Recorded');

    const b = builds(page);
    expect(Object.keys(b)).toEqual(['gate', 'lobby', 'web', 'fleet', 'worker', 'data', 'portal', 'hostd', 'serve']);
    // A program's build is what its tasks report: ONE's version and the commit's short sha.
    expect(b.gate.build).toBe('ONE 0.0.22.5922 b0c2b38');
    expect(b.gate.release).toBe('Matches the release');
    expect(page.get('tr[data-part="gate"] .build-version').text()).toBe('ONE 0.0.22.5922');
    expect(page.get('tr[data-part="gate"] .build-commit').text()).toBe('b0c2b38');
    // Web mid-rollout: the old task's report, the new one's image commit (it has
    // not reported yet), each counted; off the release, which names its image's commit.
    expect(b.web.build).toBe('ONE 0.0.22.5922 b0c2b38 (1 task) abc1234 (1 task)');
    expect(b.web.release).toBe("Not the release's Release image abc1234");
    // A program none of whose tasks reports: its running task's image commit.
    expect(b.data.build).toBe('abc1234');
    // The website, hostd and the hosted servers, by their images' commits or
    // hostd's own report; the ended machine's agent is not counted.
    expect(b.portal.build).toBe('abc1234');
    expect(b.portal.release).toBe('Matches the release');
    expect(b.hostd.build).toBe('ONE 0.0.22.5922 b0c2b38');
    expect(b.hostd.release).toBe('Matches the release');
    expect(b.serve.build).toBe('abc1234');
    expect(b.serve.release).toBe('Matches the release');
    expect(page.get('tr[data-part="hostd"] a').attributes('href')).toBe('/admin/servers/machines');
    expect(page.get('tr[data-part="web"] a').attributes('href')).toBe('/admin/programs/web');
    // Nowhere a bare task definition or its revision in place of a build: the
    // release header names its build sha.
    expect(page.get('.running-builds').text()).not.toContain('revision');
    expect(page.get('.running-release').text()).toContain('Build abc1234');
    expect(page.get('.running-release').text()).not.toContain('Commit');
    expect(page.text()).not.toMatch(/opennova-prod-\w+:\d+/);
  });

  it('counts each build a part runs, and says where one is not known', async () => {
    read.mockResolvedValue({
      ok: true,
      value: overview({
        machines: [
          machine(1),
          machine(3, { agent_version: 'ONE 0.0.22.5922 (b0c2b38, staging)' }),
          machine(4, { agent_version: 'ONE 0.0.21.5800 (0f0f0f0, staging)', agent_image: 'ghcr.io/opennova-net/opennova-hostd@sha256:0ld' }),
          // An agent from before it reported ONE's label: its commit alone.
          machine(5, { agent_version: 'f42f7fe' }),
          machine(6, { agent_version: null, agent_image: null }),
        ],
        serve_images: [
          { image: SERVE_IMAGE, servers: 3 },
          { image: 'ghcr.io/opennova-net/opennova-serve@sha256:0ld', servers: 1 },
        ],
      }),
    });
    const page = await mountSection();
    const b = builds(page);
    expect(b.hostd.build).toBe(
      'ONE 0.0.22.5922 b0c2b38 (2 machines) ONE 0.0.21.5800 0f0f0f0 (1 machine) f42f7fe (1 machine) Build not known for 1 machine',
    );
    expect(b.hostd.release).toBe("Not the release's Release image abc1234");
    expect(b.serve.build).toBe('abc1234 (3 servers) Build not known for 1 server');
    expect(b.serve.release).toBe("Not the release's Release image abc1234");
  });

  it('says what each part runs before any release names its images', async () => {
    read.mockResolvedValue({
      ok: true,
      value: overview({ cloud: cloud({ release: release({ image_commits: {} }) }), serve_images: [] }),
    });
    const page = await mountSection();
    const b = builds(page);
    expect(b.gate.build).toBe('ONE 0.0.22.5922 b0c2b38');
    expect(b.data.build).toBe('Build not known for 1 task');
    expect(b.portal.build).toBe('Build not known for 2 tasks');
    expect(b.serve.build).toBe('No hosted server runs');
    expect(b.serve.release).toBe('');
  });

  it('sums the deployment up in five tiles and warns of what to look at first', async () => {
    const page = await mountSection();
    expect(page.get('h1').text()).toBe('System');
    expect(page.get('.tab-nav a[aria-current="page"]').text()).toBe('Infrastructure');
    expect(page.get('.page-meta').text()).toContain('Read from AWS');
    expect(read).toHaveBeenCalledTimes(1);
    const t = tiles(page);
    expect(Object.keys(t)).toEqual(['Programs', 'Tasks running', 'Load balancer targets', 'Lobby instances', 'Machines']);
    expect(t['Programs']).toEqual(['5 of 6 steady', 'web rolling out']);
    expect(t['Tasks running']).toEqual(['6 of 7', '1 starting']);
    expect(t['Load balancer targets']).toEqual(['4 of 6 healthy', '4 listeners']);
    expect(t['Lobby instances']).toEqual(['1 live', '1 taking clients, 3 sessions']);
    expect(t['Machines']).toEqual(['1', '1 shared, 0 dedicated, 4 of 12 slots used']);
    // The machines and the lobbies are other pages': their tiles open them.
    const href = (label: string) => page.findAll('dl.infra-tiles > div').find((d) => d.get('dt').text() === label)!.get('a').attributes('href');
    expect(href('Machines')).toBe('/admin/servers/machines');
    expect(href('Lobby instances')).toBe('/admin/system');
    expect(page.findAll('.infra-warnings li').map((w) => w.text())).toEqual([
      'The website (TCP 443): 1 target unhealthy (Target.FailedHealthChecks).',
    ]);
    expect(page.find('.infra-unavailable').exists()).toBe(false);
    const text = page.text();
    expect(text).not.toMatch(/\d+ ?ms\b/);
    expect(text).not.toContain('_seconds');
    expect(text).not.toContain('undefined');
    expect(text).not.toContain('null');
  });

  it('shows each program with its build, its rollout, whether it is the release\'s, its tasks and events', async () => {
    const page = await mountSection();
    const names = page.findAll('article.infra-service h3').map((h) => h.text());
    expect(names).toEqual(['gate', 'lobby', 'web', 'fleet', 'worker', 'data']);

    const gate = card(page, 'gate');
    expect(gate.text()).toContain('Steady');
    expect(gate.text()).toContain('1 of 1 running');
    expect(gate.get('.infra-program-build').text()).toBe('ONE 0.0.22.5922 b0c2b38');
    expect(gate.find('.infra-program-off-release').exists()).toBe(false);
    expect(gate.text()).not.toContain('revision');
    expect(gate.text()).toContain('opennova-novaworld-gate@sha256:0123456789ab');
    expect(gate.findAll('li.infra-deployment')).toHaveLength(0);
    expect(gate.get('details summary').text()).toBe('Recent events (1)');
    expect(gate.get('li.infra-event').text()).toContain('(service gate) has reached a steady state.');

    const web = card(page, 'web');
    expect(web.text()).toContain('Rolling out');
    expect(web.text()).toContain('1 of 2 running');
    expect(web.text()).toContain('1 starting');
    expect(web.get('.infra-program-build').text()).toBe('ONE 0.0.22.5922 b0c2b38 (1 task), abc1234 (1 task)');
    expect(web.get('.infra-program-off-release').text()).toBe("Not the release's");
    const deployments = web.findAll('li.infra-deployment').map((d) => d.text());
    expect(deployments).toHaveLength(2);
    expect(deployments[0]).toContain('New ECS task revision 43: 0 of 1 running, 1 task failed, rollout in progress');
    expect(deployments[1]).toContain('Old ECS task revision 42: 1 of 1 running, rollout completed');
    const tasks = web.findAll('li.infra-task');
    expect(tasks).toHaveLength(2);
    expect(tasks[0].text()).toContain('4444dddd');
    expect(tasks[0].text()).toContain('Healthy');
    expect(tasks[0].text()).toContain('10.40.0.14');
    expect(tasks[0].text()).toContain('0.5 vCPU, 1 GiB');
    expect(tasks[0].text()).toContain('ONE 0.0.22.5922 b0c2b38');
    expect(tasks[0].text()).not.toContain('revision');
    expect(tasks[1].text()).toContain('Health unknown');
    expect(tasks[1].text()).toContain('Pending');
    expect(tasks[1].text()).toContain('abc1234');
    expect(tasks[1].text()).toContain("Not the release's");
    expect(tasks[1].text()).not.toContain('revision');
    expect(tasks[1].text()).toContain('not started');
    expect(tasks[1].text()).toContain('us-east-1b');
    expect(tasks[1].get('span[title="Task 3333cccc3333cccc3333cccc3333cccc"]').text()).toBe('3333cccc');
    const stopped = web.findAll('li.infra-stopped-task');
    expect(stopped).toHaveLength(1);
    expect(stopped[0].text()).toContain('9999ffff');
    expect(stopped[0].text()).toContain('1 min ago');
    expect(stopped[0].text()).toContain('Essential container in task exited; web exited with code 1');
    expect(web.get('details summary').text()).toBe('Recent events (2)');

    const oneOff = page.get('.infra-one-off');
    expect(oneOff.text()).toContain('7777aaaa');
    expect(oneOff.text()).toContain('opennova-prod-backup');
    expect(oneOff.text()).toContain('started 10 min ago');
  });

  it('says what each program uses now and its tasks gone, each card opening its own page', async () => {
    const page = await mountSection();
    expect(programs).toHaveBeenCalledTimes(1);
    const web = card(page, 'web');
    expect(web.get('h3 a').attributes('href')).toBe('/admin/programs/web');
    expect(web.get('.infra-program-usage').text()).toBe('web: 41 % CPU of 0.5 vCPU, 612 MiB of 1 GiB, 300 kB/s in, 1.2 MB/s out.');
    expect(web.find('.infra-program-restarts').exists()).toBe(false);
    const worker = card(page, 'worker');
    expect(worker.get('.infra-program-restarts').text()).toBe('1 task gone in 24 hours');
    expect(worker.get('.infra-program-restarts').classes()).toContain('text-warn');
    // A program none of whose tasks reports, in words and colour.
    const data = card(page, 'data');
    expect(data.get('.infra-program-usage').text()).toBe('data: no report in the last 3 minutes.');
    expect(data.get('.infra-program-usage').classes()).toContain('text-warn');
  });

  it("leaves hostd's daemon and the fleet cluster to the Fleet page, its build alone here", async () => {
    const page = await mountSection();
    expect(page.findAll('article.infra-service h3').map((h) => h.text())).not.toContain('hostd');
    expect(page.find('li.infra-instance').exists()).toBe(false);
    expect(page.find('tr[data-part="hostd"]').exists()).toBe(true);
  });

  it("says when what the programs use could not be read, the rest standing", async () => {
    programs.mockResolvedValue({ ok: false, error: { code: 'db_error', status: 500, message: 'The server hit a database error. Try again shortly.' } });
    const page = await mountSection();
    expect(page.get('.infra-programs-error').text()).toContain('The server hit a database error.');
    expect(card(page, 'gate').text()).toContain('Steady');
    expect(card(page, 'gate').find('.infra-program-usage').exists()).toBe(false);
  });

  it('lists the target groups, the database and the cache and the fleet leader, and no lobby table', async () => {
    const page = await mountSection();
    const targets = page.findAll('tr.infra-target-row');
    expect(targets).toHaveLength(6);
    expect(targets[0].text()).toContain('The gate (UDP 7597)');
    expect(targets[0].text()).toContain('10.40.0.11:7597');
    expect(targets[0].text()).toContain('Healthy');
    expect(targets[3].text()).toContain('Registering');
    expect(targets[3].text()).toContain('Target registration is in progress');
    expect(targets[5].text()).toContain('The website (TCP 443)');
    expect(targets[5].text()).toContain('Unhealthy');
    expect(targets[5].text()).toContain('Health checks failed');
    expect(targets[5].text()).toContain('Target.FailedHealthChecks');

    const db = page.get('.infra-database');
    expect(db.text()).toContain('opennova-prod-db');
    expect(db.text()).toContain('available');
    expect(db.text()).toContain('postgres 17.4');
    expect(db.text()).toContain('db.t4g.small');
    expect(db.text()).toContain('Multi-AZYes');
    expect(db.text()).toContain('us-east-1a, standby in us-east-1b');
    expect(db.text()).toContain('20 GiB gp3');
    const cache = page.get('.infra-cache');
    expect(cache.text()).toContain('opennova-prod-cache');
    expect(cache.text()).toContain('cache.t4g.micro');
    expect(cache.text()).toContain('FailoverAutomatic');
    expect(cache.text()).toContain('Primary, us-east-1a');
    expect(cache.text()).toContain('Replica, us-east-1b');

    // The lobby instances are NovaWorld's tab's; the machines Servers'.
    expect(page.find('tr.infra-lobby-row').exists()).toBe(false);
    expect(page.find('#infra-lobbies').exists()).toBe(false);
    expect(page.find('.infra-machines-summary').exists()).toBe(false);

    const fleet = page.get('.infra-fleet');
    expect(fleet.text()).toContain('fleet-1234abcd');
    expect(fleet.text()).toContain('1 hour ago');
    expect(fleet.text()).toContain('The 3rd');

    // Each AWS resource opens its charts.
    expect(page.findAll('a.infra-resource-link').map((a) => a.attributes('href'))).toEqual([
      '/admin/resources/nlb',
      '/admin/resources/rds',
      '/admin/resources/cache',
    ]);
  });

  it('on the local stack says AWS is not read and shows the database sections alone', async () => {
    read.mockResolvedValue({ ok: true, value: localOverview() });
    const page = await mountSection();
    expect(page.get('.infra-unavailable').text()).toBe('AWS is not read on this stack.');
    expect(page.get('.infra-unavailable').attributes('title')).toContain('ONNET_INFRA_CLUSTER is unset');
    // The programs' own samples still show, without ECS's view or hostd's.
    const cards = page.findAll('article.infra-service');
    expect(cards.map((c) => c.get('h3').text())).toEqual(['gate', 'lobby', 'web', 'fleet', 'worker', 'data']);
    expect(cards[0].text()).toContain('1 task reporting');
    expect(cards[0].text()).not.toContain('No ECS service');
    expect(cards[0].find('li.infra-task').exists()).toBe(false);
    expect(page.find('#infra-targets').exists()).toBe(false);
    expect(page.find('.infra-database').exists()).toBe(false);
    // The builds the programs and the machines' agents report stand without AWS.
    expect(page.get('.running-release').text()).toBe('The release is not read on this stack.');
    const b = builds(page);
    expect(b.gate.build).toBe('ONE 0.0.22.5922 b0c2b38');
    expect(b.gate.release).toBe('');
    expect(b.hostd.build).toBe('ONE 0.0.22.5922 b0c2b38');
    expect(b.portal).toBeUndefined();
    const t = tiles(page);
    expect(t['Programs']).toEqual(['Unknown', 'Not read on this stack']);
    expect(t['Load balancer targets']).toEqual(['Unknown', 'Not read on this stack']);
    expect(t['Lobby instances']).toEqual(['1 live', '1 taking clients, 3 sessions']);
    expect(t['Machines'][0]).toBe('1');
    expect(page.text()).not.toContain('EC2 no longer runs');
  });

  it('says why AWS or one of its sections could not be read, the others standing', async () => {
    read.mockResolvedValue({
      ok: true,
      value: overview({ cloud: cloud({ error: "the ECS agent's credentials endpoint did not answer: curl exited with code 7" }) }),
    });
    let page = await mountSection();
    expect(page.get('.infra-cloud-error').text()).toBe(
      "AWS could not be read: the ECS agent's credentials endpoint did not answer: curl exited with code 7",
    );
    // The programs' samples stand; ECS's view of them does not.
    expect(page.findAll('article.infra-service')).toHaveLength(6);
    expect(page.find('li.infra-task').exists()).toBe(false);
    expect(tiles(page)['Programs']).toEqual(['Unknown', 'AWS could not be read']);
    page.unmount();

    const groups = cloud().target_groups;
    groups[1] = { ...groups[1], targets: [], error: 'elasticloadbalancing answered 403: AccessDenied: not authorized' };
    read.mockResolvedValue({
      ok: true,
      value: overview({
        cloud: cloud({
          target_groups: groups,
          database: null,
          database_error: 'rds answered 403: AccessDenied: User is not authorized to perform: rds:DescribeDBInstances',
          machines_error: 'ec2 answered 403: UnauthorizedOperation: You are not authorized',
        }),
      }),
    });
    page = await mountSection();
    expect(page.findAll('tr.infra-target-row')[1].text()).toContain(
      'Could not be read: elasticloadbalancing answered 403: AccessDenied: not authorized',
    );
    expect(page.get('.infra-database').text()).toContain('rds answered 403: AccessDenied');
    expect(page.get('.infra-cache').text()).toContain('available');
    expect(tiles(page)['Machines'][1]).toContain('EC2 not read: ec2 answered 403: UnauthorizedOperation');
    expect(tiles(page)['Load balancer targets']).toEqual(['3 of 5 healthy', '1 target group not read']);
  });

  it('warns of a failed rollout, a program short of tasks, a missing service, a busy database and no lobby or fleet leader', async () => {
    read.mockResolvedValue({
      ok: true,
      value: overview({
        cloud: cloud({
          services: [
            service('gate', {
              deployments: [{ ...service('gate').deployments[0], rollout_state: 'FAILED', rollout_reason: 'tasks failed to start' }],
            }),
            service('worker', { running: 0, tasks: [] }),
            service('fleet', { task_definition: 'opennova-prod-fleet:9', on_release: false }),
          ],
          missing_services: ['lobby (MISSING)'],
          database: { ...cloud().database!, status: 'modifying' },
          target_groups: [],
        }),
        lobbies: [],
        fleet: { leader: null, generation: 0, elected_at: null },
      }),
    });
    const page = await mountSection();
    expect(page.findAll('.infra-warnings li').map((w) => w.text())).toEqual([
      "gate's last rollout failed: tasks failed to start.",
      'worker has 0 of 1 tasks running.',
      'ECS has no service lobby (MISSING).',
      'The database is modifying.',
      'fleet is not on the release.',
      'No lobby instance is taking clients.',
      'No fleet leader is elected.',
    ]);
    expect(card(page, 'gate').text()).toContain('Rollout failed');
    expect(card(page, 'worker').text()).toContain('Short of tasks');
    expect(card(page, 'worker').text()).toContain('No tasks.');
    // A program ECS runs no service of.
    expect(card(page, 'lobby').text()).toContain('No ECS service');
    expect(builds(page).fleet.release).toBe("Not the release's Release image abc1234");
    expect(page.get('.infra-fleet').text()).toContain('None elected');
  });

  it('says a deploy is rolling out while one is, naming the programs it rolled, and labels the updater', async () => {
    const rolled = { task_definition: 'opennova-prod-web:9', on_release: false };
    read.mockResolvedValue({
      ok: true,
      value: overview({
        cloud: cloud({
          services: [
            service('gate'),
            service('lobby', {
              deployments: [
                { ...service('lobby').deployments[0], rollout_state: 'IN_PROGRESS', task_definition: 'opennova-prod-lobby:8' },
                { ...service('lobby').deployments[0], id: 'ecs-svc/lobby-old', status: 'ACTIVE' },
              ],
            }),
            service('web', rolled),
            service('fleet', { ...rolled, task_definition: 'opennova-prod-fleet:9' }),
            service('worker'),
          ],
          target_groups: [
            {
              key: 'update',
              name: 'opennova-prod-update',
              targets: [{ id: '10.40.0.13', port: 20044, zone: 'us-east-1a', state: 'healthy', reason: null, description: null }],
              error: null,
            },
          ],
        }),
      }),
    });
    const page = await mountSection();
    expect(page.findAll('.infra-warnings li').map((w) => w.text())).toEqual([
      'A deploy is rolling out (lobby now); web and fleet run their new revisions.',
    ]);
    expect(page.get('tr.infra-target-row').text()).toContain('The updater (TCP 20044, 20556, 21068, 21580)');
  });

  it('shows a failed read and keeps polling', async () => {
    read.mockResolvedValue({ ok: false, error: { code: 'forbidden', status: 403, message: 'This account is not an admin.' } });
    const page = await mountSection();
    expect(page.get('[role="alert"]').text()).toBe('This account is not an admin.');
    expect(page.find('article.infra-service').exists()).toBe(false);
  });

  it('reads no release yet as none, and says when it could not be read', async () => {
    read.mockResolvedValue({ ok: true, value: overview({ cloud: cloud({ release: null, release_none: true }) }) });
    let page = await mountSection();
    expect(page.get('.running-release').text()).toBe('No release is recorded yet.');
    expect(builds(page).gate.build).toBe('ONE 0.0.22.5922 b0c2b38');
    expect(ago(0)).toBe('2026-10-17T12:01:30Z');
    page.unmount();

    read.mockResolvedValue({
      ok: true,
      value: overview({ cloud: cloud({ release: null, release_error: 'ssm answered 400: ParameterNotFound' }) }),
    });
    page = await mountSection();
    expect(page.get('.running-release').text()).toBe('The release could not be read: ssm answered 400: ParameterNotFound');
  });
});
