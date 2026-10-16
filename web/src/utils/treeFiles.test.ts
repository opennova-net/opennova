import { describe, expect, it } from 'vitest';
import { canonicalTreeFile, pickTreeFiles, treeFileRules } from './treeFiles';

// A file as a folder pick (webkitdirectory) gives it: its path relative to
// the picked folder's parent.
function picked(path: string): File {
  const name = path.split('/').pop() ?? path;
  const file = new File(['x'], name);
  Object.defineProperty(file, 'webkitRelativePath', { value: path });
  return file;
}

describe('the kinds of tree', () => {
  it("lists a base's files, required first, and an expansion's pair", () => {
    expect(treeFileRules('base', '').map((r) => `${r.name}${r.required ? '*' : ''}`)).toEqual([
      'resource.pff*',
      'localres.pff*',
      'language.pff*',
      'version.txt',
      'score.ini',
      '_NSTMOUT.TXT',
      'gt.ssc',
    ]);
    expect(treeFileRules('expansion', 'jox01').map((r) => `${r.name}${r.required ? '*' : ''}`)).toEqual([
      'jox01.pff*',
      'jox01L.pff*',
      'version.txt*',
      'gt.ssc',
    ]);
  });

  it('spells a name the canonical way, whatever case it came in', () => {
    expect(canonicalTreeFile('base', '', 'RESOURCE.PFF')).toBe('resource.pff');
    expect(canonicalTreeFile('base', '', '_nstmout.txt')).toBe('_NSTMOUT.TXT');
    expect(canonicalTreeFile('expansion', 'jox01', 'JOX01L.PFF')).toBe('jox01L.pff');
    expect(canonicalTreeFile('base', '', 'jox01.pff')).toBeNull();
    expect(canonicalTreeFile('expansion', 'jox01', 'resource.pff')).toBeNull();
  });
});

describe('pickTreeFiles', () => {
  it("keeps a base's files at the picked folder's root, says what is missing and counts the rest", () => {
    const files = [
      picked('JointOps/RESOURCE.PFF'),
      picked('JointOps/localres.pff'),
      picked('JointOps/version.txt'),
      picked('JointOps/Jointops.exe'),
      picked('JointOps/expansion/jox01/jox01.pff'),
      picked('JointOps/expansion/jox01/language.pff'),
    ];
    const result = pickTreeFiles('base', '', files);
    expect(result.kept.map((k) => k.name)).toEqual(['resource.pff', 'localres.pff', 'version.txt']);
    expect(result.kept[0].file.name).toBe('RESOURCE.PFF');
    expect(result.missing).toEqual(['language.pff']);
    expect(result.ignored).toBe(3);
  });

  it("keeps an expansion's files from its own folder", () => {
    const files = [picked('jox01/jox01.pff'), picked('jox01/JOX01L.PFF'), picked('jox01/version.txt'), picked('jox01/notes.txt')];
    const result = pickTreeFiles('expansion', 'jox01', files);
    expect(result.kept.map((k) => k.name)).toEqual(['jox01.pff', 'jox01L.pff', 'version.txt']);
    expect(result.missing).toEqual([]);
    expect(result.ignored).toBe(1);
    // The whole install picked for an expansion: its files are deeper, so none is kept.
    const deep = pickTreeFiles('expansion', 'jox01', [picked('JointOps/expansion/jox01/jox01.pff')]);
    expect(deep.kept).toEqual([]);
    expect(deep.missing).toEqual(['jox01.pff', 'jox01L.pff', 'version.txt']);
    expect(deep.ignored).toBe(1);
  });

  it('takes a plain pick of files with no paths, each name once', () => {
    const files = [new File(['a'], 'resource.pff'), new File(['b'], 'RESOURCE.PFF'), new File(['c'], 'language.pff')];
    const result = pickTreeFiles('base', '', files);
    expect(result.kept.map((k) => k.name)).toEqual(['resource.pff', 'language.pff']);
    expect(result.kept[0].file.name).toBe('resource.pff');
    expect(result.missing).toEqual(['localres.pff']);
    expect(result.ignored).toBe(1);
  });
});
