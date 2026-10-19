<template>
  <div class="space-y-10">
    <AdminCrumbs />

    <div role="status" :class="message ? 'rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online' : 'sr-only'">{{ message }}</div>
    <div role="alert" :class="error ? 'rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger' : 'sr-only'">{{ error }}</div>

    <div v-if="detail === null && loading" class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">
      Loading the account…
    </div>
    <div v-else-if="detail === null" class="space-y-3 rounded-panel border border-border bg-panel p-6">
      <h1 class="text-xl font-semibold text-ink">The account could not be loaded</h1>
      <button type="button" :class="secondaryButton" @click="load">Try again</button>
    </div>

    <template v-else>
      <header class="space-y-2">
        <h1 class="break-words text-3xl font-semibold text-ink">{{ detail.user.username }}</h1>
        <p class="flex flex-wrap items-center gap-2 text-sm text-ink-muted">
          <span>Account {{ detail.user.id }}</span>
          <span v-if="detail.user.email" class="break-all">{{ detail.user.email }}</span>
          <span :class="detail.user.role === 'admin' ? `${badge} bg-accent/15 text-accent` : `${badge} bg-hover text-ink-muted`">
            {{ detail.user.role === 'admin' ? 'Admin' : 'Player' }}
          </span>
          <span :class="isActiveAccount(detail.user.account_status) ? 'text-ink' : 'text-warn'">
            {{ accountStatusLabel(detail.user.account_status) }}
          </span>
          <span v-if="isSelf" class="text-xs">(you)</span>
        </p>
      </header>

      <!-- Profile, and its edit. -->
      <section class="space-y-4" aria-labelledby="user-profile">
        <h2 id="user-profile" class="text-xl font-semibold text-ink">Profile</h2>
        <form class="grid gap-4 rounded-panel border border-border bg-panel p-4 sm:grid-cols-2 sm:p-6" @submit.prevent="saveProfile">
          <label class="space-y-1">
            <span :class="labelClass">Username</span>
            <input v-model="edit.username" required autocomplete="off" :class="inputClass" />
          </label>
          <label class="space-y-1">
            <span :class="labelClass">NovaWorld handle</span>
            <input v-model="edit.nwhandle" required autocomplete="off" aria-describedby="user-handle-hint" :class="inputClass" />
            <span id="user-handle-hint" class="block text-xs text-ink-muted">The player's name in the game.</span>
          </label>
          <label class="space-y-1">
            <span :class="labelClass">PCID</span>
            <input v-model="edit.pcid" autocomplete="off" aria-describedby="user-pcid-hint" :class="[inputClass, 'font-mono']" />
            <span id="user-pcid-hint" class="block text-xs text-ink-muted">The player's NovaWorld id, 8 hexadecimal digits.</span>
          </label>
          <label class="space-y-1">
            <span :class="labelClass">NWH value</span>
            <input v-model="edit.nwh" autocomplete="off" aria-describedby="user-nwh-hint" :class="[inputClass, 'font-mono']" />
            <span id="user-nwh-hint" class="block text-xs text-ink-muted">A field retail clients carry with the handle; usually 1.</span>
          </label>
          <label class="space-y-1 sm:col-span-2">
            <span :class="labelClass">New password</span>
            <input
              v-model="edit.password"
              type="password"
              autocomplete="new-password"
              aria-describedby="user-password-hint"
              :class="inputClass"
            />
            <span id="user-password-hint" class="block text-xs text-ink-muted">Leave it empty to keep the password.</span>
          </label>
          <div class="flex justify-end sm:col-span-2">
            <button type="submit" :disabled="busy" :class="primaryButton">Save profile</button>
          </div>
        </form>
      </section>

      <!-- Role. -->
      <section class="space-y-3" aria-labelledby="user-role">
        <h2 id="user-role" class="text-xl font-semibold text-ink">Role</h2>
        <p class="text-sm text-ink-muted">
          {{ detail.user.role === 'admin' ? 'An admin manages every account and the service.' : 'A player has no admin pages.' }}
        </p>
        <button type="button" :disabled="busy" :class="secondaryButton" @click="changeRole(detail.user.role === 'admin' ? 'player' : 'admin')">
          {{ detail.user.role === 'admin' ? 'Make player' : 'Make admin' }}
        </button>
      </section>

      <!-- Hosting entitlement, with the comp grant. -->
      <section class="space-y-4" aria-labelledby="user-hosting">
        <h2 id="user-hosting" class="text-xl font-semibold text-ink">Hosting entitlement</h2>
        <dl class="grid gap-x-6 gap-y-2 rounded-panel border border-border bg-panel p-4 text-sm sm:grid-cols-[auto_1fr]">
          <dt class="text-ink-muted">May run at once</dt>
          <dd class="text-ink">{{ detail.entitlement.max_servers }} hosted server{{ detail.entitlement.max_servers === 1 ? '' : 's' }}</dd>
          <dt class="text-ink-muted">From</dt>
          <dd class="text-ink">{{ entitlementLabel(detail.entitlement) }}</dd>
          <dt class="text-ink-muted">Live now</dt>
          <dd class="text-ink">{{ liveCount }}</dd>
          <dt class="text-ink-muted">Own machines</dt>
          <dd class="text-ink">{{ detail.entitlement.own_machines }}</dd>
          <dt class="text-ink-muted">New servers</dt>
          <dd :class="detail.entitlement.can_start ? 'text-ink' : 'text-warn'">
            {{ detail.entitlement.can_start ? 'May start' : 'Cannot start (no entitlement, or a payment past due)' }}
          </dd>
        </dl>
        <p
          v-if="detail.capacity?.oversold"
          id="user-oversold"
          class="rounded-panel border border-warn/40 bg-warn/10 p-4 text-sm text-warn"
        >
          {{ oversoldText(detail.capacity) }}
        </p>
        <form class="space-y-2" @submit.prevent="saveComp">
          <div class="flex flex-wrap items-end gap-4">
            <div class="space-y-1">
              <label for="user-comp-servers" :class="labelClass">Admin grant (comp servers)</label>
              <input
                id="user-comp-servers"
                v-model.number="comp"
                type="number"
                min="0"
                :max="COMP_SERVERS_MAX"
                step="1"
                inputmode="numeric"
                aria-describedby="user-comp-hint"
                :aria-invalid="compProblem ? 'true' : undefined"
                :class="numberClass"
              />
            </div>
            <div class="space-y-1">
              <label for="user-comp-own" :class="labelClass">Own machines</label>
              <input
                id="user-comp-own"
                v-model.number="compOwn"
                type="number"
                min="0"
                :max="COMP_OWN_MACHINES_MAX"
                step="1"
                inputmode="numeric"
                aria-describedby="user-comp-hint"
                :aria-invalid="compOwnProblem ? 'true' : undefined"
                :class="numberClass"
              />
            </div>
            <button
              type="submit"
              :disabled="busy || compProblem !== '' || compOwnProblem !== '' || !compChanged"
              :class="primaryButton"
            >
              Save grant
            </button>
          </div>
          <p id="user-comp-hint" class="text-xs" :class="compProblem || compOwnProblem ? 'text-danger' : 'text-ink-muted'">
            {{
              compProblem ||
              compOwnProblem ||
              `0 to ${COMP_SERVERS_MAX} free hosted servers, and 0 to ${COMP_OWN_MACHINES_MAX} own machines for them. The account gets the larger of the servers and its tier's, and the own machines on top of any it pays for; lowering them stops its newest servers first.`
            }}
          </p>
        </form>
      </section>

      <!-- Hosted servers. -->
      <section class="space-y-3" aria-labelledby="user-servers">
        <div class="flex flex-wrap items-baseline justify-between gap-2">
          <h2 id="user-servers" class="text-xl font-semibold text-ink">Hosted servers</h2>
          <HistoryToggle v-model="showServerHistory" :count="replacedServerIds.size" />
        </div>
        <p v-if="detail.servers.length === 0" class="text-sm text-ink-muted">None yet.</p>
        <ul v-else class="divide-y divide-border rounded-panel border border-border bg-panel">
          <li v-for="s in serversShown" :key="s.id" class="flex flex-wrap items-center justify-between gap-2 px-4 py-3 text-sm">
            <RouterLink :to="`/admin/fleet/${s.id}`" class="font-medium text-accent hover:text-ink-bright">
              {{ s.settings.game_name || 'Unnamed server' }}
            </RouterLink>
            <span class="flex items-center gap-2 text-ink-muted">
              <span :class="stateBadgeClass(s.state)">{{ stateLabel(s.state) }}</span>
              <span v-if="s.own_machine" class="text-xs">Own machine</span>
              <RelativeTime v-if="s.ended_at" :value="s.ended_at" :now="now" />
            </span>
          </li>
        </ul>
      </section>

      <!-- Static addresses, with an admin's comp address. -->
      <section class="space-y-3" aria-labelledby="user-addresses">
        <h2 id="user-addresses" class="text-xl font-semibold text-ink">Static addresses</h2>
        <p v-if="detail.addresses.length === 0" class="text-sm text-ink-muted">None.</p>
        <ul v-else class="divide-y divide-border rounded-panel border border-border bg-panel">
          <li v-for="a in detail.addresses" :key="a.id" class="flex flex-wrap items-center justify-between gap-2 px-4 py-3 text-sm">
            <span class="min-w-0">
              <span class="break-all font-mono text-ink">{{ a.hostname }}</span>
              <span class="block text-xs text-ink-muted">
                {{ addressEndpoint(a) || 'No IP yet' }}, {{ a.source === 'comp' ? 'an admin grant' : 'its own subscription' }}
              </span>
            </span>
            <span class="flex items-center gap-2 text-ink-muted">
              <span :class="addressStateClass(a.state)">{{ addressStateLabel(a) }}</span>
              <RouterLink v-if="a.server" :to="`/admin/fleet/${a.server.id}`" class="text-accent hover:text-ink-bright">
                {{ a.server.game_name || `Server ${a.server.id}` }}
              </RouterLink>
            </span>
          </li>
        </ul>
        <form v-if="!addressesOff" class="space-y-2" novalidate @submit.prevent="addAddress">
          <label for="user-comp-address" :class="labelClass">Comp address</label>
          <div class="flex flex-wrap items-center gap-2">
            <input
              id="user-comp-address"
              v-model="compAddress"
              type="text"
              autocomplete="off"
              autocapitalize="none"
              spellcheck="false"
              maxlength="32"
              aria-describedby="user-comp-address-hint"
              :aria-invalid="compAddressProblem ? 'true' : undefined"
              :class="[fieldClass, 'w-48 font-mono']"
            />
            <button type="submit" :disabled="busy" :class="primaryButton">Add comp address</button>
          </div>
          <p id="user-comp-address-hint" class="text-xs" :class="compAddressProblem ? 'text-danger' : 'text-ink-muted'">
            {{
              compAddressProblem ||
              `A static address free of charge, one for each of the account's own machines; it counts as sold. ${ADDRESS_NAME_RULE}`
            }}
          </p>
        </form>
        <RouterLink to="/admin/addresses" class="inline-block text-sm text-accent hover:text-ink-bright">
          Rename or release addresses on the Static addresses page
        </RouterLink>
      </section>

      <!-- Billing ids. -->
      <section class="space-y-3" aria-labelledby="user-billing">
        <h2 id="user-billing" class="text-xl font-semibold text-ink">Billing</h2>
        <p v-if="!detail.billing.stripe_customer_id" class="text-sm text-ink-muted">No Stripe customer: this account has never checked out.</p>
        <template v-else>
          <p class="text-sm text-ink">
            Stripe customer
            <a
              :href="stripeDashboardUrl('customers', detail.billing.stripe_customer_id, detail.billing.livemode)"
              target="_blank"
              rel="noopener noreferrer"
              class="font-mono text-accent hover:text-ink-bright"
            >{{ detail.billing.stripe_customer_id }}</a>
            <span class="text-ink-muted"> ({{ detail.billing.livemode ? 'live' : 'test' }} mode)</span>
          </p>
          <div v-if="detail.billing.subscriptions.length" class="relative overflow-x-auto rounded-panel border border-border bg-panel">
            <table class="min-w-full divide-y divide-border text-sm">
              <caption class="sr-only">Subscriptions</caption>
              <thead>
                <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
                  <th scope="col" class="px-4 py-2">Subscription</th>
                  <th scope="col" class="px-4 py-2">For</th>
                  <th scope="col" class="px-4 py-2">Status</th>
                  <th scope="col" class="px-4 py-2">Period ends</th>
                </tr>
              </thead>
              <tbody class="divide-y divide-border text-ink">
                <tr v-for="sub in detail.billing.subscriptions" :key="sub.stripe_subscription_id">
                  <td class="px-4 py-2">
                    <a
                      :href="stripeDashboardUrl('subscriptions', sub.stripe_subscription_id, detail.billing.livemode)"
                      target="_blank"
                      rel="noopener noreferrer"
                      class="break-all font-mono text-xs text-accent hover:text-ink-bright"
                    >{{ sub.stripe_subscription_id }}</a>
                  </td>
                  <td class="whitespace-nowrap px-4 py-2">{{ subscriptionItemLabel(sub) }}</td>
                  <td class="whitespace-nowrap px-4 py-2">
                    {{ subscriptionStatusLabel(sub.status) }}<template v-if="sub.cancel_at_period_end">, ends at the period's end</template>
                  </td>
                  <td class="whitespace-nowrap px-4 py-2">
                    <RelativeTime v-if="sub.current_period_end" :value="sub.current_period_end" :now="now" />
                    <span v-else class="text-ink-muted">None</span>
                  </td>
                </tr>
              </tbody>
            </table>
          </div>
        </template>
      </section>

      <!-- CD keys. -->
      <section class="space-y-3" aria-labelledby="user-cd-keys">
        <h2 id="user-cd-keys" class="text-xl font-semibold text-ink">CD keys</h2>
        <p v-if="keys === null" class="text-sm text-ink-muted">{{ keysError || 'Loading the keys…' }}</p>
        <p v-else-if="keys.keys.length === 0" class="text-sm text-ink-muted">No key on this account.</p>
        <ul v-else class="divide-y divide-border rounded-panel border border-border bg-panel">
          <li v-for="k in keys.keys" :key="k.id" class="flex flex-wrap items-center justify-between gap-2 px-4 py-2 text-sm">
            <span class="font-mono text-ink">{{ k.serial }}</span>
            <span class="text-ink-muted">
              {{ k.state.charAt(0).toUpperCase() + k.state.slice(1) }}, {{ k.activations }} machine{{ k.activations === 1 ? '' : 's' }} activated
            </span>
          </li>
        </ul>
        <RouterLink to="/admin/cd-keys" class="inline-block text-sm text-accent hover:text-ink-bright">Manage keys on the CD keys page</RouterLink>
      </section>

      <!-- Delete. -->
      <section class="space-y-3 rounded-panel border border-danger/40 p-4" aria-labelledby="user-delete">
        <h2 id="user-delete" class="text-lg font-semibold text-danger">Delete the account</h2>
        <p class="text-sm text-ink-muted">
          The account goes for good. One whose support tier still grants hosting cannot be deleted until the tier
          ends.
        </p>
        <button type="button" :disabled="busy" class="rounded-control border border-danger px-4 py-2 text-sm font-medium text-danger transition hover:bg-danger/10 disabled:opacity-40" @click="remove">
          Delete {{ detail.user.username }}…
        </button>
      </section>
    </template>
  </div>
