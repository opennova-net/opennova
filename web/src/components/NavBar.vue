<template>
  <header class="fixed inset-x-0 top-0 z-20 border-b border-border bg-surface/95">
    <div class="mx-auto flex max-w-6xl items-center justify-between px-4 py-3 sm:px-6">
      <div class="flex items-center gap-3">
        <RouterLink to="/" class="text-lg font-semibold tracking-wide text-ink" aria-label="OpenNova home">
          Open<span class="text-accent">Nova</span>
        </RouterLink>
        <span
          class="inline-flex h-2.5 w-2.5 items-center justify-center rounded-full bg-online"
          title="Online"
        ></span>
      </div>

      <nav class="hidden items-center gap-6 text-sm uppercase tracking-wider text-ink md:flex">
        <RouterLink class="hover:text-ink-bright transition" to="/lobby">Lobbies</RouterLink>
        <RouterLink class="hover:text-ink-bright transition" to="/admin">Admin</RouterLink>
        <template v-if="ready">
          <template v-if="user">
            <RouterLink class="hover:text-ink-bright transition" to="/me/servers">My servers</RouterLink>
            <span class="text-ink-muted">
              <span class="sr-only">Logged in as </span>
              <span class="normal-case tracking-normal font-medium text-ink">{{ user.username }}</span>
            </span>
            <button
              type="button"
              class="rounded-control border border-border-strong px-4 py-1 uppercase tracking-wider text-ink transition hover:border-accent disabled:opacity-50"
              :disabled="signingOut"
              @click="handleSignOut('desktop')"
            >
              Log out
            </button>
          </template>
          <template v-else>
            <RouterLink ref="desktopLogin" class="hover:text-ink-bright transition" :to="loginLink">Log in</RouterLink>
            <RouterLink
              class="rounded-control border border-accent px-4 py-1 text-accent transition hover:bg-accent hover:text-on-accent"
              to="/register"
            >
              Register
            </RouterLink>
          </template>
        </template>
      </nav>

      <button
        ref="menuToggle"
        class="inline-flex items-center justify-center rounded-control border border-border-strong p-2 text-ink transition hover:bg-hover md:hidden"
        type="button"
        :aria-expanded="isMenuOpen"
        aria-controls="primary-navigation"
        @click="toggleMenu"
      >
        <span class="sr-only">Toggle navigation</span>
        <svg
          v-if="!isMenuOpen"
          xmlns="http://www.w3.org/2000/svg"
          class="h-5 w-5"
          fill="none"
          viewBox="0 0 24 24"
          stroke="currentColor"
          stroke-width="2"
        >
          <path stroke-linecap="round" stroke-linejoin="round" d="M4 6h16M4 12h16M4 18h16" />
        </svg>
        <svg
          v-else
          xmlns="http://www.w3.org/2000/svg"
          class="h-5 w-5"
          fill="none"
          viewBox="0 0 24 24"
          stroke="currentColor"
          stroke-width="2"
        >
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
      <nav
        v-if="isMenuOpen"
        id="primary-navigation"
        class="border-t border-border bg-surface/95 px-4 pb-4 pt-2 md:hidden"
      >
        <div class="flex flex-col gap-2 text-sm uppercase tracking-[0.2em] text-ink">
          <RouterLink class="rounded-control px-3 py-2 hover:bg-hover" to="/lobby" @click="closeMenu">Lobbies</RouterLink>
          <RouterLink class="rounded-control px-3 py-2 hover:bg-hover" to="/admin" @click="closeMenu">Admin</RouterLink>
          <template v-if="ready">
            <template v-if="user">
              <RouterLink class="rounded-control px-3 py-2 hover:bg-hover" to="/me/servers" @click="closeMenu">My servers</RouterLink>
              <p class="px-3 py-2 text-ink-muted">
                Logged in as
                <span class="normal-case tracking-normal font-medium text-ink">{{ user.username }}</span>
              </p>
              <button
                type="button"
                class="rounded-control border border-border-strong px-4 py-2 text-center uppercase tracking-[0.2em] text-ink transition hover:border-accent disabled:opacity-50"
                :disabled="signingOut"
                @click="handleSignOut('mobile')"
              >
                Log out
              </button>
            </template>
            <template v-else>
              <RouterLink class="rounded-control px-3 py-2 hover:bg-hover" :to="loginLink" @click="closeMenu">Log in</RouterLink>
              <RouterLink
                class="rounded-control border border-accent px-4 py-2 text-center text-accent transition hover:bg-accent hover:text-on-accent"
                to="/register"
                @click="closeMenu"
              >
                Register
              </RouterLink>
            </template>
          </template>
        </div>
      </nav>
    </Transition>
  </header>
</template>

<script setup lang="ts">
import { computed, nextTick, ref, watch } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import { useSession } from '../composables/useSession';

const isMenuOpen = ref(false);
const toggleMenu = () => {
  isMenuOpen.value = !isMenuOpen.value;
};
const closeMenu = () => {
  isMenuOpen.value = false;
};

const route = useRoute();
const router = useRouter();
watch(
  () => route.fullPath,
  () => {
    isMenuOpen.value = false;
  }
);

const { user, ready, signOut } = useSession();
const signingOut = ref(false);

// The login link brings the visitor back to the page they were on.
const loginLink = computed(() => {
  if (route.path === '/' || route.path === '/login' || route.path === '/register') return '/login';
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
