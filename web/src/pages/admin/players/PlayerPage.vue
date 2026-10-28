<template>
  <div class="space-y-8">
    <PageHeader
      :title="detail?.user.username ?? 'Player'"
      :crumbs="CRUMBS"
      :tabs="detail ? tabs : []"
      refreshable
      :loaded-at="loadedAt"
      :now="now"
      :loading="loading"
      @refresh="load"
    >
      <template v-if="detail" #meta>
        <span v-if="detail.user.nwhandle" class="player-handle text-ink" title="In-game handle">{{ detail.user.nwhandle }}</span>
        <Badge :tone="detail.user.role === 'admin' ? 'info' : 'muted'">{{ detail.user.role === 'admin' ? 'Admin' : 'Player' }}</Badge>
        <Badge :tone="accountStatusTone(detail.user.account_status)">{{ accountStatusLabel(detail.user.account_status) }}</Badge>
        <span v-if="onlineNow" class="player-online inline-flex flex-wrap items-baseline gap-x-2">
          <Badge tone="ok">Online</Badge>
          <ServerLink v-if="onlineNow.server" :name="onlineNow.server.name" :id="onlineNow.server.hosted_server_id" :rid="onlineNow.server.rid" />
          <span v-else>in the lobby</span>
        </span>
        <span v-if="detail.user.email" class="break-all">{{ detail.user.email }}</span>
        <span v-if="detail.user.created_at" class="player-joined">Joined {{ formatDate(detail.user.created_at) }}</span>
        <span class="player-seen">
          Last seen <RelativeTime v-if="detail.user.last_login" :value="detail.user.last_login" :now="now" /><template v-else>never</template>
        </span>
        <span v-if="isSelf">You</span>
      </template>
    </PageHeader>

    <LiveNotices :message="message" :error="error" />

    <p v-if="detail === null && loading" class="text-sm text-ink-muted">Loading…</p>
    <button v-else-if="detail === null" type="button" class="btn-secondary" @click="load">Try again</button>

    <Notice v-if="detail?.capacity?.oversold" id="user-oversold" tone="warn">
      {{ soldCapacityText(detail.capacity) }} New subscriptions are paused.
    </Notice>

    <template v-if="detail && tab === 'overview'">
      <section class="space-y-3" aria-labelledby="player-servers">
        <div class="flex flex-wrap items-baseline justify-between gap-2">
          <h2 id="player-servers" class="section-heading">Servers</h2>
          <div class="flex items-center gap-4 text-sm">
            <HistoryToggle v-model="showServerHistory" :count="replacedServerIds.size" />
            <RouterLink :to="{ path: '/admin/servers', query: { owner: detail.user.username } }" class="link">See in Servers</RouterLink>
          </div>
        </div>
        <div class="table-wrap">
          <table class="data-table">
            <caption class="sr-only">Hosted servers</caption>
            <thead>
              <tr>
                <th scope="col">Server</th>
                <th scope="col">Game</th>
                <th scope="col">State</th>
                <th scope="col">Players</th>
              </tr>
            </thead>
            <tbody>
              <TableState v-if="serversShown.length === 0" :colspan="4" :rows="serversShown" empty="No servers yet." />
              <tr v-for="s in serversShown" :key="s.id" class="player-server">
                <th scope="row"><ServerLink :name="s.settings.game_name" :id="s.id" /></th>
                <td>
                  {{ gameName(s) }}
                  <span v-if="s.own_machine" class="block text-xs text-ink-muted">Dedicated machine</span>
                </td>
                <td class="whitespace-nowrap">
                  <span :class="stateBadgeClass(s.state)">{{ stateLabel(s.state) }}</span>
                  <RelativeTime v-if="s.ended_at" :value="s.ended_at" :now="now" class="ml-2 text-xs text-ink-muted" />
                </td>
                <td class="whitespace-nowrap">{{ s.live && s.listing ? `${s.listing.players} of ${s.listing.max_players}` : '' }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>

      <section class="space-y-3" aria-labelledby="player-projects">
        <h2 id="player-projects" class="section-heading">Projects</h2>
        <div class="table-wrap">
          <table class="data-table">
            <caption class="sr-only">Projects</caption>
            <thead>
              <tr>
                <th scope="col">Project</th>
                <th scope="col">Kind</th>
                <th scope="col">Published</th>
              </tr>
            </thead>
            <tbody>
              <TableState v-if="!projects || projects.length === 0" :colspan="3" :rows="projects" :loading="loading" empty="No projects." />
              <tr v-for="p in projects ?? []" :key="p.id" class="player-project">
                <th scope="row"><ProductLink :product-ref="p.key || p.id" :name="p.display_name" /></th>
                <td>{{ PRODUCT_KINDS[p.kind] ?? p.kind }}</td>
                <td :class="p.published_release ? 'font-mono' : 'text-ink-muted'">{{ p.published_release?.name || 'Not published' }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>

      <section class="space-y-3" aria-labelledby="player-billing">
        <h2 id="player-billing" class="section-heading">Billing</h2>
        <dl class="player-billing grid gap-x-6 gap-y-1.5 text-sm sm:grid-cols-[auto_1fr]">
          <dt class="text-ink-muted">Hosting</dt>
          <dd>{{ entitlementLabel(detail.entitlement) }}</dd>
          <dt class="text-ink-muted">May run</dt>
          <dd>{{ mayRun }}</dd>
          <dt class="text-ink-muted">New servers</dt>
          <dd :class="detail.entitlement.can_start ? '' : 'text-warn'" :title="detail.entitlement.can_start ? undefined : 'No entitlement, or a payment past due'">
            {{ detail.entitlement.can_start ? 'May start' : 'Cannot start' }}
          </dd>
          <dt class="text-ink-muted">Stripe customer</dt>
          <dd>
            <template v-if="detail.billing.stripe_customer_id">
              <a
                :href="stripeDashboardUrl('customers', detail.billing.stripe_customer_id, detail.billing.livemode)"
                target="_blank"
                rel="noopener noreferrer"
                class="entity-link font-mono"
              >{{ detail.billing.stripe_customer_id }}</a>
              <span v-if="!detail.billing.livemode" class="ml-2 text-xs text-ink-muted">test mode</span>
            </template>
            <span v-else class="text-ink-muted">None</span>
          </dd>
        </dl>
        <div v-if="detail.billing.subscriptions.length" class="table-wrap">
          <table class="data-table">
            <caption class="sr-only">Subscriptions</caption>
            <thead>
              <tr>
                <th scope="col">Subscription</th>
                <th scope="col">For</th>
                <th scope="col">Status</th>
                <th scope="col">Period ends</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="sub in detail.billing.subscriptions" :key="sub.stripe_subscription_id">
                <td>
                  <a
                    :href="stripeDashboardUrl('subscriptions', sub.stripe_subscription_id, detail.billing.livemode)"
                    target="_blank"
                    rel="noopener noreferrer"
                    class="entity-link break-all font-mono text-xs"
                  >{{ sub.stripe_subscription_id }}</a>
                </td>
                <td class="whitespace-nowrap">{{ subscriptionItemLabel(sub) }}</td>
                <td class="whitespace-nowrap" :class="sub.status === 'past_due' ? 'text-warn' : ''">
                  {{ subscriptionStatusLabel(sub.status) }}<template v-if="sub.cancel_at_period_end">, ending</template>
                </td>
                <td class="whitespace-nowrap">
                  <RelativeTime v-if="sub.current_period_end" :value="sub.current_period_end" :now="now" />
                  <span v-else class="text-ink-muted">None</span>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>

      <section class="space-y-3" aria-labelledby="player-cd-keys">
        <div class="flex flex-wrap items-baseline justify-between gap-2">
          <h2 id="player-cd-keys" class="section-heading">CD keys</h2>
          <RouterLink
            :to="{ path: '/admin/players/cd-keys', query: { account: String(detail.user.id) }, hash: '#cd-account-heading' }"
            class="link text-sm"
          >
            Manage keys
          </RouterLink>
        </div>
        <div class="table-wrap">
          <table class="data-table">
            <caption class="sr-only">CD keys</caption>
            <thead>
              <tr>
                <th scope="col">Key</th>
                <th scope="col">Game</th>
                <th scope="col">State</th>
                <th scope="col">Machines</th>
              </tr>
            </thead>
            <tbody>
              <TableState v-if="!keys || keys.keys.length === 0" :colspan="4" :rows="keys?.keys ?? null" :loading="loading" empty="No keys." />
              <tr v-for="k in keys?.keys ?? []" :key="k.id" class="player-key">
                <th scope="row" class="font-mono font-normal">{{ k.serial }}</th>
                <td>{{ titleName(k.title) }}</td>
                <td><Badge :tone="keyStateTone(k.state)">{{ keyStateLabel(k.state) }}</Badge></td>
                <td>{{ k.activations }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>

      <section class="space-y-3" aria-labelledby="player-addresses">
        <div class="flex flex-wrap items-baseline justify-between gap-2">
          <h2 id="player-addresses" class="section-heading">Static addresses</h2>
          <RouterLink to="/admin/billing/addresses" class="link text-sm">Manage addresses</RouterLink>
        </div>
        <div class="table-wrap">
          <table class="data-table">
            <caption class="sr-only">Static addresses</caption>
            <thead>
              <tr>
                <th scope="col">Address</th>
                <th scope="col">Endpoint</th>
                <th scope="col">From</th>
                <th scope="col">State</th>
                <th scope="col">Server</th>
              </tr>
            </thead>
            <tbody>
              <TableState v-if="detail.addresses.length === 0" :colspan="5" :rows="detail.addresses" empty="None." />
              <tr v-for="a in detail.addresses" :key="a.id" class="player-address">
                <th scope="row" class="break-all font-mono font-normal">{{ a.hostname }}</th>
                <td class="whitespace-nowrap font-mono text-xs" :class="addressEndpoint(a) ? '' : 'text-ink-muted'">{{ addressEndpoint(a) || 'No IP yet' }}</td>
                <td class="whitespace-nowrap">{{ a.source === 'comp' ? 'Admin grant' : 'Subscription' }}</td>
                <td><span :class="addressStateClass(a.state)">{{ addressStateLabel(a) }}</span></td>
                <td><ServerLink v-if="a.server" :name="a.server.game_name" :id="a.server.id" /></td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>
    </template>

    <template v-if="detail && tab === 'settings'">
      <section class="space-y-3" aria-labelledby="player-profile">
        <h2 id="player-profile" class="section-heading">Profile</h2>
        <form class="grid items-end gap-3 sm:grid-cols-2 lg:grid-cols-3" @submit.prevent="saveProfile">
          <label>
            <span class="field-label">Username</span>
            <input id="player-username" v-model="edit.username" required autocomplete="off" class="field" />
          </label>
          <label>
            <span class="field-label">In-game handle</span>
            <input id="player-handle" v-model="edit.nwhandle" required autocomplete="off" class="field" />
          </label>
          <label>
            <span class="field-label">New password</span>
            <input id="player-password" v-model="edit.password" type="password" autocomplete="new-password" placeholder="Unchanged" class="field" />
          </label>
          <details class="sm:col-span-2 lg:col-span-3">
            <summary class="text-sm text-ink-muted">Advanced identity</summary>
            <div class="mt-3 grid gap-3 sm:grid-cols-2 lg:grid-cols-3">
              <label>
                <span class="field-label">PCID</span>
                <input id="player-pcid" v-model="edit.pcid" autocomplete="off" title="8 hexadecimal digits" class="field font-mono" />
              </label>
              <label>
                <span class="field-label">NWH</span>
                <input id="player-nwh" v-model="edit.nwh" autocomplete="off" class="field font-mono" />
              </label>
            </div>
          </details>
          <div>
            <button type="submit" :disabled="busy" class="btn-primary">Save profile</button>
          </div>
        </form>
      </section>

      <section class="space-y-3" aria-labelledby="player-role">
        <h2 id="player-role" class="section-heading">Role</h2>
        <div class="flex items-center gap-3">
          <Badge :tone="detail.user.role === 'admin' ? 'info' : 'muted'">{{ detail.user.role === 'admin' ? 'Admin' : 'Player' }}</Badge>
          <button type="button" :disabled="busy" class="btn-secondary btn-sm" @click="changeRole(detail.user.role === 'admin' ? 'player' : 'admin')">
            {{ detail.user.role === 'admin' ? 'Make player' : 'Make admin' }}
          </button>
        </div>
      </section>

      <section class="space-y-3" aria-labelledby="player-grant">
        <h2 id="player-grant" class="section-heading">Hosting grant</h2>
        <form class="flex flex-wrap items-end gap-3" title="Free of charge, added to what the account buys" @submit.prevent="saveComp">
          <label>
            <span class="field-label">Shared servers</span>
            <input
              id="user-comp-servers"
              v-model.number="comp"
              type="number"
              min="0"
              :max="COMP_SERVERS_MAX"
              step="1"
              inputmode="numeric"
              :aria-invalid="compProblem ? 'true' : undefined"
              :aria-describedby="compProblem || compOwnProblem ? 'user-comp-problem' : undefined"
              class="field w-24"
            />
          </label>
          <label>
            <span class="field-label">Dedicated machines</span>
            <input
              id="user-comp-own"
              v-model.number="compOwn"
              type="number"
              min="0"
              :max="COMP_OWN_MACHINES_MAX"
              step="1"
              inputmode="numeric"
              :aria-invalid="compOwnProblem ? 'true' : undefined"
              :aria-describedby="compProblem || compOwnProblem ? 'user-comp-problem' : undefined"
              class="field w-24"
            />
          </label>
          <button type="submit" :disabled="busy || compProblem !== '' || compOwnProblem !== '' || !compChanged" class="btn-primary">Save grant</button>
        </form>
        <p v-if="compProblem || compOwnProblem" id="user-comp-problem" class="text-xs text-danger">{{ compProblem || compOwnProblem }}</p>
      </section>

      <section v-if="!addressesOff" class="space-y-3" aria-labelledby="player-comp-address">
        <h2 id="player-comp-address" class="section-heading">Comp static address</h2>
        <form class="flex flex-wrap items-end gap-3" novalidate @submit.prevent="addAddress">
          <label>
            <span class="field-label">Name</span>
            <input
              id="user-comp-address"
              v-model="compAddress"
              type="text"
              autocomplete="off"
              autocapitalize="none"
              spellcheck="false"
              maxlength="32"
              title="One per dedicated machine, free of charge"
              :aria-invalid="compAddressProblem ? 'true' : undefined"
              :aria-describedby="compAddressProblem ? 'user-comp-address-problem' : undefined"
              class="field w-48 font-mono"
            />
          </label>
          <button type="submit" :disabled="busy" class="btn-primary">Add address</button>
        </form>
        <p v-if="compAddressProblem" id="user-comp-address-problem" class="text-xs text-danger">{{ compAddressProblem }}</p>
      </section>

      <section class="space-y-3" aria-labelledby="player-delete">
        <h2 id="player-delete" class="section-heading">Delete account</h2>
        <button type="button" :disabled="busy" class="btn-danger" @click="remove">Delete {{ detail.user.username }}…</button>
      </section>
    </template>
  </div>
</template>

<script setup lang="ts">
// /admin/players/<id>: one account. Overview: its servers, projects, billing,
// CD keys and static addresses, each linked; Settings: its profile, role,
// hosting grant (a cut below what it runs is confirmed first), a comp static
// address, and its deletion. The header says whether it is online now, and
// where it plays.
import { computed, onMounted, reactive, ref, watch } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import HistoryToggle from '../../../components/HistoryToggle.vue';
import RelativeTime from '../../../components/RelativeTime.vue';
import ProductLink from '../../../components/links/ProductLink.vue';
import ServerLink from '../../../components/links/ServerLink.vue';
import Badge from '../../../components/ui/Badge.vue';
import LiveNotices from '../../../components/ui/LiveNotices.vue';
import Notice from '../../../components/ui/Notice.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import TableState from '../../../components/ui/TableState.vue';
import type { Tab } from '../../../components/ui/TabNav.vue';
import { addCompAddress } from '../../../api/adminAddresses';
import { stripeDashboardUrl } from '../../../api/adminBilling';
import { hasAdminToken } from '../../../api/authToken';
import { fetchAccountCdKeys } from '../../../api/cdKeys';
import { fetchOnline } from '../../../api/online';
import { fetchProducts } from '../../../api/products';
import { deleteUser, fetchUser, listUsers, setCompGrants, updateUser } from '../../../api/users';
import { confirmAction } from '../../../composables/useConfirm';
import { useSession } from '../../../composables/useSession';
import { useUnsavedChanges } from '../../../composables/useUnsavedChanges';
import type { CdKeyAccountResponse } from '../../../types/cdKeys';
import type { OnlinePlayer } from '../../../types/online';
import type { Product, ProductKind } from '../../../types/products';
import { COMP_OWN_MACHINES_MAX, COMP_SERVERS_MAX, type Role, type User, type UserDetail, type UserListEntry } from '../../../types/users';
import { addressEndpoint, addressNameProblem, addressStateClass, addressStateLabel } from '../../../utils/addressView';
import type { Crumb } from '../../../utils/adminNav';
import {
  accountStatusLabel,
  accountStatusTone,
  entitlementLabel,
  isActiveAccount,
  serverPlacesLabel,
  soldCapacityText,
} from '../../../utils/adminView';
import { subscriptionItemLabel, subscriptionStatusLabel } from '../../../utils/billingView';
import { formatDate } from '../../../utils/format';
import { gameName, titleName } from '../../../utils/games';
import { replacedServers } from '../../../utils/history';
import { stateBadgeClass, stateLabel } from '../../../utils/hostedState';
import { keyStateLabel, keyStateTone } from './cdKeyView';

const CRUMBS: Crumb[] = [{ label: 'Players', to: '/admin/players/accounts' }];
const PRODUCT_KINDS: Record<ProductKind, string> = { game: 'Game', expansion: 'Expansion', app: 'App' };

const route = useRoute();
const router = useRouter();
const { user: sessionUser, refresh: refreshSession } = useSession();

const tab = computed(() => (route.query.tab === 'settings' ? 'settings' : 'overview'));
const tabs = computed<Tab[]>(() => [
  { label: 'Overview', to: { path: route.path } },
  { label: 'Settings', to: { path: route.path, query: { tab: 'settings' } } },
]);

const detail = ref<UserDetail | null>(null);
const keys = ref<CdKeyAccountResponse | null>(null);
// The products this account owns; null until read.
const projects = ref<Product[] | null>(null);
// Where the account is on NovaWorld now; null while it is not.
const onlineNow = ref<OnlinePlayer | null>(null);
const loading = ref(false);
const loadedAt = ref<number | null>(null);
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

// An ended server its owner started again since is history, shown on request
// (utils/history.ts).
const showServerHistory = ref(false);
const replacedServerIds = computed(() => replacedServers(detail.value?.servers ?? []));
const serversShown = computed(() => (detail.value?.servers ?? []).filter((s) => showServerHistory.value || !replacedServerIds.value.has(s.id)));

// "2 servers: 2 on shared machines", "0 servers".
const mayRun = computed(() => {
  const e = detail.value?.entitlement;
  if (!e) return '';
  const count = `${e.max_servers} server${e.max_servers === 1 ? '' : 's'}`;
  return e.max_servers > 0 ? `${count}: ${serverPlacesLabel(e)}` : count;
});

const id = computed(() => Number(route.params.id));
const isSelf = computed(() => sessionUser.value?.id === detail.value?.user.id);

// "" when `value` is a whole number from 0 to `max`, else what to enter.
function countProblem(value: number | string, max: number): string {
  const n = typeof value === 'number' ? value : Number(String(value).trim());
  if (String(value).trim() === '' || !Number.isInteger(n) || n < 0 || n > max) return `Enter a whole number from 0 to ${max}.`;
  return '';
}
const compProblem = computed(() => countProblem(comp.value, COMP_SERVERS_MAX));
const compOwnProblem = computed(() => countProblem(compOwn.value, COMP_OWN_MACHINES_MAX));
const compChanged = computed(
  () => !!detail.value && (Number(comp.value) !== detail.value.user.comp_servers || Number(compOwn.value) !== detail.value.user.comp_own_machines),
);
useUnsavedChanges(() => {
  const u = detail.value?.user;
  return (
    !!u &&
    (edit.username !== u.username ||
      edit.nwhandle !== u.nwhandle ||
      edit.pcid !== u.pcid ||
      edit.nwh !== u.nwh ||
      edit.password !== '' ||
      compChanged.value ||
      (!addressesOff.value && compAddress.value.trim() !== ''))
  );
});
const compAddressProblem = computed(() => (compAddressTried.value ? addressNameProblem(compAddress.value.trim()) : ''));

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

// The account, and beside it its keys, its projects and whether it is
// online: each of those stands alone, so one failing leaves the rest shown.
async function load() {
  const account = id.value;
  loading.value = true;
  const [result, k, o, p] = await Promise.all([fetchUser(account), fetchAccountCdKeys(account), fetchOnline(), fetchProducts()]);
  loading.value = false;
  now.value = Date.now();
  if (!result.ok) {
    // Another account's page shows nothing of the last one.
    if (detail.value && detail.value.user.id !== account) detail.value = null;
    fail(result.error.code === 'not_found' ? 'No account has this id.' : result.error.message);
    return;
  }
  detail.value = result.value;
  adopt(result.value.user);
  loadedAt.value = now.value;
  error.value = '';
  keys.value = k.ok ? k.value : null;
  onlineNow.value = o.ok ? (o.value.players.find((x) => x.account?.id === account) ?? null) : null;
  projects.value = p.ok ? p.value.products.filter((x) => x.owner_id === account) : null;
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
// accounts as the service has them now: a demotion that leaves no admin who
// can sign in says so.
function roleChangePrompt(u: User, role: Role, list: UserListEntry[]): string {
  if (role === 'admin') return `Make ${u.username} an admin? They can manage every account and the service.`;
  const first =
    sessionUser.value?.id === u.id
      ? hasAdminToken.value
        ? 'Remove your own admin role? These pages stay open only while your admin token is valid.'
        : 'Remove your own admin role? These pages close at once.'
      : `Make ${u.username} a player? They lose the admin pages at once.`;
  const others = list.filter((x) => x.role === 'admin' && x.id !== u.id);
  if (others.length === 0) return `${first} No other account is an admin: only the admin token or a bootstrap admin can make one again.`;
  if (!others.some((x) => isActiveAccount(x.account_status))) return `${first} No other admin can sign in: only the admin token can make one again.`;
  return first;
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
    if (!(await confirmAction(roleChangePrompt(current, role, list.value)))) return;
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

// What a cut of dedicated machines does to the `over` newest servers on them:
// `moving` of them move to a shared machine (one each into the owner's free
// shared servers, while the fleet has room), the rest stop.
function dedicatedCutText(over: number, moving: number): string {
  if (moving === 0) return over === 1 ? 'Their newest server on one stops.' : `Their ${over} newest servers on them stop.`;
  if (moving === over) {
    return over === 1
      ? 'Their newest server on one moves to a shared machine, restarting its match, or stops if the fleet is full.'
      : `Their ${over} newest servers on them move to a shared machine, restarting their matches, or stop if the fleet is full.`;
  }
  return `Of their ${over} newest servers on them, ${moving} move${moving === 1 ? 's' : ''} to a shared machine and the rest stop.`;
}

async function saveComp() {
  if (!detail.value || busy.value || compProblem.value || compOwnProblem.value) return;
  const n = Number(comp.value);
  const own = Number(compOwn.value);
  const u = detail.value.user;
  const e = detail.value.entitlement;
  // Its live servers in each pool, oldest first, as the service keeps them.
  // A stopping one still holds its place on shared machines, and no cut
  // counts it.
  const live = detail.value.servers.filter((s) => s.live).sort((a, b) => a.id - b.id);
  const shared = live.filter((s) => !s.own_machine);
  const sharedRunning = shared.filter((s) => s.state !== 'stopping');
  const ownRunning = live.filter((s) => s.own_machine && s.state !== 'stopping');
  const warnings: string[] = [];
  // Its servers on shared machines after the change: the grant and the
  // tier's, added up. The newest there past it stop.
  const sharedAfter = n + e.tier_servers;
  const sharedOver = sharedRunning.length - sharedAfter;
  if (sharedOver > 0) {
    const what = sharedOver === 1 ? 'Their newest server there stops, ending its match.' : `Their ${sharedOver} newest servers there stop, ending their matches.`;
    warnings.push(`Lower ${u.username}'s shared servers to ${sharedAfter}? ${what}`);
  }
  // Its dedicated machines after it: the ones it pays for (no tier needed)
  // and the grant's. The newest servers past them move to a shared machine
  // while it has a shared server free, else stop.
  const ownAfter = e.own_machines - e.comp_own_machines + own;
  const ownOver = ownRunning.length - ownAfter;
  if (ownOver > 0) {
    const moving = Math.min(ownOver, Math.max(0, sharedAfter - shared.length));
    warnings.push(`Lower ${u.username}'s dedicated machines to ${ownAfter}? ${dedicatedCutText(ownOver, moving)}`);
  }
  if (warnings.length > 0 && !(await confirmAction(warnings.join('\n\n')))) return;
  busy.value = true;
  const result = await setCompGrants(u.id, { servers: n, own_machines: own });
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  // The sold capacity with the grant counted: the warning follows it at once.
  if (detail.value && result.value.capacity) detail.value = { ...detail.value, capacity: result.value.capacity };
  const granted = [
    ...(n > 0 ? [`${n} shared server${n === 1 ? '' : 's'}`] : []),
    ...(own > 0 ? [`${own} dedicated machine${own === 1 ? '' : 's'}`] : []),
  ];
  flash(granted.length === 0 ? `${u.username} has no grant now.` : `${u.username}'s grant is now ${granted.join(' and ')}.`);
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
  flash(`${u.username} has the comp address ${result.value.hostname}.`);
  await load();
}

async function remove() {
  if (!detail.value || busy.value) return;
  const u = detail.value.user;
  if (!(await confirmAction(`Delete ${u.username}? This cannot be undone.`))) return;
  busy.value = true;
  const result = await deleteUser(u.id);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  detail.value = null;
  await router.push({ path: '/admin/players/accounts' });
}

// Another account's page in the same component (a route left for another
// page has no id: nothing to read).
watch(id, (next) => {
  if (Number.isInteger(next) && next > 0) void load();
});
onMounted(load);
</script>