</template>

<script setup lang="ts">
// /admin/users/<id>: one account (GET /api/admin/users/<id>): its profile and
// edit (PUT /api/admin/users/<id>), its role (a demotion of the last admin
// who can log in says how one comes back), its hosting entitlement with the
// admin's comp grants (PUT .../comp: servers and own machines, 0 to 16 each)
// and a warning while grants and subscriptions oversell the fleet (the
// service's sold capacity, also in the grant's reply), its hosted servers
// (each to its fleet page), its static addresses with a comp one (POST
// .../addresses), its Stripe ids (to Stripe's dashboard, in the key's mode),
// its CD keys (GET .../cd-keys) and its deletion (409 active_subscription
// while a tier grants).
import AdminCrumbs from '../../components/AdminCrumbs.vue';
import { computed, onMounted, reactive, ref, watch } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import RelativeTime from '../../components/RelativeTime.vue';
import HistoryToggle from '../../components/HistoryToggle.vue';
import { hasAdminToken } from '../../api/authToken';
import { addCompAddress } from '../../api/adminAddresses';
import { stripeDashboardUrl } from '../../api/adminBilling';
import { fetchAccountCdKeys } from '../../api/cdKeys';
import { deleteUser, fetchUser, listUsers, setCompGrants, updateUser } from '../../api/users';
import { useSession } from '../../composables/useSession';
import type { CdKeyAccountResponse } from '../../types/cdKeys';
import {
  COMP_OWN_MACHINES_MAX,
  COMP_SERVERS_MAX,
  type Role,
  type SoldCapacity,
  type User,
  type UserDetail,
  type UserListEntry,
} from '../../types/users';
import { replacedServers } from '../../utils/history';
import {
  ADDRESS_NAME_RULE,
  addressEndpoint,
  addressNameProblem,
  addressStateClass,
  addressStateLabel,
} from '../../utils/addressView';
import { accountStatusLabel, entitlementLabel, isActiveAccount } from '../../utils/adminView';
import { subscriptionItemLabel, subscriptionStatusLabel } from '../../utils/billingView';
import { stateBadgeClass, stateLabel } from '../../utils/hostedState';

