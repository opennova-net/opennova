<template>
  <section class="px-4 py-10 sm:px-6 sm:py-14">
    <div class="mx-auto max-w-3xl rounded-panel border border-border bg-panel p-8">
      <h1 class="text-3xl font-semibold text-ink">Register</h1>
      <p class="mt-2 text-sm text-ink-muted">
        Pick a handle and a password. That is what you log in with in-game.
      </p>
      <form class="mt-8 grid gap-6" novalidate @submit.prevent="handleSubmit">
        <label class="grid gap-2 text-sm text-ink">
          Game Handle
          <input v-model="form.username" type="text" required minlength="2" maxlength="30"
                 class="rounded-control border border-ink-muted/60 bg-surface px-4 py-3 focus:border-accent focus:outline-none" />
        </label>
        <label class="grid gap-2 text-sm text-ink">
          Display Name (NW Handle)
          <input v-model="form.nwhandle" type="text" maxlength="30"
                 placeholder="defaults to handle"
                 class="rounded-control border border-ink-muted/60 bg-surface px-4 py-3 focus:border-accent focus:outline-none" />
        </label>
        <label class="grid gap-2 text-sm text-ink">
          Email
          <input ref="emailInput" v-model="form.email" name="email" type="email" required maxlength="254"
                 autocomplete="email" autocapitalize="none" spellcheck="false"
                 :aria-invalid="emailProblem ? 'true' : undefined" aria-describedby="register-email-hint"
                 class="rounded-control border border-ink-muted/60 bg-surface px-4 py-3 focus:border-accent focus:outline-none" />
          <span id="register-email-hint" class="text-xs" :class="emailProblem ? 'text-danger' : 'text-ink-muted'">
            {{ emailProblem || 'We send a welcome mail here, and a password reset link when you ask for one.' }}
          </span>
        </label>
        <label class="grid gap-2 text-sm text-ink">
          Password
          <input v-model="form.password" type="password" required minlength="3" maxlength="72"
                 class="rounded-control border border-ink-muted/60 bg-surface px-4 py-3 focus:border-accent focus:outline-none" />
        </label>
        <button type="submit" :disabled="busy"
                class="rounded-control bg-accent px-6 py-3 font-semibold text-on-accent hover:bg-accent/90 disabled:opacity-50">
          {{ busy ? 'Creating account…' : 'Register Account' }}
        </button>
        <div
          v-if="success"
          role="status"
          class="flex flex-wrap items-center justify-between gap-3 rounded-panel border border-online/40 bg-online/10 p-3 text-sm text-online"
        >
          <p>
            Account created! Username <code class="font-mono">{{ success.username }}</code>, PCID
            <code class="font-mono">{{ success.pcid }}</code>. You can now log in from the game, or log in
            to this site.
            <template v-if="success.email">A welcome mail is on its way to {{ success.email }}.</template>
          </p>
          <RouterLink
            ref="loginOffer"
            :to="loginLink(success.username)"
            class="rounded-control bg-accent px-4 py-2 font-semibold text-on-accent hover:bg-accent/90"
          >
            Log in
          </RouterLink>
        </div>
        <p v-if="error" role="alert" class="rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger">
          {{ error }}
        </p>
      </form>
      <p class="mt-6 text-sm text-ink-muted">
        Already have an account?
        <RouterLink :to="loginLink()" class="link">Log in</RouterLink>
      </p>
    </div>
  </section>
</template>

<script setup lang="ts">
import { nextTick, reactive, ref } from 'vue';
import { RouterLink, useRoute, type RouteLocationRaw } from 'vue-router';
import { registerUser } from '../api/users';
import { safeRedirect } from '../router/guards';
import type { User } from '../types/users';
import { EMAIL_HINT, isEmailAddress } from '../utils/email';

const route = useRoute();

// The login page, carrying on the ?redirect= this page was opened with and,
// after a registration, the new username.
function loginLink(username?: string): RouteLocationRaw {
  const query: Record<string, string> = {};
  const redirect = safeRedirect(route.query.redirect);
  if (redirect !== '/') query.redirect = redirect;
  if (username) query.username = username;
  return { path: '/login', query };
}

const loginOffer = ref<{ $el: HTMLElement } | null>(null);

const form = reactive({ username: '', nwhandle: '', email: '', password: '' });
const busy = ref(false);
const success = ref<User | null>(null);
const error = ref<string | null>(null);
// What is wrong with the email field: the address rule, checked before the
// request, or the server's refusal of it. Shown under the field.
const emailProblem = ref<string | null>(null);
const emailInput = ref<HTMLInputElement | null>(null);

// The server's refusals in words; any other answer shows its own message.
function refusalText(code: string | undefined, message: string | undefined): string | null {
  switch (code) {
    case 'username_exists':
      return 'Another account already has that username.';
    case 'email_exists':
      return 'Another account already uses that email address.';
    case 'missing_field':
      return 'Fill in the username, the email and the password.';
    case 'rate_limited':
      return 'Too many registrations from your address. Try again in a few minutes.';
    default:
      return message || code || null;
  }
}

async function handleSubmit() {
  error.value = null;
  emailProblem.value = null;
  success.value = null;
  const email = form.email.trim();
  if (!isEmailAddress(email)) {
    emailProblem.value = EMAIL_HINT;
    emailInput.value?.focus();
    return;
  }
  busy.value = true;
  try {
    success.value = await registerUser({
      username: form.username.trim(),
      password: form.password,
      nwhandle: form.nwhandle.trim() || undefined,
      email,
    });
    form.username = '';
    form.nwhandle = '';
    form.email = '';
    form.password = '';
    // The next step is the login: put the keyboard on it.
    await nextTick();
    loginOffer.value?.$el.focus();
  } catch (e) {
    const detail = (e as { response?: { data?: { error?: string; message?: string } } })
      ?.response?.data;
    const text = refusalText(detail?.error, detail?.message);
    const aboutEmail =
      detail?.error === 'email_exists' || (detail?.error === 'invalid_field' && /email/i.test(detail?.message ?? ''));
    if (aboutEmail) {
      emailProblem.value = text;
      emailInput.value?.focus();
    } else {
      error.value = text || (e as Error).message || 'Registration failed.';
    }
  } finally {
    busy.value = false;
  }
}
</script>
