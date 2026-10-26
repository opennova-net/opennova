<template>
  <div class="page py-6 lg:py-10">
    <div class="mb-6 flex items-center justify-between gap-4">
      <p class="eyebrow">Your space</p>
      <button ref="menuButton" type="button" class="btn-secondary lg:hidden" aria-haspopup="dialog" aria-controls="account-menu" @click="openMenu">Account menu</button>
    </div>

    <div class="flex gap-10">
      <aside class="hidden w-52 shrink-0 lg:block">
        <div class="sticky top-24 space-y-6">
          <div v-if="user" class="border-b border-border pb-5">
            <p class="truncate font-semibold text-ink">{{ user.username }}</p>
            <p class="mt-1 text-xs text-ink-muted">NovaWorld account</p>
          </div>
          <nav aria-label="Your account">
            <ul class="space-y-1">
              <li v-for="item in sections" :key="item.to">
                <RouterLink
                  :to="item.to"
                  class="workspace-nav-link"
                  :class="isActive(item) ? 'bg-raised text-accent' : 'text-ink-muted hover:bg-hover hover:text-ink'"
                  :aria-current="isActive(item) ? 'page' : undefined"
                >
                  {{ item.label }}
                </RouterLink>
              </li>
            </ul>
          </nav>
          <RouterLink to="/lobby" class="block text-sm text-ink-muted hover:text-accent">Find a game <span aria-hidden="true">↗</span></RouterLink>
        </div>
      </aside>

      <div class="min-w-0 flex-1">
        <RouterView />
      </div>
    </div>
    <dialog id="account-menu" ref="menu" aria-labelledby="account-menu-title" class="m-0 ml-auto h-dvh max-h-none w-80 max-w-[90vw] border-l border-border bg-panel p-6 text-ink backdrop:bg-black/60" @click="onBackdrop" @close="onClosed">
      <div class="mb-8 flex items-center justify-between gap-3">
        <h2 id="account-menu-title" class="text-lg font-semibold">Your account</h2>
        <button type="button" class="btn-secondary" @click="menu?.close()">Close</button>
      </div>
      <p v-if="user" class="mb-5 break-words text-sm text-ink-muted">{{ user.username }}</p>
      <nav aria-label="Account menu">
        <ul class="space-y-2">
          <li v-for="item in sections" :key="item.to">
            <RouterLink :to="item.to" class="workspace-nav-link" :class="isActive(item) ? 'bg-raised text-accent' : 'text-ink-muted'" :aria-current="isActive(item) ? 'page' : undefined" @click="menu?.close()">{{ item.label }}</RouterLink>
          </li>
        </ul>
      </nav>
    </dialog>
  </div>
</template>

<script setup lang="ts">
// The account area (/me/...): a side nav over its pages, modelled on the admin
// layout. Its routes need a logged-in visitor (the router's requiresUser).
import { RouterLink, RouterView, useRoute } from 'vue-router';
import { onMounted, onUnmounted, ref, watch } from 'vue';
import { useSession } from '../../composables/useSession';

interface AccountSection {
  to: string;
  label: string;
}

const sections: AccountSection[] = [
  { to: '/me', label: 'Overview' },
  { to: '/me/servers', label: 'Servers' },
  { to: '/me/support', label: 'Billing' },
  { to: '/me/account', label: 'Account' },
];

const route = useRoute();
const { user } = useSession();
const menu = ref<HTMLDialogElement | null>(null);
const menuButton = ref<HTMLButtonElement | null>(null);

// A section is active on its own path and every path under it (a server's
// page is under Servers).
function isActive(item: AccountSection): boolean {
  if (item.to === '/me') return route.path === '/me' || route.path === '/me/';
  if (item.to === '/me/servers' && route.path === '/me/addresses') return true;
  return route.path === item.to || route.path.startsWith(`${item.to}/`);
}
function openMenu() { menu.value?.showModal(); }
function onClosed() { if (window.innerWidth < 1024) menuButton.value?.focus(); }
function onResize() { if (menu.value?.open && window.innerWidth >= 1024) menu.value.close(); }
function onBackdrop(event: MouseEvent) {
  if (event.target !== menu.value || !menu.value) return;
  const rect = menu.value.getBoundingClientRect();
  if (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom) menu.value.close();
}
watch(() => route.fullPath, () => { if (menu.value?.open) menu.value.close(); });
onMounted(() => window.addEventListener('resize', onResize));
onUnmounted(() => window.removeEventListener('resize', onResize));
</script>
