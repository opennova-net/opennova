import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import DataSetsSection from './DataSetsSection.vue';
import {
  completeTreeFile,
  createDataSet,
  createTree,
  deleteDataSet,
  deleteTree,
  fetchDataSets,
  finalizeTree,
  putTreePart,
  updateAllServers,
  updateDataSet,
} from '../../api/dataSets';
import { uploadFile } from '../../utils/partUpload';
import { dataSet, dataSetsResponse, JO_SET, tree } from '../../test/dataSetFixtures';

vi.mock('../../api/dataSets', () => ({
  completeTreeFile: vi.fn(),
  createDataSet: vi.fn(),
  createTree: vi.fn(),
  deleteDataSet: vi.fn(),
  deleteTree: vi.fn(),
  fetchDataSets: vi.fn(),
  finalizeTree: vi.fn(),
  putTreePart: vi.fn(),
  updateAllServers: vi.fn(),
  updateDataSet: vi.fn(),
}));

// The parts go through the site by uploadFile (its own test covers it); here
// it reports the whole file at once and stores one part.
vi.mock('../../utils/partUpload', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../utils/partUpload')>()),
  uploadFile: vi.fn(),
}));

const sets = vi.mocked(fetchDataSets);
const create = vi.mocked(createDataSet);
const update = vi.mocked(updateDataSet);
const removeSet = vi.mocked(deleteDataSet);
const openTree = vi.mocked(createTree);
const putPart = vi.mocked(putTreePart);
const completeFile = vi.mocked(completeTreeFile);
const finalize = vi.mocked(finalizeTree);
const removeTree = vi.mocked(deleteTree);
const updateAll = vi.mocked(updateAllServers);
const upload = vi.mocked(uploadFile);

const PART_SIZE = 32 * 1024 * 1024;

// A file as a folder pick (webkitdirectory) gives it.
function picked(path: string, size = 10): File {
  const name = path.split('/').pop() ?? path;
  const file = new File(['x'.repeat(size)], name);
  Object.defineProperty(file, 'webkitRelativePath', { value: path });
  return file;
}

// jsdom's file inputs take no files from a test: the list is set on the
// element before the change event.
async function pickFiles(page: VueWrapper, selector: string, files: File[]) {
  const input = page.get(selector);
  Object.defineProperty(input.element, 'files', { value: files, configurable: true });
  await input.trigger('change');
  await flushPromises();
}

function submitForm(page: VueWrapper, selector: string) {
  return page.get(selector).element.closest('form')!.dispatchEvent(new Event('submit'));
}

