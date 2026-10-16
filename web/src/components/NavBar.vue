<template>
  <header @keydown.esc="closeMenuToToggle" class="fixed inset-x-0 top-0 z-20 border-b border-border bg-surface/95 backdrop-blur">
    <div class="page flex h-16 items-center justify-between gap-6">
      <RouterLink to="/" class="flex items-center gap-2 text-lg font-semibold tracking-wide text-ink" aria-label="OpenNova home">
        <img src="/favicon.svg" alt="" class="h-6 w-6" width="24" height="24" />
        <span>Open<span class="text-accent">Nova</span></span>
      </RouterLink>

      <nav aria-label="Primary" class="hidden flex-1 items-center gap-6 text-sm text-ink-muted lg:flex">
        <RouterLink v-for="link in siteLinks" :key="link.to" :to="link.to" class="transition hover:text-ink-bright" active-class="text-ink">
          {{ link.label }}
        </RouterLink>
        <a :href="GITHUB_URL" target="_blank" rel="noopener" class="transition hover:text-ink-bright">GitHub</a>
        <RouterLink v-if="isAdmin" to="/admin" class="transition hover:text-ink-bright" active-class="text-ink">Admin</RouterLink>
      </nav>

      <div class="hidden items-center gap-4 text-sm lg:flex">
        <template v-if="ready">
          <template v-if="user">
            <RouterLink to="/me/servers" class="text-ink-muted transition hover:text-ink-bright" active-class="text-ink">My servers</RouterLink>
            <RouterLink to="/me/account" class="max-w-[10rem] truncate font-medium text-ink transition hover:text-ink-bright">
              <span class="sr-only">Your account: logged in as </span>{{ user.username }}
            </RouterLink>
            <button type="button" class="text-ink-muted transition hover:text-ink-bright disabled:opacity-50" :disabled="signingOut" @click="handleSignOut('desktop')">
              Log out
            </button>
          </template>
          <template v-else>
            <RouterLink ref="desktopLogin" :to="loginLink" class="text-ink-muted transition hover:text-ink-bright">Log in</RouterLink>
            <RouterLink to="/register" class="text-ink-muted transition hover:text-ink-bright">Register</RouterLink>
          </template>
        </template>
        <RouterLink :to="downloadsLink" class="btn-primary px-4 py-2">Download</RouterLink>
      </div>

      <button
        ref="menuToggle"
        class="inline-flex h-11 w-11 items-center justify-center rounded-control border border-border-strong text-ink transition hover:bg-hover lg:hidden"
        type="button"
        :aria-expanded="isMenuOpen"
        aria-controls="primary-navigation"
        @click="toggleMenu"
      >
        <span class="sr-only">{{ isMenuOpen ? 'Close the menu' : 'Open the menu' }}</span>
        <svg v-if="!isMenuOpen" xmlns="http://www.w3.org/2000/svg" class="h-5 w-5" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2" aria-hidden="true">
          <path stroke-linecap="round" stroke-linejoin="round" d="M4 6h16M4 12h16M4 18h16" />
        </svg>
        <svg v-else xmlns="http://www.w3.org/2000/svg" class="h-5 w-5" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2" aria-hidden="true">
          <path stroke-linecap="round" stroke-linejoin="round" d="M6 18L18 6M6 6l12 12" />
        </svg>
      </button>
    </div>

    <Transition
      enter-active-class="transition duration-150 ease-out"
      enter-from-class="opacity-0 -translate-y-2"
      enter-to-class="opacity-100 translate-y-0"
      leave-active-class="transition duration-150 ease-in"
      leave-from-class="opacity-100 translate-y-0"
      leave-to-class="opacity-0 -translate-y-2"
    >
      <nav v-if="isMenuOpen" id="primary-navigation" aria-label="Primary" class="max-h-[calc(100vh-4rem)] overflow-y-auto border-t border-border bg-surface px-4 pb-4 pt-2 lg:hidden">
        <div class="flex flex-col gap-1 text-base text-ink">
          <RouterLink v-for="link in siteLinks" :key="link.to" :to="link.to" class="rounded-control px-3 py-2.5 hover:bg-hover" @click="closeMenu">
            {{ link.label }}
          </RouterLink>
          <a :href="GITHUB_URL" target="_blank" rel="noopener" class="rounded-control px-3 py-2.5 hover:bg-hover" @click="closeMenu">GitHub</a>
          <RouterLink v-if="isAdmin" to="/admin" class="rounded-control px-3 py-2.5 hover:bg-hover" @click="closeMenu">Admin</RouterLink>
          <template v-if="ready">
            <template v-if="user">
              <RouterLink to="/me/servers" class="rounded-control px-3 py-2.5 hover:bg-hover" @click="closeMenu">My servers</RouterLink>
              <RouterLink to="/me/account" class="rounded-control px-3 py-2.5 text-ink-muted hover:bg-hover" @click="closeMenu">
                Account: <span class="font-medium text-ink">{{ user.username }}</span>
              </RouterLink>
              <button type="button" class="rounded-control px-3 py-2.5 text-left hover:bg-hover disabled:opacity-50" :disabled="signingOut" @click="handleSignOut('mobile')">
                Log out
              </button>
            </template>
            <template v-else>
              <RouterLink :to="loginLink" class="rounded-control px-3 py-2.5 hover:bg-hover" @click="closeMenu">Log in</RouterLink>
              <RouterLink to="/register" class="rounded-control px-3 py-2.5 hover:bg-hover" @click="closeMenu">Register</RouterLink>
            </template>
          </template>
          <RouterLink :to="downloadsLink" class="btn-primary mt-2" @click="closeMenu">Download</RouterLink>
        </div>
      </nav>
    </Transition>
  </header>
