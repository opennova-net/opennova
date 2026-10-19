import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import DataSetsSection from './DataSetsSection.vue';
import {
  completeRelease,
  completeTreeFile,
  createDataSet,
  createRelease,
  createTree,
  deleteDataSet,
  deleteRelease,
  deleteTree,
  fetchDataSets,
  fetchReleases,
  finalizeTree,
  publishRelease,
  putReleasePart,
  putTreePart,
  retireRelease,
  updateAllServers,
  updateDataSet,
} from '../../api/dataSets';
import { uploadFile } from '../../utils/partUpload';
import { dataSet, dataSetsResponse, JO_SET, release, tree } from '../../test/dataSetFixtures';

vi.mock('../../api/dataSets', () => ({
  completeRelease: vi.fn(),
  completeTreeFile: vi.fn(),
  createDataSet: vi.fn(),
  createRelease: vi.fn(),
  createTree: vi.fn(),
  deleteDataSet: vi.fn(),
  deleteRelease: vi.fn(),
  deleteTree: vi.fn(),
  fetchDataSets: vi.fn(),
  fetchReleases: vi.fn(),
  finalizeTree: vi.fn(),
  publishRelease: vi.fn(),
  putReleasePart: vi.fn(),
  putTreePart: vi.fn(),
  retireRelease: vi.fn(),
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
const list = vi.mocked(fetchReleases);
const create = vi.mocked(createDataSet);
const update = vi.mocked(updateDataSet);
const removeSet = vi.mocked(deleteDataSet);
const openTree = vi.mocked(createTree);
const putPart = vi.mocked(putTreePart);
const completeFile = vi.mocked(completeTreeFile);
const finalize = vi.mocked(finalizeTree);
const removeTree = vi.mocked(deleteTree);
const updateAll = vi.mocked(updateAllServers);
const openRelease = vi.mocked(createRelease);
const putReleasePartMock = vi.mocked(putReleasePart);
const completeUpload = vi.mocked(completeRelease);
const publish = vi.mocked(publishRelease);
const retire = vi.mocked(retireRelease);
const removeRelease = vi.mocked(deleteRelease);
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
    list.mockResolvedValue({ ok: true, value: [release(1), release(2, { status: 'draft' }), release(3, { status: 'retired' })] });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
  });

  it('lists each data set with its kind, updater, data, release and servers', async () => {
    const page = await mountSection();
    expect(page.text()).toContain("Uploads go to the service's local store in 32 MiB parts");
    const rows = page.findAll('tr.ds-row');
    expect(rows).toHaveLength(3);
    expect(rows[0].text()).toContain('Joint Operations');
    expect(rows[0].text()).toContain('Base game Joint Operations, lobby Joint Operations');
    expect(rows[0].text()).toContain('JOTRus');
    expect(rows[0].text()).toContain('JOPTR.CMD');
    expect(rows[0].text()).toContain('retail updater');
    expect(rows[0].text()).toContain('1.7.5.5');
    expect(rows[0].text()).toContain('JO 1.7.5.5');
    expect(rows[0].text()).toContain('ab12cd34ef56');
    expect(rows[0].text()).toContain('3 live');
    expect(rows[0].text()).toContain('1 outdated');
    expect(rows[1].text()).toContain('Expansion jox01 of Joint Operations');
    expect(rows[1].text()).toContain('JOEus');
    expect(rows[2].text()).toContain('No data yet');
    expect(rows[2].text()).toContain('Not served');
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
    expect(trees[0].text()).toContain('baseline');
    expect(trees[0].text()).toContain('uploaded');
    expect(trees[0].text()).toContain('Ready');
    expect(trees[0].text()).toContain('1.3 GB');
    expect(trees[0].text()).toContain('54');
    expect(trees[0].text()).toContain('2');
    expect(trees[0].text()).toContain('by root');
    expect(trees[1].text()).not.toContain('current');
    expect(trees[1].text()).not.toContain('baseline');
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

    // An expansion: its base among the bases, its code's shape, the updater pair.
    await page.get('#ds-new-kind').setValue('expansion');
    expect(page.findAll('#ds-new-parent_id option').map((o) => o.text())).toEqual(['Joint Operations', 'Delta Force Xtreme 2']);
    await page.get('#ds-new-display_name').setValue(' Escalation 2 ');
    await page.get('#ds-new-expansion_code').setValue('JOX-02');
    await page.get('#ds-new-updater_login').setValue('JOEus2');
    submitForm(page, '#ds-new-display_name');
    await flushPromises();
    expect(create).not.toHaveBeenCalled();
    expect(page.get('#ds-new-expansion_code-error').text()).toContain('1 to 8 lowercase letters and digits');
    expect(page.get('#ds-new-command_file-error').text()).toBe('An updater login needs its command file.');

    await page.get('#ds-new-expansion_code').setValue('jox02');
    await page.get('#ds-new-command_file').setValue('joeptr2.cmd');
    submitForm(page, '#ds-new-display_name');
    await flushPromises();
    expect(create).toHaveBeenCalledWith({
      display_name: 'Escalation 2',
      kind: 'expansion',
      retail_updater: false,
      updater_login: 'JOEus2',
      command_file: 'JOEPTR2.CMD',
      parent_id: 1,
      expansion_code: 'jox02',
    });
    expect(page.find('#ds-new-form').exists()).toBe(false);
    expect(page.get('[role="status"]').text()).toBe('Escalation 2 is created. Upload its data next.');
    expect(sets).toHaveBeenCalledTimes(2);

    // A base sends its title and lobby, and no updater when none is named.
    await page.get('#ds-new-toggle').trigger('click');
    await page.get('#ds-new-display_name').setValue('OpenNova');
    await page.get('#ds-new-game').setValue('opennova_0_consumer');
    submitForm(page, '#ds-new-display_name');
    await flushPromises();
    expect(create).toHaveBeenLastCalledWith({ display_name: 'OpenNova', kind: 'base', retail_updater: false, title: 'jo', game: 'opennova_0_consumer' });
  });

  it("edits a data set's name, lobby and updater", async () => {
    update.mockResolvedValue({ ok: true, value: dataSet(1, { display_name: 'JO' }) });
    const page = await mountSection();
    await page.findAll('tr.ds-row')[0].findAll('button').find((b) => b.text().startsWith('Edit'))!.trigger('click');
    await page.get('#ds-edit-display_name-1').setValue('JO');
    await page.get('#ds-edit-game-1').setValue('opennova_0_consumer');
    await page.get('#ds-edit-retail_updater-1').setValue(false);
    submitForm(page, '#ds-edit-display_name-1');
    await flushPromises();
    expect(update).toHaveBeenCalledWith(1, { display_name: 'JO', game: 'opennova_0_consumer', updater_login: 'JOTRus', command_file: 'JOPTR.CMD', retail_updater: false });
    expect(page.get('[role="status"]').text()).toBe('JO is saved.');

    // An expansion has no lobby of its own; a command file without its login
    // is refused in the page.
    update.mockResolvedValue({ ok: true, value: dataSet(2, { display_name: 'Escalation (JO)' }) });
    await page.findAll('tr.ds-row')[1].findAll('button').find((b) => b.text().startsWith('Edit'))!.trigger('click');
    expect(page.find('#ds-edit-game-2').exists()).toBe(false);
    await page.get('#ds-edit-display_name-2').setValue('Escalation (JO)');
    await page.get('#ds-edit-updater_login-2').setValue('');
    submitForm(page, '#ds-edit-display_name-2');
    await flushPromises();
    expect(update).toHaveBeenCalledTimes(1);
    expect(page.get('tr.ds-edit-row').text()).toContain('A command file needs the updater login that goes with it.');
    await page.get('#ds-edit-command_file-2').setValue('');
    submitForm(page, '#ds-edit-display_name-2');
    await flushPromises();
    expect(update).toHaveBeenLastCalledWith(2, { display_name: 'Escalation (JO)', updater_login: '', command_file: '', retail_updater: false });
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
    expect(page.get('#ds-upload-total').text()).toContain('The data program checks the files');

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

  it('lists the releases with their sections, and publishes, retires and deletes after a confirmation', async () => {
    publish.mockResolvedValue({ ok: true, value: release(2, { status: 'published' }) });
    retire.mockResolvedValue({ ok: true, value: release(1, { status: 'retired' }) });
    removeRelease.mockResolvedValue({ ok: true, value: undefined });
    const page = await mountSection();
    const rows = page.findAll('tr.ds-release-row');
    expect(rows).toHaveLength(3);
    expect(rows[0].text()).toContain('JO 1.7.5.1');
    expect(rows[0].text()).toContain('Published');
    expect(rows[0].text()).toContain('312 MB');
    expect(rows[0].text()).toContain('ab12cd34ef56');
    const sections = rows[0].findAll('.ds-release-section').map((s) => s.text());
    expect(sections[0]).toContain('Joint Operations');
    expect(sections[0]).toContain('tree 10 ready');
    expect(sections[1]).toContain('Escalation');
    expect(sections[1]).toContain('(jox01)');
    expect(rows[1].text()).toContain('Draft');
    expect(rows[2].text()).toContain('Retired');

    await rows[1].findAll('button').find((b) => b.text().startsWith('Publish'))!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining("builds each data set's new tree; servers update when their owners choose"));
    expect(publish).toHaveBeenCalledWith(2);
    expect(page.get('[role="status"]').text()).toContain('JO 1.7.5.2 is published');

    await rows[0].findAll('button').find((b) => b.text().startsWith('Retire'))!.trigger('click');
    await flushPromises();
    expect(retire).toHaveBeenCalledWith(1);
    await rows[2].findAll('button').find((b) => b.text().startsWith('Delete'))!.trigger('click');
    await flushPromises();
    expect(removeRelease).toHaveBeenCalledWith(3);
    // A published release offers no delete, a retired one no publish.
    expect(rows[0].findAll('button').map((b) => b.text())).not.toContain('Delete JO 1.7.5.1');
    expect(rows[2].findAll('button').map((b) => b.text())).not.toContain('Publish JO 1.7.5.3');
  });

  it('uploads a release package in parts through the site and completes it', async () => {
    openRelease.mockResolvedValue({ ok: true, value: { release: { id: 7 }, upload: { key: 'updates/upload-7.sfs', upload_id: 'r7', part_size: PART_SIZE } } });
    putReleasePartMock.mockResolvedValue({ ok: true, value: { etag: '"e1"' } });
    upload.mockImplementation(async (file, options) => {
      options.onProgress?.(file.size);
      // The page's part goes through the API with the release's upload, the
      // part's bytes and the upload's signal.
      const stored = await options.putPart(1, file.slice(0, file.size), options.signal);
      expect(stored).toEqual({ etag: '"e1"' });
      return [{ part_number: 1, etag: '"e1"' }];
    });
    completeUpload.mockResolvedValue({ ok: true, value: release(7, { status: 'validating' }) });
    const page = await mountSection();
    expect((page.get('#ds-release-upload').element as HTMLButtonElement).disabled).toBe(true);
    await pickFiles(page, '#ds-release-file', [new File(['p'.repeat(100)], 'jo_1759.sfs')]);
    await page.get('#ds-release-name').setValue('JO 1.7.5.9');
    expect((page.get('#ds-release-upload').element as HTMLButtonElement).disabled).toBe(false);
    submitForm(page, '#ds-release-name');
    await flushPromises();
    expect(openRelease).toHaveBeenCalledWith({ name: 'JO 1.7.5.9', size: 100 });
    expect(upload.mock.calls[0][1]).toMatchObject({ partSize: PART_SIZE });
    expect(putReleasePartMock).toHaveBeenCalledWith(7, 'r7', 1, expect.any(Blob), expect.any(AbortSignal));
    expect(putReleasePartMock.mock.calls[0][3].size).toBe(100);
    expect(completeUpload).toHaveBeenCalledWith(7, { upload_id: 'r7', parts: [{ part_number: 1, etag: '"e1"' }] });
    expect(page.get('[role="status"]').text()).toBe('JO 1.7.5.9 is uploaded; the data program checks the package.');
    expect(page.find('#ds-release-progress').exists()).toBe(false);
  });

  it("ends a release upload with the service's refusal of a part, and removes the half-uploaded release", async () => {
    openRelease.mockResolvedValue({ ok: true, value: { release: { id: 8 }, upload: { key: 'updates/upload-8.sfs', upload_id: 'r8', part_size: PART_SIZE } } });
    putReleasePartMock.mockResolvedValue({ ok: false, error: { code: 'store_error', status: 502, message: 'The object store refused the request.' } });
    upload.mockImplementation(async (file, options) => {
      await options.putPart(1, file, options.signal);
      return [];
    });
    removeRelease.mockResolvedValue({ ok: true, value: undefined });
    const page = await mountSection();
    await pickFiles(page, '#ds-release-file', [new File(['p'], 'jo.sfs')]);
    submitForm(page, '#ds-release-name');
    await flushPromises();
    expect(removeRelease).toHaveBeenCalledWith(8);
    expect(completeUpload).not.toHaveBeenCalled();
    expect(page.get('#ds-release-progress').text()).toContain('The upload failed: The object store refused the request.');
  });
});
