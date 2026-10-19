import { describe, expect, it } from 'vitest';
import { replacedMachines, replacedServers } from './history';

const server = (id: number, name: string, live: boolean, owner: number | null = null) => ({
  id,
  live,
  owner,
  settings: { game_name: name },
});

describe('replacedServers', () => {
  it('hides an ended server once its owner started one of the same name after it', () => {
    const servers = [
      server(10, 'Retail Joint Ops', true),
      server(9, 'Staging JO base test', false),
      server(4, 'Retail Joint Ops', false),
      server(1, 'Retail Joint Ops', false),
    ];
    expect([...replacedServers(servers)].sort()).toEqual([1, 4]);
  });

  it('keeps a server whose name no later start took, and never hides a live one', () => {
    const servers = [server(3, 'Alpha', false), server(2, 'Beta', true), server(1, 'Beta', true)];
    expect(replacedServers(servers).size).toBe(0);
  });

  it("never lets another account's server of the same name replace one", () => {
    const servers = [server(7, 'Test', true, 2), server(6, 'Test', false, 1)];
    expect(replacedServers(servers, (s) => s.owner).size).toBe(0);
    expect([...replacedServers(servers)]).toEqual([6]);
  });
});

describe('replacedMachines', () => {
  const machine = (id: string, state: string, kind = 'shared') => ({ id, state, kind });

  it('hides the ended and failed machines a later working launch of their kind replaced', () => {
    const machines = [machine('89', 'ready'), machine('88', 'ended'), machine('86', 'failed'), machine('79', 'failed')];
    expect([...replacedMachines(machines)].sort()).toEqual(['79', '86', '88']);
  });

  it('keeps failures no launch has worked after, and a dedicated machine for its own kind', () => {
    const machines = [machine('91', 'failed'), machine('90', 'failed'), machine('89', 'ready'), machine('88', 'ended', 'own')];
    expect(replacedMachines(machines).size).toBe(0);
  });
});
