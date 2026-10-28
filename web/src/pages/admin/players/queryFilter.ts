// A filter the Players pages keep in the URL query, so a link can open them
// filtered (/admin/players/accounts?q=biggy): read from the query, written
// back with replace, and the keys in `resets` (the page) dropped with it.

import { computed, type WritableComputedRef } from 'vue';
import { useRoute, useRouter } from 'vue-router';

export function useQueryText(key: string, resets: readonly string[] = []): WritableComputedRef<string> {
  const route = useRoute();
  const router = useRouter();
  return computed({
    get: () => {
      const v = route.query[key];
      return typeof v === 'string' ? v : '';
    },
    set: (value: string) => {
      const query: Record<string, unknown> = { ...route.query, [key]: value || undefined };
      for (const r of resets) query[r] = undefined;
      void router.replace({ path: route.path, query: query as typeof route.query });
    },
  });
}
