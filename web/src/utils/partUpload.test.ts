import { Blob as NodeBlob } from 'node:buffer';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { partCount, uploadFile } from './partUpload';

// What one putPart got: the part's number and its bytes.
interface Put {
  partNumber: number;
  body: Uint8Array;
}

// A putPart that keeps every part it was given and answers it the way
// `answer` says (an ETag by default).
function storePut(answer: (put: Put, call: number) => Promise<{ etag: string }> | { etag: string } = okWithEtag) {
  const puts: Put[] = [];
  const putPart = vi.fn(async (partNumber: number, blob: Blob, _signal?: AbortSignal) => {
    const put: Put = { partNumber, body: new Uint8Array(await blob.arrayBuffer()) };
    puts.push(put);
    return answer(put, puts.length);
  });
  return { putPart, puts };
}

function okWithEtag(put: Put): { etag: string } {
  return { etag: `"etag-${put.partNumber}"` };
}

const bytes = (text: string) => new TextEncoder().encode(text);

describe('uploadFile', () => {
  beforeEach(() => {
    // jsdom's Blob cannot give its bytes (no arrayBuffer); Node's can, and
    // slices the same way.
    vi.stubGlobal('Blob', NodeBlob);
  });

  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('sends the file in parts of the given size and keeps the ETags', async () => {
    const { putPart, puts } = storePut();
    const progress: number[] = [];
    const parts = await uploadFile(new Blob([bytes('0123456789')]), {
      partSize: 4,
      putPart,
      onProgress: (n) => progress.push(n),
    });

    expect(parts).toEqual([
      { part_number: 1, etag: '"etag-1"' },
      { part_number: 2, etag: '"etag-2"' },
      { part_number: 3, etag: '"etag-3"' },
    ]);
    // Each part carried its slice, and only that.
    expect(puts.map((p) => p.partNumber)).toEqual([1, 2, 3]);
    expect(puts.map((p) => new TextDecoder().decode(p.body))).toEqual(['0123', '4567', '89']);
    // Progress counted the bytes stored so far, up to the whole file.
    expect(progress).toHaveLength(3);
    expect(progress[progress.length - 1]).toBe(10);
  });

  it('keeps at most three parts in flight', async () => {
    let inFlight = 0;
    let most = 0;
    const release: (() => void)[] = [];
    const { putPart } = storePut(
      (put) =>
        new Promise<{ etag: string }>((resolve) => {
          inFlight++;
          most = Math.max(most, inFlight);
          release.push(() => {
            inFlight--;
            resolve(okWithEtag(put));
          });
        }),
    );
    const done = uploadFile(new Blob([bytes('abcdefg')]), { partSize: 1, putPart });
    // Let the first parts reach the service.
    await vi.waitFor(() => expect(release).toHaveLength(3));
    expect(putPart).toHaveBeenCalledTimes(3);
    // Each part that lands lets the next one go.
    while (release.length > 0) {
      release.shift()!();
      await new Promise((r) => setTimeout(r, 0));
    }
    const parts = await done;
    expect(parts.map((p) => p.part_number)).toEqual([1, 2, 3, 4, 5, 6, 7]);
    expect(most).toBe(3);
    expect(putPart).toHaveBeenCalledTimes(7);
  });

  it('tries a failed part again, three more times, then gives up', async () => {
    let attempts = 0;
    const { putPart } = storePut((put) => {
      attempts++;
      if (attempts === 1) throw new TypeError('network down');
      if (attempts === 2) throw new Error('The object store refused the request.');
      return okWithEtag(put);
    });
    const parts = await uploadFile(new Blob([bytes('ab')]), { partSize: 8, putPart, retryDelayMs: 0 });
    expect(parts).toEqual([{ part_number: 1, etag: '"etag-1"' }]);
    expect(putPart).toHaveBeenCalledTimes(3);

    const failing = storePut(() => {
      throw new Error('The service answered part 1 with HTTP 503.');
    });
    await expect(uploadFile(new Blob([bytes('ab')]), { partSize: 8, putPart: failing.putPart, retryDelayMs: 0 })).rejects.toThrow('HTTP 503');
    expect(failing.putPart).toHaveBeenCalledTimes(4);
  });

  it('stops when the signal aborts', async () => {
    const controller = new AbortController();
    const { putPart } = storePut(
      (put) =>
        new Promise<{ etag: string }>((resolve, reject) => {
          controller.signal.addEventListener('abort', () => reject(new DOMException('aborted', 'AbortError')));
          if (put.partNumber === 1) controller.abort();
          else resolve(okWithEtag(put));
        }),
    );
    const upload = uploadFile(new Blob([bytes('abcdefgh')]), { partSize: 1, putPart, signal: controller.signal, retryDelayMs: 0 });
    await expect(upload).rejects.toMatchObject({ name: 'AbortError' });
    // The three in flight went out, each with the upload's signal; nothing after them.
    expect(putPart.mock.calls.length).toBeLessThanOrEqual(3);
    expect(putPart.mock.calls[0][2]).toBe(controller.signal);

    // Already aborted: nothing goes out at all.
    const aborted = new AbortController();
    aborted.abort();
    const idle = storePut();
    await expect(uploadFile(new Blob([bytes('ab')]), { partSize: 1, putPart: idle.putPart, signal: aborted.signal })).rejects.toMatchObject({
      name: 'AbortError',
    });
    expect(idle.putPart).not.toHaveBeenCalled();
  });

  it('sends an empty file as one empty part', async () => {
    const { putPart, puts } = storePut();
    expect(partCount(0, 4)).toBe(1);
    expect(partCount(8, 4)).toBe(2);
    expect(partCount(9, 4)).toBe(3);
    const parts = await uploadFile(new Blob([]), { partSize: 4, putPart });
    expect(parts).toHaveLength(1);
    expect(puts[0].body).toHaveLength(0);
  });
});
