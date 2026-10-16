// One file to the store in parts, through the site (S3's multipart upload,
// which web relays: the browser PUTs each part to the admin route, web hands
// it to the store and answers the store's ETag, which the complete call
// names). At most three parts are in flight; a part that fails is tried
// again, three more times, after a short wait that doubles; an aborted
// signal stops everything.

import type { UploadedPart } from '../types/dataSets';

// How many parts are sent at once.
export const PARTS_IN_FLIGHT = 3;
// How many more times a failed part is sent.
export const PART_RETRIES = 3;

export interface UploadFileOptions {
  // Every part but the last is this long (the API's part_size, 32 MiB).
  partSize: number;
  // Stores part `partNumber` (from 1), the given bytes, and resolves with
  // the store's ETag. A rejection (the service's refusal, no reply at all)
  // is tried again; `signal` is the upload's, for the request in flight.
  putPart: (partNumber: number, blob: Blob, signal?: AbortSignal) => Promise<{ etag: string }>;
  // Called with the bytes stored so far, after each part lands.
  onProgress?: (uploadedBytes: number) => void;
  signal?: AbortSignal;
  // The wait before a part's first retry, doubled each time (tests shorten it).
  retryDelayMs?: number;
}

function abortError(signal: AbortSignal | undefined): Error {
  const reason = signal?.reason;
  if (reason instanceof Error) return reason;
  return new DOMException('The upload was cancelled.', 'AbortError');
}

export function isAbortError(err: unknown): boolean {
  return err instanceof DOMException ? err.name === 'AbortError' : (err as { name?: unknown } | null)?.name === 'AbortError';
}

function wait(ms: number, signal: AbortSignal | undefined): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      reject(abortError(signal));
      return;
    }
    const timer = setTimeout(() => {
      signal?.removeEventListener('abort', onAbort);
      resolve();
    }, ms);
    function onAbort() {
      clearTimeout(timer);
      reject(abortError(signal));
    }
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

// The parts of a file, each `partSize` long but the last; an empty file is
// one empty part (a multipart upload needs at least one).
export function partCount(size: number, partSize: number): number {
  return Math.max(1, Math.ceil(size / partSize));
}

async function sendPart(file: Blob, options: UploadFileOptions, partNumber: number): Promise<UploadedPart> {
  const { partSize, signal } = options;
  const start = (partNumber - 1) * partSize;
  const slice = file.slice(start, Math.min(file.size, start + partSize));
  let lastError: unknown = null;
  for (let attempt = 0; attempt <= PART_RETRIES; attempt++) {
    if (signal?.aborted) throw abortError(signal);
    if (attempt > 0) await wait((options.retryDelayMs ?? 500) * 2 ** (attempt - 1), signal);
    try {
      const { etag } = await options.putPart(partNumber, slice, signal);
      return { part_number: partNumber, etag };
    } catch (err) {
      if (signal?.aborted) throw abortError(signal);
      lastError = err;
    }
  }
  throw lastError instanceof Error ? lastError : new Error(`Part ${partNumber} could not be stored.`);
}

// The file in the store, part by part; the parts for the complete call, in
// order. Rejects with the first failure (an AbortError when cancelled), once
// the parts in flight have settled.
export async function uploadFile(file: Blob, options: UploadFileOptions): Promise<UploadedPart[]> {
  const { partSize, signal } = options;
  if (!Number.isInteger(partSize) || partSize <= 0) throw new Error('The part size must be a whole number of bytes.');
  if (signal?.aborted) throw abortError(signal);
  const count = partCount(file.size, partSize);
  const parts: UploadedPart[] = new Array(count);
  let next = 1;
  let uploaded = 0;
  let failure: unknown = null;

  async function worker(): Promise<void> {
    while (failure === null && next <= count) {
      const partNumber = next++;
      try {
        const part = await sendPart(file, options, partNumber);
        parts[partNumber - 1] = part;
        uploaded += Math.min(partSize, file.size - (partNumber - 1) * partSize);
        options.onProgress?.(uploaded);
      } catch (err) {
        if (failure === null) failure = err;
        return;
      }
    }
  }

  await Promise.all(Array.from({ length: Math.min(PARTS_IN_FLIGHT, count) }, worker));
  if (failure !== null) throw failure;
  return parts;
}