describe('DataSetsSection', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  let confirm: MockInstance<typeof window.confirm>;

  async function mountSection() {
    wrapper = mount(DataSetsSection);
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    confirm = vi.spyOn(window, 'confirm').mockReturnValue(true);
    sets.mockResolvedValue({ ok: true, value: dataSetsResponse() });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
  });

  it('lists each data set with its kind, data and servers', async () => {
    const page = await mountSection();
    expect(page.text()).toContain("through the site to the service's local store, in 32 MiB parts");
    const rows = page.findAll('tr.ds-row');
    expect(rows).toHaveLength(3);
    expect(rows[0].text()).toContain('Joint Operations');
    expect(rows[0].text()).toContain('Base game Joint Operations, lobby Joint Operations');
    expect(rows[0].text()).toContain('1.7.5.5');
    expect(rows[0].text()).toContain('3 live');
    expect(rows[0].text()).toContain('1 outdated');
    expect(rows[1].text()).toContain('Expansion jox01 of Joint Operations');
    expect(rows[2].text()).toContain('No data yet');
    // Only a data set with outdated servers offers their update.
    expect(rows[0].findAll('button').map((b) => b.text())).toContain('Update servers of Joint Operations');
    expect(rows[2].findAll('button').map((b) => b.text())).not.toContain('Update servers of Delta Force Xtreme 2');
    // No em dash in the page's copy.
    expect(page.text()).not.toContain('—');
  });

  it("expands a data set's trees and deletes one, showing why one stays", async () => {
    removeTree.mockResolvedValueOnce({ ok: true, value: undefined });
    const page = await mountSection();
    expect(page.findAll('tr.ds-tree-row')).toHaveLength(0);
    await page.findAll('tr.ds-row')[0].findAll('button').find((b) => b.text().startsWith('Trees (2)'))!.trigger('click');
    const trees = page.findAll('tr.ds-tree-row');
    expect(trees).toHaveLength(2);
    expect(trees[0].text()).toContain('current');
    expect(trees[0].text()).toContain('Ready');
    expect(trees[0].text()).toContain('1.3 GB');
    expect(trees[0].text()).toContain('54');
    expect(trees[0].text()).toContain('2');
    expect(trees[0].text()).toContain('by root');
    expect(trees[1].text()).not.toContain('current');
    expect(trees[1].text()).toContain('Failed');
    expect(trees[1].text()).toContain('version.txt is missing');

    await trees[1].findAll('button').find((b) => b.text().startsWith('Delete'))!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining('Delete tree 11 of Joint Operations?'));
    expect(removeTree).toHaveBeenCalledWith(1, 11);
    expect(page.get('[role="status"]').text()).toBe('Tree 11 is being pruned.');

    removeTree.mockResolvedValueOnce({
      ok: false,
      error: { code: 'pinned', status: 409, message: '2 live servers run on that tree, so it stays until they update or stop.' },
    });
    await page.findAll('tr.ds-tree-row')[0].findAll('button').find((b) => b.text().startsWith('Delete'))!.trigger('click');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('2 live servers run on that tree, so it stays until they update or stop.');
  });

  it('checks the new data set form before sending it', async () => {
    create.mockResolvedValue({ ok: true, value: dataSet(5, { display_name: 'Escalation 2' }) });
    const page = await mountSection();
    await page.get('#ds-new-toggle').trigger('click');
    expect(page.find('#ds-new-form').exists()).toBe(true);
    // A base, by default, with the first title and lobby picked.
    expect((page.get('#ds-new-title').element as HTMLSelectElement).value).toBe('jo');
    expect((page.get('#ds-new-game').element as HTMLSelectElement).value).toBe('jop_2_consumer');

    // Nothing named: refused in the page.
    submitForm(page, '#ds-new-display_name');
    await flushPromises();
    expect(create).not.toHaveBeenCalled();
    expect(page.get('#ds-new-display_name-error').text()).toBe('Enter a display name.');

    // An expansion: its base among the bases, its code's shape.
    await page.get('#ds-new-kind').setValue('expansion');
    expect(page.findAll('#ds-new-parent_id option').map((o) => o.text())).toEqual(['Joint Operations', 'Delta Force Xtreme 2']);
    await page.get('#ds-new-display_name').setValue(' Escalation 2 ');
    await page.get('#ds-new-expansion_code').setValue('JOX-02');
    submitForm(page, '#ds-new-display_name');
    await flushPromises();
    expect(create).not.toHaveBeenCalled();
    expect(page.get('#ds-new-expansion_code-error').text()).toContain('1 to 8 lowercase letters and digits');

    await page.get('#ds-new-expansion_code').setValue('jox02');
    submitForm(page, '#ds-new-display_name');
    await flushPromises();
    expect(create).toHaveBeenCalledWith({ display_name: 'Escalation 2', kind: 'expansion', parent_id: 1, expansion_code: 'jox02' });
    expect(page.find('#ds-new-form').exists()).toBe(false);
    expect(page.get('[role="status"]').text()).toBe('Escalation 2 is created. Upload its data next.');
    expect(sets).toHaveBeenCalledTimes(2);

    // A base sends its title and lobby.
    await page.get('#ds-new-toggle').trigger('click');
    await page.get('#ds-new-display_name').setValue('OpenNova');
    await page.get('#ds-new-game').setValue('opennova_0_consumer');
    submitForm(page, '#ds-new-display_name');
    await flushPromises();
    expect(create).toHaveBeenLastCalledWith({ display_name: 'OpenNova', kind: 'base', title: 'jo', game: 'opennova_0_consumer' });
  });

  it("edits a data set's name and lobby", async () => {
    update.mockResolvedValue({ ok: true, value: dataSet(1, { display_name: 'JO' }) });
    const page = await mountSection();
    await page.findAll('tr.ds-row')[0].findAll('button').find((b) => b.text().startsWith('Edit'))!.trigger('click');
    await page.get('#ds-edit-display_name-1').setValue('JO');
    await page.get('#ds-edit-game-1').setValue('opennova_0_consumer');
    submitForm(page, '#ds-edit-display_name-1');
    await flushPromises();
    expect(update).toHaveBeenCalledWith(1, { display_name: 'JO', game: 'opennova_0_consumer' });
    expect(page.get('[role="status"]').text()).toBe('JO is saved.');

    // An expansion has no lobby of its own.
    update.mockResolvedValue({ ok: true, value: dataSet(2, { display_name: 'Escalation (JO)' }) });
    await page.findAll('tr.ds-row')[1].findAll('button').find((b) => b.text().startsWith('Edit'))!.trigger('click');
    expect(page.find('#ds-edit-game-2').exists()).toBe(false);
    await page.get('#ds-edit-display_name-2').setValue('Escalation (JO)');
    submitForm(page, '#ds-edit-display_name-2');
    await flushPromises();
    expect(update).toHaveBeenLastCalledWith(2, { display_name: 'Escalation (JO)' });
  });

  it("keeps only the kind's files out of a picked folder, then uploads, completes and finalizes them", async () => {
    const page = await mountSection();
    await page.findAll('tr.ds-row')[0].findAll('button').find((b) => b.text().startsWith('Upload data'))!.trigger('click');
    expect(page.get('#ds-uploader').text()).toContain('Joint Operations');
    expect(page.get('#ds-uploader').text()).toContain('resource.pff, localres.pff and language.pff (required)');

    // The install's root files, one in its own case, one that is no tree file,
    // and the expansion's files deeper down.
    await pickFiles(page, '#ds-upload-folder', [
      picked('JointOps/resource.pff', 30),
      picked('JointOps/LOCALRES.PFF', 20),
      picked('JointOps/Jointops.exe'),
      picked('JointOps/expansion/jox01/jox01.pff'),
    ]);
    const kept = page.findAll('li.ds-upload-kept').map((li) => li.text());
    expect(kept[0]).toContain('resource.pff');
    expect(kept[1]).toContain('localres.pff');
    expect(kept).toHaveLength(2);
    expect(page.findAll('li.ds-upload-missing').map((li) => li.text())).toEqual(['language.pffmissing, required']);
    expect(page.get('#ds-upload-checklist').text()).toContain('2 other files left out');
    expect((page.get('#ds-upload-start').element as HTMLButtonElement).disabled).toBe(true);

    // With the required file: the upload opens the tree, sends each file and finalizes.
    await pickFiles(page, '#ds-upload-folder', [picked('JointOps/resource.pff', 30), picked('JointOps/localres.pff', 20), picked('JointOps/language.pff', 10)]);
    expect(page.findAll('li.ds-upload-missing')).toHaveLength(0);
    expect((page.get('#ds-upload-start').element as HTMLButtonElement).disabled).toBe(false);

    openTree.mockResolvedValue({
      ok: true,
      value: {
        tree: { id: 13 },
        uploads: ['resource.pff', 'localres.pff', 'language.pff'].map((name) => ({ name, key: `trees/13/${name}`, upload_id: `u-${name}`, part_size: PART_SIZE })),
      },
    });
    putPart.mockResolvedValue({ ok: true, value: { etag: '"e1"' } });
    upload.mockImplementation(async (file, options) => {
      options.onProgress?.(file.size);
      // The page's part goes through the API with the file's upload, the
      // part's bytes and the upload's signal.
      const stored = await options.putPart(1, file.slice(0, file.size), options.signal);
      expect(stored).toEqual({ etag: '"e1"' });
      return [{ part_number: 1, etag: '"e1"' }];
    });
    completeFile.mockResolvedValue({ ok: true, value: undefined });
    finalize.mockResolvedValue({ ok: true, value: tree(13, { state: 'finalizing' }) });
    // The read after finalize finds the tree still finalizing, the next one ready.
    const finalizing = dataSetsResponse({ data_sets: [{ ...JO_SET, trees: [...JO_SET.trees, tree(13, { state: 'finalizing', version: '', ready_at: null })] }] });
    const ready = dataSetsResponse({ data_sets: [{ ...JO_SET, trees: [...JO_SET.trees, tree(13, { version: '1.7.5.9', missions: 60 })] }] });
    sets.mockResolvedValueOnce({ ok: true, value: finalizing });
    sets.mockResolvedValue({ ok: true, value: ready });

    await page.get('#ds-upload-start').trigger('click');
    await flushPromises();
    expect(openTree).toHaveBeenCalledWith(1, [
      { name: 'resource.pff', size: 30 },
      { name: 'localres.pff', size: 20 },
      { name: 'language.pff', size: 10 },
    ]);
    expect(upload).toHaveBeenCalledTimes(3);
    expect(upload.mock.calls[0][1]).toMatchObject({ partSize: PART_SIZE });
    expect(putPart).toHaveBeenCalledTimes(3);
    expect(putPart).toHaveBeenCalledWith(1, 13, 'resource.pff', 'u-resource.pff', 1, expect.any(Blob), expect.any(AbortSignal));
    expect(putPart.mock.calls[0][5].size).toBe(30);
    expect(completeFile).toHaveBeenCalledWith(1, 13, 'localres.pff', { upload_id: 'u-localres.pff', parts: [{ part_number: 1, etag: '"e1"' }] });
    expect(finalize).toHaveBeenCalledWith(1, 13);
    expect(page.findAll('li.ds-upload-progress')).toHaveLength(3);
    expect(page.get('#ds-upload-total').text()).toContain('The worker checks the files');

    // The next poll finds it ready.
    await page.findAll('button').find((b) => b.text() === 'Refresh')!.trigger('click');
    await flushPromises();
    expect(page.get('#ds-upload-total').text()).toContain('version 1.7.5.9, 60 missions');
    expect(page.get('[role="status"]').text()).toBe('Joint Operations 1.7.5.9 is ready: new servers start on it.');
  });

  it("ends the upload with the service's refusal of a part, and removes the half-uploaded tree", async () => {
    const page = await mountSection();
    await page.findAll('tr.ds-row')[0].findAll('button').find((b) => b.text().startsWith('Upload data'))!.trigger('click');
    await pickFiles(page, '#ds-upload-files', [new File(['a'], 'resource.pff'), new File(['b'], 'localres.pff'), new File(['c'], 'language.pff')]);
    openTree.mockResolvedValue({
      ok: true,
      value: { tree: { id: 14 }, uploads: ['resource.pff', 'localres.pff', 'language.pff'].map((name) => ({ name, key: '', upload_id: `u-${name}`, part_size: PART_SIZE })) },
    });
    // The refusal's message is what the page's putPart throws, and what
    // uploadFile rejects with once its retries are spent.
    putPart.mockResolvedValue({ ok: false, error: { code: 'store_error', status: 502, message: 'The object store refused the request.' } });
    upload.mockImplementation(async (file, options) => {
      await options.putPart(1, file, options.signal);
      return [];
    });
    removeTree.mockResolvedValue({ ok: true, value: undefined });
    await page.get('#ds-upload-start').trigger('click');
    await flushPromises();
    expect(removeTree).toHaveBeenCalledWith(1, 14);
    expect(page.get('#ds-upload-total').text()).toContain('The upload failed: The object store refused the request. The half-uploaded tree was removed.');
    expect(completeFile).not.toHaveBeenCalled();
    expect(finalize).not.toHaveBeenCalled();
  });

  it("asks before updating a data set's servers or deleting it, naming what happens", async () => {
    updateAll.mockResolvedValue({ ok: true, value: 2 });
    removeSet.mockResolvedValue({ ok: false, error: { code: 'in_use', status: 409, message: 'That data set is in use.' } });
    const page = await mountSection();
    const row = page.findAll('tr.ds-row')[0];
    await row.findAll('button').find((b) => b.text().startsWith('Update servers'))!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining('Update the 1 outdated server of Joint Operations?'));
    expect(updateAll).toHaveBeenCalledWith(1);
    expect(page.get('[role="status"]').text()).toBe('2 servers are updating.');

    confirm.mockReturnValueOnce(false);
    await row.findAll('button').find((b) => b.text().startsWith('Delete'))!.trigger('click');
    expect(removeSet).not.toHaveBeenCalled();
    await row.findAll('button').find((b) => b.text().startsWith('Delete'))!.trigger('click');
    await flushPromises();
    expect(removeSet).toHaveBeenCalledWith(1);
    expect(page.get('[role="alert"]').text()).toBe('That data set is in use.');
  });
});
