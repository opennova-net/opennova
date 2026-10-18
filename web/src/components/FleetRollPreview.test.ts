import { flushPromises, mount } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import FleetRollPreview from './FleetRollPreview.vue';
import { fetchRollPlan, rollFleet } from '../api/admin';
import { rollPlan } from '../test/adminFixtures';
import { testRouter } from '../test/router';

vi.mock('../api/admin', () => ({ fetchRollPlan: vi.fn(), rollFleet: vi.fn() }));

const plan = vi.mocked(fetchRollPlan);
const roll = vi.mocked(rollFleet);

describe('FleetRollPreview', () => {
  async function mountPreview() {
    const router = await testRouter('/admin/fleet?tab=roll');
    const wrapper = mount(FleetRollPreview, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    plan.mockResolvedValue({ ok: true, value: rollPlan() });
  });

  it('shows replace or keep per machine, the preview picked to start with', async () => {
    const page = await mountPreview();
    expect(page.get('.roll-image').text()).toBe('The launch templates pin ami-0bbb.');
    const rows = page.findAll('tr.roll-row');
    expect(rows).toHaveLength(2);
    expect(rows[0].text()).toContain('Replace');
    expect(rows[0].text()).toContain("its image is not the templates' (ami-0bbb)");
    expect(rows[1].text()).toContain('Keep');
    expect((page.get('#roll-pick-3').element as HTMLInputElement).checked).toBe(true);
    expect((page.get('#roll-pick-4').element as HTMLInputElement).checked).toBe(false);
    expect(page.text()).toContain('1 machine picked: 2 servers move, each down while it moves.');
  });

  it('rolls the machines picked behind the typed phrase', async () => {
    roll.mockResolvedValue({ ok: true, value: 2 });
    const page = await mountPreview();
    await page.get('#roll-pick-4').setValue(true);
    const submit = page.get('form button[type="submit"]');
    expect(submit.text()).toBe('Roll 2 machines');
    await page.get('#roll-confirm').setValue('ROL');
    expect((submit.element as HTMLButtonElement).disabled).toBe(true);
    await page.get('#roll-confirm').setValue('ROLL');
    await page.get('form').trigger('submit');
    await flushPromises();
    expect(roll).toHaveBeenCalledWith(['3', '4']);
    expect(page.get('[role="status"]').text()).toBe('2 machines are draining: their servers move to fresh machines one at a time.');
    expect(page.emitted('rolled')).toHaveLength(1);
    // The preview is read again.
    expect(plan).toHaveBeenCalledTimes(2);
  });

  it("says there is nothing to roll, and a refused roll's reason", async () => {
    plan.mockResolvedValue({ ok: true, value: rollPlan({ image_id: null, machines: [], replace: [] }) });
    const empty = await mountPreview();
    expect(empty.text()).toContain('No machine takes servers, so there is nothing to roll.');
    expect(empty.get('.roll-image').text()).toContain('No image is pinned here');
    expect(empty.find('form').exists()).toBe(false);

    plan.mockResolvedValue({ ok: true, value: rollPlan() });
    roll.mockResolvedValue({ ok: false, error: { code: 'invalid_field', status: 400, message: 'The server refused one of the values sent. Check them and try again.' } });
    const page = await mountPreview();
    await page.get('#roll-confirm').setValue('ROLL');
    await page.get('form').trigger('submit');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toContain('The server refused one of the values sent.');
  });
});