const badge = 'inline-flex rounded-control px-2 py-0.5 text-xs font-semibold';
const labelClass = 'block text-xs font-medium uppercase tracking-wider text-ink-muted';
const fieldClass =
  'rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none aria-[invalid=true]:border-danger';
const inputClass = `w-full ${fieldClass}`;
const numberClass = `w-24 ${fieldClass}`;
const primaryButton =
  'rounded-control bg-accent px-4 py-2 text-sm font-medium text-on-accent transition hover:bg-accent/90 disabled:cursor-not-allowed disabled:opacity-50';
const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const route = useRoute();
const router = useRouter();
const { user: sessionUser, refresh: refreshSession } = useSession();

const detail = ref<UserDetail | null>(null);
// An ended server its owner started again since is history, shown on request
// (utils/history.ts).
const showServerHistory = ref(false);
const replacedServerIds = computed(() => replacedServers(detail.value?.servers ?? []));
const serversShown = computed(() =>
  (detail.value?.servers ?? []).filter((s) => showServerHistory.value || !replacedServerIds.value.has(s.id)),
);
const keys = ref<CdKeyAccountResponse | null>(null);
const keysError = ref('');
const loading = ref(false);
const busy = ref(false);
const message = ref('');
const error = ref('');
const now = ref(Date.now());
const comp = ref<number | string>(0);
const compOwn = ref<number | string>(0);
const compAddress = ref('');
const compAddressTried = ref(false);
// The service sells no static address (it answered addresses_disabled): the
// comp address form goes.
const addressesOff = ref(false);
const edit = reactive({ username: '', nwhandle: '', pcid: '', nwh: '', password: '' });

