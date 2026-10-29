import { mount, flushPromises } from '@vue/test-utils';
import { beforeEach, expect, it, vi } from 'vitest';
import { defineComponent, h } from 'vue';
import ServerAdminPanel from './ServerAdminPanel.vue';
import { provideConsoleQueue } from '../composables/useConsoleQueue';
import { editConfiguration, readConfiguration, sendAdminCommand } from '../api/serverConfiguration';
import { confirmAction } from '../composables/useConfirm';
import type { ConfigurationState } from '../api/serverConfiguration';
vi.mock('../api/serverConfiguration', async (original) => ({
  ...await original<typeof import('../api/serverConfiguration')>(),
  readConfiguration: vi.fn(), editConfiguration: vi.fn(), sendAdminCommand: vi.fn(),
}));
vi.mock('../composables/useConfirm', () => ({ confirmAction: vi.fn() }));
const state: ConfigurationState = {
  saved: { product_id: 1, expansion_id: null, game_name: 'Host', game_password: '', max_players: 32,
    time_limit: 30, kill_limit: 50, max_score: 5, respawn: 5, start_delay: 0, team_ff: false,
    list_on_novalogic: false, rotation: [], options: { server_message: 'Welcome' } },
  settings_revision: 7, settings: [{ key: 'ServerPassword', value: 'secret' }], weapons: [],
  schema: [{ key: 'server_message', label: 'Server message', group: 'Server', kind: 'text', default: '',
    min: 0, max: 127, command: '', core: false, note: '' }],
};
function page() {
  return mount(defineComponent({ setup() {
    provideConsoleQueue();
    return () => h(ServerAdminPanel, { target: { kind: 'owner', id: 1 } });
  } }));
}
beforeEach(() => {
  vi.clearAllMocks();
  vi.mocked(readConfiguration).mockResolvedValue(state);
  const result = { outcome: 'answered', sent: true, delivery_unknown: false, configuration_saved: true, settings_revision: 8, replies: ['OK'] };
  vi.mocked(editConfiguration).mockResolvedValue(result);
  vi.mocked(sendAdminCommand).mockResolvedValue(result);
  vi.mocked(confirmAction).mockResolvedValue(true);
});
it('sends only edited settings with the revision and masks live passwords', async () => {
  const wrapper = page();
  const details = wrapper.get('details');
  details.element.open = true; await details.trigger('toggle'); await flushPromises();
  expect(wrapper.text()).not.toContain('secret');
  await wrapper.get('#live-settings-1-server_message').setValue('New message');
  await wrapper.findAll('button').find((b) => b.text() === 'Apply and save changes')!.trigger('click');
  await flushPromises();
  expect(editConfiguration).toHaveBeenCalledWith({kind: 'owner', id: 1}, 7, {options: {server_message: 'New message'}});
  expect(wrapper.text()).toContain('Accepted changes saved');
  wrapper.unmount();
});
it('asks before bulk destructive commands and displays every reply', async () => {
  const wrapper = page();
  vi.mocked(sendAdminCommand).mockResolvedValueOnce({ outcome: 'answered', sent: true, delivery_unknown: false,
    configuration_saved: false, settings_revision: 0, replies: ['OK - one', 'ERROR - two'] });
  await wrapper.findAll('button').find((b) => b.text() === 'Apply to all')!.trigger('click');
  await flushPromises();
  expect(confirmAction).toHaveBeenCalled();
  expect(sendAdminCommand).toHaveBeenCalledWith({kind: 'owner', id: 1}, 'PLAYER PUNT ALL');
  expect(wrapper.get('pre').text()).toContain('OK - one\nERROR - two');
  wrapper.unmount();
});
