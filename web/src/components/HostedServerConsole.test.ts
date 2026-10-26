import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { defineComponent, h } from 'vue';
import HostedServerConsole from './HostedServerConsole.vue';
import {
  consolePlayerAction,
  fetchConsoleChat,
  fetchConsoleLog,
  fetchConsoleMissions,
  fetchConsolePlayers,
  fetchConsoleRotation,
} from '../api/hostedServer';
import { provideConsoleQueue } from '../composables/useConsoleQueue';
import { confirmAction } from '../composables/useConfirm';
import { CONSOLE_TICK_MS } from '../composables/useHostedConsole';
import type { ConsolePlayer } from '../types/hostedServer';

vi.mock('../api/hostedServer', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api/hostedServer')>()),
  fetchConsolePlayers: vi.fn(),
  fetchConsoleRotation: vi.fn(),
  fetchConsoleMissions: vi.fn(),
  fetchConsoleChat: vi.fn(),
  fetchConsoleLog: vi.fn(),
  consolePlayerAction: vi.fn(),
}));
vi.mock('../composables/useConfirm', () => ({ confirmAction: vi.fn() }));

const players = vi.mocked(fetchConsolePlayers);

function player(slot: number, name: string): ConsolePlayer {
  return { slot, name, team: 1, class: 0, kills: 0, deaths: 0, ping: 60 };
}

// The console inside a page that provides the console queue, as the server page does.
function mountConsole(novalogicSlots?: number[]) {
  const Page = defineComponent({
    setup() {
      provideConsoleQueue();
      return () =>
        h(HostedServerConsole, {
          target: { kind: 'owner', id: 1 },
          mode: 'owner',
          serverName: 'Server 1',
          ...(novalogicSlots ? { novalogicSlots } : {}),
        });
    },
  });
  return mount(Page);
}

describe('HostedServerConsole', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  beforeEach(() => {
    vi.clearAllMocks();
    players.mockResolvedValue({ ok: true, value: [player(1, 'Ours'), player(3, 'Tudor.M')] });
    vi.mocked(fetchConsoleRotation).mockResolvedValue({ ok: true, value: [] });
    vi.mocked(fetchConsoleMissions).mockResolvedValue({ ok: true, value: [] });
    vi.mocked(fetchConsoleChat).mockResolvedValue({ ok: true, value: [] });
    vi.mocked(fetchConsoleLog).mockResolvedValue({ ok: true, value: { server_id: 1, link: null, lines: [] } });
    vi.mocked(confirmAction).mockResolvedValue(true);
    vi.mocked(consolePlayerAction).mockResolvedValue({ ok: true, value: { slot: 1, result: 'sent' } });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    vi.useRealTimers();
  });

  async function rows() {
    for (let i = 0; i < 5; i++) await flushPromises();
    return wrapper!.findAll('tbody tr').filter((r) => r.find('th[scope="row"]').exists());
  }

  it("marks the players who came through NovaLogic's NovaWorld, by their slot", async () => {
    wrapper = mountConsole([3]);
    const [ours, theirs] = await rows();
    expect(ours.get('th').text()).toBe('Ours');
    expect(ours.find('.player-novalogic').exists()).toBe(false);
    expect(theirs.get('.player-novalogic').text()).toBe("NovaLogic: came through NovaLogic's NovaWorld");
    expect(theirs.get('.player-novalogic').attributes('title')).toBe("Came through NovaLogic's NovaWorld");
  });

  it('marks no one without the slots (a server not listed there)', async () => {
    wrapper = mountConsole();
    const listed = await rows();
    expect(listed).toHaveLength(2);
    expect(wrapper.find('.player-novalogic').exists()).toBe(false);
  });

  it.each(['Ban', 'Kill'])('does not %s a slot whose player changed while confirmation was open', async (label) => {
    vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval'] });
    let approve!: (accepted: boolean) => void;
    vi.mocked(confirmAction).mockImplementationOnce(() => new Promise((resolve) => { approve = resolve; }));
    wrapper = mountConsole();
    const [first] = await rows();
    await first.findAll('button').find((button) => button.text().startsWith(label))!.trigger('click');
    expect(consolePlayerAction).not.toHaveBeenCalled();

    players.mockResolvedValue({ ok: true, value: [player(1, 'New arrival')] });
    await vi.advanceTimersByTimeAsync(CONSOLE_TICK_MS);
    await rows();
    expect(wrapper.text()).toContain('New arrival');
    approve(true);
    await flushPromises();

    expect(consolePlayerAction).not.toHaveBeenCalled();
    expect(wrapper.get('[role="alert"]').text()).toContain('The player list changed');
    expect(wrapper.findAll('p').some((paragraph) => paragraph.attributes('aria-hidden') === 'true' && paragraph.text().includes('The player list changed'))).toBe(true);
  });

  it('allows a confirmed player action when a refresh only changes their scores', async () => {
    vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval'] });
    let approve!: (accepted: boolean) => void;
    vi.mocked(confirmAction).mockImplementationOnce(() => new Promise((resolve) => { approve = resolve; }));
    wrapper = mountConsole();
    const [first] = await rows();
    await first.findAll('button').find((button) => button.text().startsWith('Ban'))!.trigger('click');
    players.mockResolvedValue({ ok: true, value: [{ ...player(1, 'Ours'), kills: 5 }] });
    await vi.advanceTimersByTimeAsync(CONSOLE_TICK_MS);
    await rows();
    approve(true);
    await flushPromises();

    expect(consolePlayerAction).toHaveBeenCalledWith({ kind: 'owner', id: 1 }, 1, 'ban');
  });
});