const id = computed(() => Number(route.params.id));
const isSelf = computed(() => sessionUser.value?.id === detail.value?.user.id);
const liveCount = computed(() => detail.value?.servers.filter((s) => s.live).length ?? 0);

// "" when `value` is a whole number from 0 to `max`, else what to enter.
function countProblem(value: number | string, max: number): string {
  const n = typeof value === 'number' ? value : Number(String(value).trim());
  if (String(value).trim() === '' || !Number.isInteger(n) || n < 0 || n > max) {
    return `Enter a whole number from 0 to ${max}.`;
  }
  return '';
}
const compProblem = computed(() => countProblem(comp.value, COMP_SERVERS_MAX));
const compOwnProblem = computed(() => countProblem(compOwn.value, COMP_OWN_MACHINES_MAX));
const compChanged = computed(
  () =>
    !!detail.value &&
    (Number(comp.value) !== detail.value.user.comp_servers || Number(compOwn.value) !== detail.value.user.comp_own_machines),
);
const compAddressProblem = computed(() => (compAddressTried.value ? addressNameProblem(compAddress.value.trim()) : ''));

// What the service has sold past the fleet, item by item.
function oversoldText(c: SoldCapacity): string {
  return (
    'Admin grants and subscriptions exceed what the fleet sells: ' +
    `shared places ${c.shared.sold} of ${c.shared.sellable}, ` +
    `own machines ${c.own_machines.sold} of ${c.own_machines.sellable}, ` +
    `addresses ${c.addresses.sold} of ${c.addresses.sellable}. New subscriptions of those items are paused.`
  );
}

