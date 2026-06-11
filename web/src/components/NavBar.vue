<template>
  <header class="fixed inset-x-0 top-0 z-20 border-b border-white/5 bg-slate-950/70 backdrop-blur">
    <div class="mx-auto flex max-w-6xl items-center justify-between px-4 py-3 sm:px-6">
      <div class="flex items-center gap-3">
        <RouterLink to="/" class="text-lg font-semibold tracking-wide text-white" aria-label="OpenNova home">
          Open<span class="text-brand-200">Nova</span>
        </RouterLink>
        <span
          class="inline-flex h-2.5 w-2.5 items-center justify-center rounded-full bg-green-300 shadow-[0_0_0_4px_rgba(34,197,94,0.25)]"
          title="Online"
        ></span>
      </div>

      <nav class="hidden items-center gap-6 text-sm uppercase tracking-wider text-slate-200 md:flex">
        <RouterLink class="hover:text-white transition" to="/lobby">Lobbies</RouterLink>
        <RouterLink class="hover:text-white transition" to="/expansions">Expansions</RouterLink>
        <RouterLink class="hover:text-white transition" to="/mod-tools">Tools</RouterLink>
        <RouterLink class="hover:text-white transition" to="/admin/users">Admin</RouterLink>
        <RouterLink
          class="rounded-full border border-brand-200 px-4 py-1 text-brand-50 transition hover:bg-brand-500 hover:text-white"
          to="/register"
        >
          Register
        </RouterLink>
      </nav>

      <button
        class="inline-flex items-center justify-center rounded-md border border-white/20 p-2 text-slate-100 transition hover:border-white md:hidden"
        type="button"
        :aria-expanded="isMenuOpen.toString()"
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
        class="border-t border-white/5 bg-slate-950/95 px-4 pb-4 pt-2 shadow-lg shadow-slate-900/40 md:hidden"
      >
        <div class="flex flex-col gap-2 text-sm uppercase tracking-[0.2em] text-slate-200">
          <RouterLink class="rounded-md px-3 py-2 hover:bg-white/5" to="/lobby" @click="closeMenu">Lobbies</RouterLink>
          <RouterLink class="rounded-md px-3 py-2 hover:bg-white/5" to="/expansions" @click="closeMenu">Expansions</RouterLink>
          <RouterLink class="rounded-md px-3 py-2 hover:bg-white/5" to="/mod-tools" @click="closeMenu">Mod Tools</RouterLink>
          <RouterLink class="rounded-md px-3 py-2 hover:bg-white/5" to="/admin/users" @click="closeMenu">Admin</RouterLink>
          <RouterLink
            class="rounded-full border border-brand-200 px-4 py-2 text-center text-brand-50 transition hover:bg-brand-500 hover:text-white"
            to="/register"
            @click="closeMenu"
          >
            Register
          </RouterLink>
        </div>
      </nav>
    </Transition>
  </header>
</template>

<script setup lang="ts">
import { ref, watch } from 'vue';
import { RouterLink, useRoute } from 'vue-router';

const isMenuOpen = ref(false);
const toggleMenu = () => {
  isMenuOpen.value = !isMenuOpen.value;
};
const closeMenu = () => {
  isMenuOpen.value = false;
};

const route = useRoute();
watch(
  () => route.fullPath,
  () => {
    isMenuOpen.value = false;
  }
);
</script>
