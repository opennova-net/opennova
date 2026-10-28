<template>
  <AdminGate>
    <Workspace label="Admin" :items="items" :current="sectionOf(route.path)?.to ?? null">
      <RouterView />
      <template v-if="hasAdminToken && !isAdmin" #footer>
        <p class="mb-2 text-xs text-ink-muted">Using an admin token</p>
        <button type="button" class="btn-ghost btn-sm" @click="clearAdminToken">Forget token</button>
      </template>
    </Workspace>
  </AdminGate>
</template>

<script setup lang="ts">
// The admin (/admin/...): its six sections down the side, the Overview's
// count of what needs an admin and Products' of the releases awaiting one,
// in the frame the player's own pages share (components/ui/Workspace.vue).
import { computed } from 'vue';
import { RouterView, useRoute } from 'vue-router';
import AdminGate from '../../components/AdminGate.vue';
import Workspace from '../../components/ui/Workspace.vue';
import type { SideNavItem } from '../../components/ui/SideNav.vue';
import { attentionCount } from '../../api/attention';
import { clearAdminToken, hasAdminToken } from '../../api/authToken';
import { provideAttention } from '../../composables/useAttention';
import { useSession } from '../../composables/useSession';
import { ADMIN_SECTIONS, sectionOf } from '../../utils/adminNav';

const route = useRoute();
const { isAdmin } = useSession();

const attention = provideAttention(computed(() => isAdmin.value || hasAdminToken.value));
const unread = computed(() => attention.error.value !== '');
const needing = computed(() => (attention.attention.value ? attentionCount(attention.attention.value) : 0));
// Releases waiting for an admin's approval: each is a finding of its own.
const awaiting = computed(() => attention.attention.value?.findings.filter((f) => f.source === 'releases').length ?? 0);

const items = computed<SideNavItem[]>(() =>
  ADMIN_SECTIONS.map((s) => {
    if (s.to === '/admin' && unread.value) {
      return { ...s, badge: { text: '?', sr: 'What needs you could not be read', tone: 'muted', class: 'admin-attention-unknown', title: 'What needs you could not be read', textHidden: true } };
    }
    if (s.to === '/admin' && needing.value > 0) {
      return { ...s, badge: { text: String(needing.value), sr: needing.value === 1 ? ' thing needs you' : ' things need you', tone: 'bad', class: 'admin-attention-count' } };
    }
    if (s.to === '/admin/products' && awaiting.value > 0) {
      return { ...s, badge: { text: String(awaiting.value), sr: ' awaiting approval', tone: 'warn', class: 'admin-awaiting-count' } };
    }
    return { to: s.to, label: s.label };
  }),
);
</script>