function flash(text: string) {
  message.value = text;
  error.value = '';
}

function fail(text: string) {
  error.value = text;
  message.value = '';
}

// The profile form and the grant follow the account as read.
function adopt(user: User) {
  if (!detail.value) return;
  detail.value = { ...detail.value, user };
  Object.assign(edit, { username: user.username, nwhandle: user.nwhandle, pcid: user.pcid, nwh: user.nwh, password: '' });
  comp.value = user.comp_servers;
  compOwn.value = user.comp_own_machines;
}

async function load() {
  loading.value = true;
  const result = await fetchUser(id.value);
  loading.value = false;
  now.value = Date.now();
  if (!result.ok) {
    fail(result.error.code === 'not_found' ? 'No account has this id.' : result.error.message);
    return;
  }
  detail.value = result.value;
  adopt(result.value.user);
  const k = await fetchAccountCdKeys(id.value);
  if (k.ok) keys.value = k.value;
  else keysError.value = k.error.message;
}

async function saveProfile() {
  if (!detail.value || busy.value) return;
  busy.value = true;
  const payload: Record<string, string> = {
    username: edit.username.trim(),
    nwhandle: edit.nwhandle.trim(),
    pcid: edit.pcid.trim(),
    nwh: edit.nwh.trim(),
  };
  if (edit.password) payload.password = edit.password;
  const result = await updateUser(detail.value.user.id, payload);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  adopt(result.value);
  flash('Profile saved.');
}