</template>

<script setup lang="ts">
import { computed, nextTick, ref, watch } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import { useSession } from '../composables/useSession';
import { GITHUB_URL } from '../utils/links';

const siteLinks = [
  { to: '/lobby', label: 'Lobbies' },
  { to: '/support', label: 'Support tiers' },
];

// Download goes to the home page's downloads section, which says what each
// release file is.
const downloadsLink = { path: '/', hash: '#downloads' };

const isMenuOpen = ref(false);
const toggleMenu = () => {
  isMenuOpen.value = !isMenuOpen.value;
};
const closeMenu = () => {
  isMenuOpen.value = false;
};
// Escape closes the open menu and gives the keyboard back to its toggle.
const closeMenuToToggle = () => {
  if (!isMenuOpen.value) return;
  closeMenu();
  menuToggle.value?.focus();
};

const route = useRoute();
const router = useRouter();
watch(
  () => route.fullPath,
  () => {
    isMenuOpen.value = false;
  }
);

const { user, ready, isAdmin, signOut } = useSession();
const signingOut = ref(false);

// The login link brings the visitor back to the page they were on, but not to
// one of the account pages a login leaves behind.
const NO_RETURN = new Set(['/', '/login', '/register', '/forgot', '/reset']);
const loginLink = computed(() => {
  if (NO_RETURN.has(route.path)) return '/login';
  return { path: '/login', query: { redirect: route.fullPath } };
});

const desktopLogin = ref<{ $el: HTMLElement } | null>(null);
const menuToggle = ref<HTMLButtonElement | null>(null);

// Log out (which also forgets a stored admin token, useSession.signOut). The
// button goes away with the session, so the keyboard moves to what takes
// its place: the Log in link on desktop, the menu toggle on mobile (the menu
// closes).
async function handleSignOut(from: 'desktop' | 'mobile') {
  signingOut.value = true;
  const result = await signOut();
  signingOut.value = false;
  closeMenu();
  if (!result.ok) {
    window.alert(result.error.message);
    return;
  }
  // A page only a logged-in visitor may see is left for the home page.
  if (route.matched.some((record) => record.meta.requiresUser)) {
    await router.push('/');
  }
  await nextTick();
  if (from === 'desktop') desktopLogin.value?.$el.focus();
  else menuToggle.value?.focus();
}
</script>
