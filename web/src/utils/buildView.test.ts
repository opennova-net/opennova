import { describe, expect, it } from 'vitest';
import {
  buildOf,
  hostdBuilds,
  imagesOnRelease,
  parseAgentVersion,
  portalBuilds,
  programBuilds,
  releaseImageCommit,
  revisionLabel,
  serveBuilds,
  versionText,
} from './buildView';
import { HOSTD_IMAGE, SERVE_IMAGE, machine, overview, programImage, release, rollingWeb, service, task } from '../test/infraFixtures';
import { program, programTask } from '../test/usageFixtures';

describe('the builds the deployment runs', () => {
  it('spells a build as ONE version and short sha, the sha alone with no version', () => {
    expect(versionText({ version: '0.0.22.5922', commit: 'b0c2b38' })).toBe('ONE 0.0.22.5922');
    expect(buildOf('0.0.22', 'b0c2b38f00d')).toEqual({ version: '0.0.22', commit: 'b0c2b38' });
    expect(buildOf(null, null)).toBeNull();
    expect(buildOf('', 'not a sha')).toBeNull();
  });

  it("reads hostd's agent_version: ONE's label, a bare commit, nothing else", () => {
    expect(parseAgentVersion('ONE 0.0.22.5922 (b0c2b38, staging)')).toEqual({ version: '0.0.22.5922', commit: 'b0c2b38' });
    expect(parseAgentVersion('ONE 0.0.22 (b0c2b38)')).toEqual({ version: '0.0.22', commit: 'b0c2b38' });
    expect(parseAgentVersion('ONE 0.0.22')).toEqual({ version: '0.0.22', commit: null });
    expect(parseAgentVersion('f42f7fef09b7e39814d002373ab7a2ca9e81ddc3')).toEqual({ version: null, commit: 'f42f7fe' });
    expect(parseAgentVersion('dev')).toBeNull();
    expect(parseAgentVersion('ONE')).toBeNull();
    expect(parseAgentVersion(null)).toBeNull();
  });

  it("names an image's commit only where it is the release's image", () => {
    const r = release();
    expect(releaseImageCommit(r, 'gate', programImage('gate'))).toBe('abc1234');
    expect(releaseImageCommit(r, 'gate', programImage('web'))).toBeNull();
    expect(releaseImageCommit(release({ image_commits: {} }), 'gate', programImage('gate'))).toBeNull();
    expect(releaseImageCommit(null, 'gate', programImage('gate'))).toBeNull();
  });

  it("takes a program's tasks' own reports first, then their images' commits", () => {
    const usage = program('web', {
      tasks: [
        programTask('4444dddd4444dddd4444dddd4444dddd'),
        programTask('3333cccc3333cccc3333cccc3333cccc', { reporting: false, one_version: null, one_commit: null }),
      ],
    });
    const builds = programBuilds('web', usage, rollingWeb(), release());
    expect(builds).toEqual({
      builds: [
        { build: { version: '0.0.22.5922', commit: 'b0c2b38' }, count: 1, source: 'reported' },
        { build: { version: null, commit: 'abc1234' }, count: 1, source: 'image' },
      ],
      unknown: 0,
      unit: 'task',
    });
    // Without the release, the quiet task's build is not known.
    expect(programBuilds('web', usage, rollingWeb(), null).unknown).toBe(1);
    // A task ECS runs that the samples do not list is counted too.
    const extra = service('gate', { tasks: [task('gate0000aaaa1111aaaa1111aaaa11', 'gate'), task('gate1111', 'gate')] });
    expect(programBuilds('gate', program('gate'), extra, release()).builds.map((b) => b.count)).toEqual([1, 1]);
    // The local stack: its samples alone.
    expect(programBuilds('gate', program('gate'), null, null).builds).toHaveLength(1);
  });

  it("counts the website's, hostd's and the hosted servers' builds", () => {
    expect(portalBuilds(rollingWeb(), release()).builds).toEqual([{ build: { version: null, commit: 'abc1234' }, count: 2, source: 'image' }]);
    const machines = [machine(1), machine(2, { state: 'ended' }), machine(3, { agent_version: null })];
    expect(hostdBuilds(machines)).toEqual({
      builds: [{ build: { version: '0.0.22.5922', commit: 'b0c2b38' }, count: 1, source: 'reported' }],
      unknown: 1,
      unit: 'machine',
    });
    const o = overview({ serve_images: [{ image: SERVE_IMAGE, servers: 2 }, { image: 'other', servers: 1 }] });
    expect(serveBuilds(o, release())).toEqual({
      builds: [{ build: { version: null, commit: 'abc1234' }, count: 2, source: 'image' }],
      unknown: 1,
      unit: 'server',
    });
  });

  it('labels the ECS task revision and says whether every image is the release\'s', () => {
    expect(revisionLabel('opennova-prod-web:43')).toBe('ECS task revision 43');
    expect(revisionLabel('opennova-prod-web')).toBeNull();
    expect(revisionLabel(null)).toBeNull();
    expect(imagesOnRelease(release(), 'hostd', [HOSTD_IMAGE, HOSTD_IMAGE])).toBe(true);
    expect(imagesOnRelease(release(), 'hostd', [HOSTD_IMAGE, 'older'])).toBe(false);
    expect(imagesOnRelease(release(), 'hostd', [null])).toBeNull();
    expect(imagesOnRelease(null, 'hostd', [HOSTD_IMAGE])).toBeNull();
  });
});