// What the admin confirms before a role change, judged on `list`, the
// accounts as the server has them now. Demoting one's own account says the
// session loses these pages at once (unless a valid admin token is also
// held). Demoting the last admin who can log in says how one comes back:
// ONNET_BOOTSTRAP_ADMIN promotes only while no account holds the admin role,
// so a banned or restricted admin left behind blocks it and only the token
// remains.
function roleChangePrompt(u: User, role: Role, list: UserListEntry[]): string {
  if (role === 'admin') return `Make ${u.username} an admin? An admin can manage every account and the service.`;
  const lines: string[] = [];
  if (sessionUser.value?.id === u.id) {
    lines.push(`You are about to remove your own admin role (${u.username}).`);
    lines.push(
      hasAdminToken.value
        ? 'Your session loses admin access at once; these pages stay open only if the stored admin token is valid.'
        : 'Your session loses admin access at once, and these pages close.',
    );
  } else {
    lines.push(`Make ${u.username} a player? Their session loses admin access at once.`);
  }
  const otherAdmins = list.filter((x) => x.role === 'admin' && x.id !== u.id);
  const inactive = otherAdmins.filter((x) => !isActiveAccount(x.account_status));
  if (otherAdmins.length === inactive.length) {
    if (inactive.length === 0) {
      lines.push(
        'No other account is an admin. Only the ADMIN_API_TOKEN, or ONNET_BOOTSTRAP_ADMIN at the next server start, can make one again.',
      );
    } else {
      const names = inactive.map((x) => `${x.username} (${accountStatusLabel(x.account_status).toLowerCase()})`).join(', ');
      lines.push(
        `No other admin can log in: ${names} ${inactive.length === 1 ? 'holds' : 'hold'} the admin role but ${inactive.length === 1 ? 'is' : 'are'} not active. ` +
          'While any account holds the role, ONNET_BOOTSTRAP_ADMIN does nothing, so only the ADMIN_API_TOKEN can make a working admin again.',
      );
    }
  }
  return lines.join('\n\n');
}

