// The attention list's one read (GET /api/admin/attention), shared under the
// admin layout: the nav's count and the Overview's and the Fleet's lists show
// the same answer. AdminLayout provides it, polled every 60 s while its gate is
// open (a closed gate sends no admin request); a page mounted without it (a
// test) reads its own.

import { inject, provide, ref, watch, type InjectionKey, type Ref } from 'vue';
import { fetchAttention } from '../api/attention';
import type { Attention } from '../types/attention';
import { usePolling } from './usePolling';

export const ATTENTION_POLL_MS = 60_000;

export interface AttentionState {
  attention: Ref<Attention | null>;
  // The last read's failure, '' after an answer.
  error: Ref<string>;
  // When the last answer came (ms).
  loadedAt: Ref<number | null>;
  loading: Ref<boolean>;
  refresh: () => Promise<void>;
}

const KEY: InjectionKey<AttentionState> = Symbol('attention');

function readAttention(enabled: Ref<boolean>): AttentionState {
  const attention = ref<Attention | null>(null);
  const error = ref('');
  const loadedAt = ref<number | null>(null);
  const poll = usePolling({
    async read(signal) {
      // A closed gate: nothing to read, and no poll until it opens.
      if (!enabled.value) return true;
      const result = await fetchAttention(signal);
      if (signal.aborted) return false;
      if (result.ok) {
        attention.value = result.value;
        loadedAt.value = Date.now();
        error.value = '';
        return true;
      }
      error.value = result.error.message;
      return false;
    },
    next: () => (enabled.value ? ATTENTION_POLL_MS : null),
    immediate: false,
  });
  watch(enabled, (on) => on && void poll.refresh(), { immediate: true });
  return { attention, error, loadedAt, loading: poll.loading, refresh: poll.refresh };
}

// The layout's: read while `enabled` holds, and given to every page under it.
export function provideAttention(enabled: Ref<boolean>): AttentionState {
  const state = readAttention(enabled);
  provide(KEY, state);
  return state;
}

// A page's: the layout's read, or its own when it has none.
export function useAttention(): AttentionState {
  return inject(KEY, null) ?? readAttention(ref(true));
}
