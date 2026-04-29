<template>
  <div class="min-h-screen bg-slate-950 text-slate-100">
    <NavBar />
    <main class="pt-20 pb-16">
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
