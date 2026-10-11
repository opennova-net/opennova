<template>
  <section class="px-6 pt-24 pb-16">
    <div class="mx-auto max-w-md rounded-panel border border-border bg-panel p-8">
      <h1 class="text-3xl font-semibold text-ink">Log in</h1>

      <p v-if="view === 'checking'" role="status" class="mt-2 text-sm text-ink-muted">
        Checking your session…
      </p>

      <template v-else-if="view === 'loggedIn' && user">
        <p class="mt-2 text-sm text-ink-muted">
          You are logged in as <span class="font-semibold text-ink">{{ user.username }}</span>.
        </p>
        <div class="mt-8 flex flex-wrap gap-3">
          <RouterLink
            :to="target"
            class="rounded-control bg-accent px-6 py-3 font-semibold text-on-accent hover:bg-accent/90"
          >
            Continue
          </RouterLink>
          <button
            type="button"
            :disabled="signingOut"
            class="rounded-control border border-border-strong px-6 py-3 font-semibold text-ink transition hover:border-accent disabled:opacity-50"
            @click="handleSignOut"
          >
            {{ signingOut ? 'Logging out…' : 'Log out' }}
          </button>
        </div>
        <p
          v-if="error"
          role="alert"
          class="mt-6 rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger"
        >
          {{ error }}
        </p>
      </template>

      <template v-else>
        <p class="mt-2 text-sm text-ink-muted">
          Use your NovaWorld account: the handle and password you log in with in-game.
        </p>
        <form class="mt-8 grid gap-6" @submit.prevent="handleSubmit">
          <label class="grid gap-2 text-sm text-ink">
            Username
            <input
              ref="usernameInput"
              v-model="form.username"
              name="username"
              type="text"
              autocomplete="username"
              autocapitalize="none"
              spellcheck="false"
              required
              maxlength="64"
              :aria-invalid="usernameInvalid ? 'true' : undefined"
              :aria-describedby="usernameInvalid ? 'login-error' : undefined"
              class="rounded-control border border-border bg-surface px-4 py-3 focus:border-accent focus:outline-none"
            />
          </label>
          <label class="grid gap-2 text-sm text-ink">
            Password
            <input
              ref="passwordInput"
              v-model="form.password"
              name="password"
              type="password"
              autocomplete="current-password"
              required
              :aria-invalid="passwordInvalid ? 'true' : undefined"
              :aria-describedby="passwordInvalid ? 'login-error' : undefined"
              class="rounded-control border border-border bg-surface px-4 py-3 focus:border-accent focus:outline-none"
            />
          </label>
          <button
            type="submit"
            :disabled="busy"
            class="rounded-control bg-accent px-6 py-3 font-semibold text-on-accent hover:bg-accent/90 disabled:opacity-50"
          >
            {{ busy ? 'Logging in…' : 'Log in' }}
          </button>
          <p
            v-if="error"
            id="login-error"
            role="alert"
            class="rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger"
          >
            {{ error }}
          </p>
        </form>
        <p class="mt-6 text-sm text-ink-muted">
          No account yet?
          <RouterLink :to="registerLink" class="text-accent hover:text-ink-bright">Register</RouterLink>
        </p>
      </template>
    </div>
  </section>
</template>

<script setup lang="ts">
import { computed, nextTick, onMounted, reactive, ref, watch } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import type { SessionErrorCode } from '../api/session';
import { useSession } from '../composables/useSession';
import { safeRedirect } from '../router/guards';

const route = useRoute();
const router = useRouter();
const { user, ready, signIn, signOut } = useSession();

const prefill = route.query.username;
const form = reactive({
  username: typeof prefill === 'string' ? prefill : '',
  password: '',
});
const busy = ref(false);
const signingOut = ref(false);
const error = ref<string | null>(null);
const errorCode = ref<SessionErrorCode | null>(null);
const usernameInput = ref<HTMLInputElement | null>(null);
const passwordInput = ref<HTMLInputElement | null>(null);

// What the page shows: nothing but a status line until /api/me has answered
// (so a logged-in visitor never sees the form flash), then the "logged in"
// view or the form. `busy` keeps the form up while a login lands.
const view = computed<'checking' | 'loggedIn' | 'form'>(() => {
  if (!ready.value && !busy.value) return 'checking';
  return user.value && !busy.value ? 'loggedIn' : 'form';
});

// Only an answer about what was typed marks a field invalid; a 429, a
// network failure or a server error says nothing about the fields.
const usernameInvalid = computed(() =>
  errorCode.value === 'invalid_credentials' ||
  errorCode.value === 'missing_field' ||
  errorCode.value === 'invalid_field',
);
const passwordInvalid = computed(() =>
  errorCode.value === 'invalid_credentials' || errorCode.value === 'missing_field',
);

// Where the login returns the visitor: ?redirect= when it names a page on
// this site, else the home page.
const target = computed(() => safeRedirect(route.query.redirect));
const registerLink = computed(() =>
  target.value === '/' ? '/register' : { path: '/register', query: { redirect: target.value } },
);

// The keyboard goes to the first field to fill whenever the form appears:
// on a cold load once /api/me answers, and after a logout from this page.
function focusFirstField() {
  (form.username ? passwordInput : usernameInput).value?.focus();
}
onMounted(() => {
  if (view.value === 'form') focusFirstField();
});
watch(view, (now) => {
  if (now === 'form') focusFirstField();
}, { flush: 'post' });

async function handleSubmit() {
  if (busy.value) return;
  busy.value = true;
  error.value = null;
  errorCode.value = null;
  const result = await signIn(form.username.trim(), form.password);
  if (result.ok) {
    // `busy` stays set until the page is left, so the form does not flip
    // to the "logged in" view for a frame first.
    await router.replace(target.value);
    busy.value = false;
    return;
  }
  busy.value = false;
  error.value = result.error.message;
  errorCode.value = result.error.code;
  if (result.error.code === 'invalid_credentials') form.password = '';
  await nextTick();
  (result.error.code === 'invalid_field' ? usernameInput : passwordInput).value?.focus();
}

async function handleSignOut() {
  signingOut.value = true;
  error.value = null;
  errorCode.value = null;
  const result = await signOut();
  signingOut.value = false;
  if (!result.ok) error.value = result.error.message;
}
</script>
