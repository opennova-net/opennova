<template>
  <div class="flex min-h-screen flex-col bg-surface text-ink">
    <a href="#content" class="sr-only focus:not-sr-only focus:fixed focus:left-4 focus:top-3 focus:z-30 focus:rounded-control focus:bg-accent focus:px-4 focus:py-2 focus:text-on-accent">
      Skip to content
    </a>
    <NavBar />
    <!-- The one <main>: its top padding clears the fixed header, so a page
         starts its own spacing from zero. -->
    <main id="content" class="flex-1 pt-16 pb-16">
      <RouterView />
    </main>
    <SiteFooter />
  </div>
</template>

<script setup lang="ts">
import { RouterView } from 'vue-router';
import NavBar from './components/NavBar.vue';
import SiteFooter from './components/SiteFooter.vue';
</script>
