import { describe, expect, it } from 'vitest';
import { findGame, gameLink, kindLine, listGames, publishedLabel, releasesOf, releasesWorking, updaterProblems } from './gamesView';
import { DFX2_SET, ESCALATION_SET, JO_SET, RUNTIME_SET, dataSetsResponse, release, runtimeRelease } from '../test/dataSetFixtures';

const ALL = [JO_SET, ESCALATION_SET, DFX2_SET, RUNTIME_SET];

describe('the games view', () => {
  it("addresses a game by its project key, or by its id when it has none", () => {
    expect(gameLink(JO_SET)).toBe('/admin/games/JO');
    expect(gameLink(RUNTIME_SET)).toBe('/admin/games/9');
    expect(findGame(ALL, 'JOX01')).toBe(ESCALATION_SET);
    expect(findGame(ALL, '9')).toBe(RUNTIME_SET);
    expect(findGame(ALL, 'jo')).toBeNull();
    expect(findGame(ALL, '99')).toBeNull();
  });

  it('says what kind a game is', () => {
    const lobbies = dataSetsResponse().games;
    expect(kindLine(JO_SET, ALL, lobbies)).toBe('Base game Joint Operations, lobby Joint Operations');
    expect(kindLine(ESCALATION_SET, ALL, lobbies)).toBe('Expansion jox01 of Joint Operations');
    expect(kindLine(RUNTIME_SET, ALL, lobbies)).toBe('The OpenNova runtime, built by CI');
  });

  it("finds each game's releases, the runtime's upload not yet read among them, and their published version", () => {
    const uploading = runtimeRelease(11, { status: 'uploading', data_set_id: null, sections: [] });
    const list = [uploading, release(10, { status: 'draft' }), release(9), runtimeRelease(7)];
    expect(releasesOf(JO_SET, list).map((r) => r.id)).toEqual([10, 9]);
    expect(releasesOf(RUNTIME_SET, list).map((r) => r.id)).toEqual([11, 7]);
    expect(publishedLabel(JO_SET, list)).toBe('1.7.5.9');
    expect(publishedLabel(RUNTIME_SET, list)).toBe('v0.0.21');
    expect(publishedLabel(DFX2_SET, list)).toBe('');
    expect(releasesWorking(list)).toBe(true);
    expect(releasesWorking([release(9)])).toBe(false);
  });

  it('lists the runtime from its releases when the games leave it out', () => {
    const list = [runtimeRelease(7), release(9)];
    const games = listGames([JO_SET], list);
    expect(games.map((g) => [g.id, g.display_name, g.kind, g.key])).toEqual([
      [1, 'Joint Operations', 'base', 'JO'],
      [9, 'OpenNova runtime', 'update', null],
    ]);
    expect(games[1].published_release?.id).toBe(7);
    expect(listGames(ALL, list)).toHaveLength(4);
  });

  it("checks the updater's login and command file together", () => {
    expect(updaterProblems('', '')).toEqual({});
    expect(updaterProblems('JOTRus', 'JOPTR.CMD')).toEqual({});
    expect(updaterProblems('', 'JOPTR.CMD').updater_login).toContain('needs the updater login');
    expect(updaterProblems('JO TR', 'JOPTR.EXE')).toEqual({
      updater_login: 'An updater login is 1 to 32 printable ASCII characters without spaces.',
      command_file: 'A command file is 1 to 15 characters ending in .CMD (JOPTR.CMD).',
    });
  });
});