async function changeRole(role: Role) {
  if (!detail.value || busy.value) return;
  busy.value = true;
  try {
    const list = await listUsers();
    if (!list.ok) {
      fail(list.error.message);
      return;
    }
    const current = list.value.find((x) => x.id === detail.value!.user.id);
    if (!current) {
      fail('This account no longer exists.');
      return;
    }
    if (current.role === role) {
      flash(`${current.username} is already ${role === 'admin' ? 'an admin' : 'a player'}.`);
      return;
    }
    if (!window.confirm(roleChangePrompt(current, role, list.value))) return;
    const self = sessionUser.value?.id === current.id;
    const result = await updateUser(current.id, { role });
    if (!result.ok) {
      fail(result.error.message);
      return;
    }
    adopt(result.value);
    flash(`${result.value.username} is now ${result.value.role === 'admin' ? 'an admin' : 'a player'}.`);
    if (self) {
      // The gate follows the session's role: without the token, a demoted
      // admin's pages close here; with one, the next read finds out whether
      // it is valid.
      await refreshSession();
      if (hasAdminToken.value) await load();
    }
  } finally {
    busy.value = false;
  }
}

async function saveComp() {
  if (!detail.value || busy.value || compProblem.value || compOwnProblem.value) return;
  const n = Number(comp.value);
  const own = Number(compOwn.value);
  const u = detail.value.user;
  // The entitlement after the change is the larger of the grant and the tier's
  // servers, whichever gives it now; the limit counts the live servers not
  // already stopping.
  const after = Math.max(n, detail.value.entitlement.tier_servers);
  const kept = detail.value.servers.filter((s) => s.live && s.state !== 'stopping').length;
  const over = kept - after;
  if (over > 0) {
    const what = over === 1 ? 'its newest server' : `its ${over} newest servers`;
    if (!window.confirm(`Lower ${u.username}'s grant to ${n}? They run ${kept} servers live, so the service stops ${what}: their matches end for everyone on them.`)) {
      return;
    }
  }
  busy.value = true;
  const result = await setCompGrants(u.id, { servers: n, own_machines: own });
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  // The sold capacity with the grant counted: the warning follows it at once.
  if (detail.value && result.value.capacity) detail.value = { ...detail.value, capacity: result.value.capacity };
  const machines = own === 0 ? '' : `, ${own} on an own machine`;
  flash(
    n === 0 && own === 0
      ? `${u.username} has no admin grant now.`
      : `${u.username} now has ${n} hosted server${n === 1 ? '' : 's'}${machines} free of charge.`,
  );
  await load();
}

async function addAddress() {
  if (!detail.value || busy.value) return;
  compAddressTried.value = true;
  const name = compAddress.value.trim();
  if (addressNameProblem(name)) return;
  const u = detail.value.user;
  busy.value = true;
  const result = await addCompAddress(u.id, name);
  busy.value = false;
  if (!result.ok) {
    if (result.error.code === 'addresses_disabled') addressesOff.value = true;
    fail(result.error.message);
    return;
  }
  compAddress.value = '';
  compAddressTried.value = false;
  flash(`${u.username} has the comp address ${result.value.hostname}; a worker sets it up now.`);
  await load();
}

async function remove() {
  if (!detail.value || busy.value) return;
  const u = detail.value.user;
  if (!window.confirm(`Delete ${u.username}? This cannot be undone.`)) return;
  busy.value = true;
  const result = await deleteUser(u.id);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  await router.push({ path: '/admin/users' });
}

// Another account's page in the same component (a route left for another
// page has no id: nothing to read).
watch(id, (next) => {
  if (Number.isInteger(next) && next > 0) void load();
});
onMounted(load);
</script>
