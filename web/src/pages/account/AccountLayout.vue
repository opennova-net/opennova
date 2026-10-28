<template>
  <div class="page py-6 lg:py-10">
    <div class="mb-6 flex justify-end lg:hidden">
      <button ref="menuButton" type="button" class="btn-secondary" aria-haspopup="dialog" aria-controls="account-menu" @click="openMenu">Account menu</button>
    </div>

    <div class="flex gap-10">
      <aside class="hidden w-52 shrink-0 lg:block">
        <div class="sticky top-24 space-y-5">
          <div class="border-b border-border pb-4">
            <p class="text-xs font-medium text-ink-muted">Your account</p>
            <p v-if="user" class="account-user truncate font-semibold text-ink-bright">{{ user.username }}</p>
          </div>
          <nav aria-label="Your account">
            <ul class="space-y-1">
              <li v-for="item in sections" :key="item.to">
                <RouterLink
                  :to="item.to"
                  class="workspace-nav-link"
                  :class="isActive(item) ? 'bg-raised text-accent' : ''"
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
      <div class="mb-6 flex items-center justify-between gap-3">
        <div class="min-w-0">
          <h2 id="account-menu-title" class="section-heading">Your account</h2>
          <p v-if="user" class="break-words text-sm text-ink-muted">{{ user.username }}</p>
        </div>
        <button type="button" class="btn-secondary" @click="menu?.close()">Close</button>
      </div>
      <nav aria-label="Account menu">
        <ul class="space-y-2">
          <li v-for="item in sections" :key="item.to">
            <RouterLink :to="item.to" class="workspace-nav-link" :class="isActive(item) ? 'bg-raised text-accent' : ''" :aria-current="isActive(item) ? 'page' : undefined" @click="menu?.close()">{{ item.label }}</RouterLink>
          </li>
        </ul>
      </nav>
    </dialog>
  </div>
</template>

<script setup lang="ts">
// The account area (/me/...): the username and a side nav over its pages (a
// drawer on a narrow screen). Its routes need a logged-in visitor.
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
  { to: '/me/projects', label: 'Projects' },
  { to: '/me/billing', label: 'Billing' },
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
