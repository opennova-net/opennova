import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import InfraSection from './InfraSection.vue';
import { fetchInfrastructure } from '../../api/infra';
import { fetchPrograms } from '../../api/usage';
import { NOW, ago, cloud, localOverview, overview, service } from '../../test/infraFixtures';
import { programsResponse } from '../../test/usageFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/infra', () => ({ fetchInfrastructure: vi.fn() }));
vi.mock('../../api/usage', () => ({ fetchPrograms: vi.fn() }));

const read = vi.mocked(fetchInfrastructure);
const programs = vi.mocked(fetchPrograms);

describe('InfraSection', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountSection() {
    const router = await testRouter('/admin/infrastructure');
    wrapper = mount(InfraSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  function tiles(page: ReturnType<typeof mount>) {
    return Object.fromEntries(page.findAll('dl.infra-tiles > div').map((tile) => [tile.get('dt').text(), tile.findAll('dd').map((d) => d.text())]));
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

  it('sums the deployment up in six cards and warns of what to look at first', async () => {
    const page = await mountSection();
    expect(page.get('h1').text()).toBe('Infrastructure');
    expect(read).toHaveBeenCalledTimes(1);
    const t = tiles(page);
    expect(Object.keys(t)).toEqual(['Programs', 'Tasks running', 'Load balancer targets', 'Release', 'Lobby instances', 'Fleet machines']);
    expect(t['Programs']).toEqual(['5 of 6 steady', 'web rolling out']);
    expect(t['Tasks running']).toEqual(['6 of 7', '1 starting']);
    expect(t['Load balancer targets']).toEqual(['4 of 6 healthy', 'Across 4 listeners']);
    expect(t['Release']).toEqual(['abc1234', '20261017T120000Z-abc1234. Every program runs it']);
    expect(t['Lobby instances']).toEqual(['1 live', '1 taking clients, 3 sessions']);
    expect(t['Fleet machines']).toEqual(['1', '4 of 12 slots used']);
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

  it('shows each program with its rollout, its revision against the release, its tasks and events', async () => {
    const page = await mountSection();
    const names = page.findAll('article.infra-service h3').map((h) => h.text());
    expect(names).toEqual(['gate', 'lobby', 'web', 'fleet', 'worker', 'data']);

    const gate = card(page, 'gate');
    expect(gate.text()).toContain('Steady');
    expect(gate.text()).toContain('1 of 1 running');
    expect(gate.text()).toContain("The release's revision");
    expect(gate.text()).toContain('opennova-novaworld-gate@sha256:0123456789ab');
    expect(gate.findAll('li.infra-deployment')).toHaveLength(0);
    expect(gate.get('details summary').text()).toBe('Recent events (1)');
    expect(gate.get('li.infra-event').text()).toContain('(service gate) has reached a steady state.');

    const web = card(page, 'web');
    expect(web.text()).toContain('Rolling out');
    expect(web.text()).toContain('1 of 2 running');
    expect(web.text()).toContain('1 starting');
    expect(web.text()).toContain("Not the release's revision");
    const deployments = web.findAll('li.infra-deployment').map((d) => d.text());
    expect(deployments).toHaveLength(2);
    expect(deployments[0]).toContain('New opennova-prod-web:43: 0 of 1 running, 1 task failed, rollout in progress');
    expect(deployments[1]).toContain('Old opennova-prod-web:42: 1 of 1 running, rollout completed');
    const tasks = web.findAll('li.infra-task');
    expect(tasks).toHaveLength(2);
    expect(tasks[0].text()).toContain('4444dddd');
    expect(tasks[0].text()).toContain('Healthy');
    expect(tasks[0].text()).toContain('10.40.0.14');
    expect(tasks[0].text()).toContain('0.5 vCPU, 1 GiB');
    expect(tasks[0].text()).not.toContain('Revision');
    expect(tasks[1].text()).toContain('Health unknown');
    expect(tasks[1].text()).toContain('Pending');
    expect(tasks[1].text()).toContain('Revision 43');
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
    expect(web.get('h3 a').attributes('href')).toBe('/admin/infrastructure/programs/web');
    expect(web.get('.infra-program-usage').text()).toBe('web: 41 % CPU of 0.5 vCPU, 612 MiB of 1 GiB, 300 kB/s in, 1.2 MB/s out.');
    expect(web.get('.infra-program-restarts').text()).toBe('No task gone in the last 24 hours.');
    const worker = card(page, 'worker');
    expect(worker.get('.infra-program-restarts').text()).toBe('1 task gone in the last 24 hours (a crash, a deploy or a scale-in).');
    expect(worker.get('.infra-program-restarts').classes()).toContain('text-warn');
    // A program none of whose tasks reports, in words and colour.
    const data = card(page, 'data');
    expect(data.get('.infra-program-usage').text()).toBe('data: no report in the last 3 minutes.');
    expect(data.get('.infra-program-usage').classes()).toContain('text-warn');
  });

  it("leaves hostd's daemon and the fleet cluster to the Fleet page", async () => {
    const page = await mountSection();
    expect(page.findAll('article.infra-service h3').map((h) => h.text())).not.toContain('hostd');
    expect(page.find('li.infra-instance').exists()).toBe(false);
  });

  it("says when what the programs use could not be read, the rest standing", async () => {
    programs.mockResolvedValue({ ok: false, error: { code: 'db_error', status: 500, message: 'The server hit a database error. Try again shortly.' } });
    const page = await mountSection();
    expect(page.get('.infra-programs-error').text()).toContain('The server hit a database error.');
    expect(card(page, 'gate').text()).toContain('Steady');
    expect(card(page, 'gate').find('.infra-program-usage').exists()).toBe(false);
  });

  it('lists the target groups, the database and the cache, the lobbies, the fleet leader and the machines', async () => {
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

    const lobbies = page.findAll('tr.infra-lobby-row');
    expect(lobbies).toHaveLength(2);
    expect(lobbies[0].text()).toContain('Taking clients');
    expect(lobbies[0].text()).toContain('3 sessions');
    expect(lobbies[0].text()).toContain('1 listed server');
    expect(lobbies[1].text()).toContain('Not seen');
    expect(lobbies[1].text()).toContain('2 min ago');

    const fleet = page.get('.infra-fleet');
    expect(fleet.text()).toContain('fleet-1234abcd');
    expect(fleet.text()).toContain('1 hour ago');
    expect(fleet.text()).toContain('The 3rd');

    // The machines in one line, their page the Fleet's.
    const machines = page.get('.infra-machines-summary');
    expect(machines.text()).toContain('1 machine live (1 shared, 0 dedicated), 4 of 12 slots in use.');
    expect(machines.get('a').attributes('href')).toBe('/admin/fleet');

    // Each AWS resource opens its charts.
    expect(page.findAll('a.infra-resource-link').map((a) => a.attributes('href'))).toEqual([
      '/admin/infrastructure/resources/nlb',
      '/admin/infrastructure/resources/rds',
      '/admin/infrastructure/resources/cache',
    ]);
  });

  it('on the local stack says AWS is not read and shows the database sections alone', async () => {
    read.mockResolvedValue({ ok: true, value: localOverview() });
    const page = await mountSection();
    expect(page.get('.infra-unavailable').text()).toContain('ONNET_INFRA_CLUSTER is unset');
    // The programs' own samples still show, without ECS's view or hostd's.
    const cards = page.findAll('article.infra-service');
    expect(cards.map((c) => c.get('h3').text())).toEqual(['gate', 'lobby', 'web', 'fleet', 'worker', 'data']);
    expect(cards[0].text()).toContain('1 task reporting');
    expect(cards[0].text()).not.toContain('ECS runs no service');
    expect(cards[0].find('li.infra-task').exists()).toBe(false);
    expect(page.find('#infra-targets').exists()).toBe(false);
    expect(page.find('.infra-database').exists()).toBe(false);
    const t = tiles(page);
    expect(t['Programs']).toEqual(['Unknown', 'Not read on this stack']);
    expect(t['Load balancer targets']).toEqual(['Unknown', 'Not read on this stack']);
    expect(t['Lobby instances']).toEqual(['1 live', '1 taking clients, 3 sessions']);
    expect(page.findAll('tr.infra-lobby-row')).toHaveLength(2);
    expect(page.get('.infra-machines-summary').text()).toContain('1 machine live');
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
    expect(page.text()).toContain('EC2 could not be read: ec2 answered 403: UnauthorizedOperation');
    expect(tiles(page)['Load balancer targets']).toEqual(['3 of 5 healthy', '1 target group could not be read']);
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
      "gate's last rollout failed: tasks failed to start. ECS rolls a failed deployment back.",
      'worker has 0 of 1 tasks running.',
      'ECS has no service lobby (MISSING).',
      'The database is modifying.',
      "fleet runs opennova-prod-fleet:9, not the release's revision: a deploy or a rollback did not finish.",
      'No lobby instance has been seen taking clients in the last 15 seconds.',
      'No fleet process has been elected yet: nothing runs the fleet.',
    ]);
    expect(card(page, 'gate').text()).toContain('Rollout failed');
    expect(card(page, 'worker').text()).toContain('Short of tasks');
    expect(card(page, 'worker').text()).toContain('No task runs.');
    // A program ECS runs no service of.
    expect(card(page, 'lobby').text()).toContain('ECS runs no service of it.');
    expect(tiles(page)['Release'][1]).toBe('20261017T120000Z-abc1234. fleet runs another revision');
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
      'A deploy is rolling out (lobby now): web and fleet run their new revisions, and the release is recorded once every program does.',
    ]);
    expect(page.get('tr.infra-target-row').text()).toContain('The updater (TCP 20044, 20556, 21068, 21580)');
  });

  it('shows a failed read and keeps polling', async () => {
    read.mockResolvedValue({ ok: false, error: { code: 'forbidden', status: 403, message: 'This account is not an admin.' } });
    const page = await mountSection();
    expect(page.get('[role="alert"]').text()).toBe('This account is not an admin.');
    expect(page.find('article.infra-service').exists()).toBe(false);
  });

  it('reads no release yet as none', async () => {
    read.mockResolvedValue({ ok: true, value: overview({ cloud: cloud({ release: null, release_none: true }) }) });
    const page = await mountSection();
    expect(tiles(page)['Release']).toEqual(['None yet', 'No app deploy has recorded a release']);
    expect(ago(0)).toBe('2026-10-17T12:01:30Z');
  });
});
