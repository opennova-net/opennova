import { createRouter, createWebHistory } from 'vue-router';
import LandingPage from '../pages/LandingPage.vue';
const PlayPage = () => import('../pages/PlayPage.vue');
const DownloadsPage = () => import('../pages/DownloadsPage.vue');
const AssetsPage = () => import('../pages/AssetsPage.vue');
const AccountOverviewPage = () => import('../pages/account/AccountOverviewPage.vue');
const LoginPage = () => import('../pages/LoginPage.vue');
const RegisterPage = () => import('../pages/RegisterPage.vue');
const ForgotPasswordPage = () => import('../pages/ForgotPasswordPage.vue');
const ResetPasswordPage = () => import('../pages/ResetPasswordPage.vue');
const LobbyPage = () => import('../pages/LobbyPage.vue');
const PublicServerPage = () => import('../pages/PublicServerPage.vue');
const AccountLayout = () => import('../pages/account/AccountLayout.vue');
const ServersPage = () => import('../pages/account/ServersPage.vue');
const NewServerPage = () => import('../pages/account/NewServerPage.vue');
const ProjectsPage = () => import('../pages/account/ProjectsPage.vue');
const NewProjectPage = () => import('../pages/account/NewProjectPage.vue');
const ProjectPage = () => import('../pages/account/ProjectPage.vue');
const MyServerStatsPage = () => import('../pages/account/MyServerStatsPage.vue');
const HostedGamePage = () => import('../pages/account/HostedGamePage.vue');
const BillingPage = () => import('../pages/account/BillingPage.vue');
const HostingPage = () => import('../pages/HostingPage.vue');
const NotFoundPage = () => import('../pages/NotFoundPage.vue');
const TermsPage = () => import('../pages/legal/TermsPage.vue');
const PrivacyPage = () => import('../pages/legal/PrivacyPage.vue');
const RefundsPage = () => import('../pages/legal/RefundsPage.vue');
const ServerDetailLayout = () => import('../pages/servers/ServerDetailLayout.vue');
const ServerOverviewTab = () => import('../pages/servers/ServerOverviewTab.vue');
const ServerConsoleTab = () => import('../pages/servers/ServerConsoleTab.vue');
const ServerStatsTab = () => import('../pages/servers/ServerStatsTab.vue');
const ServerLogsTab = () => import('../pages/servers/ServerLogsTab.vue');
const AdminLayout = () => import('../pages/admin/AdminLayout.vue');
const OverviewPage = () => import('../pages/admin/OverviewPage.vue');
const OnlineTab = () => import('../pages/admin/players/OnlineTab.vue');
const AccountsTab = () => import('../pages/admin/players/AccountsTab.vue');
const CdKeysTab = () => import('../pages/admin/players/CdKeysTab.vue');
const PlayerPage = () => import('../pages/admin/players/PlayerPage.vue');
const ServersTab = () => import('../pages/admin/servers/ServersTab.vue');
const MachinesTab = () => import('../pages/admin/servers/MachinesTab.vue');
const RollTab = () => import('../pages/admin/servers/RollTab.vue');
const MachinePage = () => import('../pages/admin/servers/MachinePage.vue');
const ProductsPage = () => import('../pages/admin/products/ProductsPage.vue');
const ProductPage = () => import('../pages/admin/products/ProductPage.vue');
const SubscriptionsTab = () => import('../pages/admin/billing/SubscriptionsTab.vue');
const AddressesTab = () => import('../pages/admin/billing/AddressesTab.vue');
const CostsTab = () => import('../pages/admin/billing/CostsTab.vue');
const NovaWorldTab = () => import('../pages/admin/system/NovaWorldTab.vue');
const InfrastructureTab = () => import('../pages/admin/system/InfrastructureTab.vue');
const JobsTab = () => import('../pages/admin/system/JobsTab.vue');
const JobPage = () => import('../pages/admin/system/JobPage.vue');
const ProgramPage = () => import('../pages/admin/system/ProgramPage.vue');
const ResourcePage = () => import('../pages/admin/system/ResourcePage.vue');
import { requireUser } from './guards';

// A server page's tabs an old /admin/fleet/<id>/<tab> link keeps.
const KEPT_SERVER_TABS = new Set(['stats', 'console', 'logs']);

// A page only a logged-in visitor may open sets `meta: { requiresUser: true }`
// (guards.ts). /admin does not: its AdminGate also takes the Bearer token.
const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', component: LandingPage },
    { path: '/play', component: PlayPage, meta: { title: 'Play' } },
    { path: '/downloads', component: DownloadsPage, meta: { title: 'Downloads' } },
    { path: '/assets', component: AssetsPage, meta: { title: 'Asset library' } },
    { path: '/login', component: LoginPage, meta: { title: 'Log in' } },
    { path: '/register', component: RegisterPage, meta: { title: 'Register' } },
    { path: '/forgot', component: ForgotPasswordPage, meta: { title: 'Forgot password' } },
    // The link a password reset mail carries: /reset?token=<64 hex digits>.
    { path: '/reset', component: ResetPasswordPage, meta: { title: 'Reset password' } },
    { path: '/lobby', component: LobbyPage, meta: { title: 'Servers' } },
    // A server our NovaWorld lists, or one on NovaLogic's (from the service's
    // copy of their list): one page for both.
    { path: '/lobby/:id(\\d+)', component: PublicServerPage, meta: { title: 'Server' } },
    { path: '/lobby/novalogic/:id(\\d+)', component: PublicServerPage, meta: { title: 'Server' } },
    { path: '/lobbies', redirect: '/lobby' },
    { path: '/hosting', component: HostingPage, meta: { title: 'Hosting' } },
    { path: '/support', redirect: '/hosting' },
    { path: '/terms', component: TermsPage, meta: { title: 'Terms of service' } },
    { path: '/privacy', component: PrivacyPage, meta: { title: 'Privacy notice' } },
    { path: '/refunds', component: RefundsPage, meta: { title: 'Refund policy' } },
    // The account area.
    {
      path: '/me',
      component: AccountLayout,
      meta: { requiresUser: true, wide: true },
      children: [
        { path: '', component: AccountOverviewPage, meta: { title: 'Overview' } },
        { path: 'servers', component: ServersPage, meta: { title: 'Servers' } },
        { path: 'servers/stats', component: MyServerStatsPage, meta: { title: 'Server stats' } },
        // A game the account hosts from the game itself, by its listing's id.
        { path: 'servers/hosts/:rid(\\d+)', component: HostedGamePage, meta: { title: 'Server' } },
        { path: 'servers/new', component: NewServerPage, meta: { title: 'New server' } },
        {
          path: 'servers/:id(\\d+)',
          component: ServerDetailLayout,
          props: { scope: 'owner' },
          meta: { title: 'Server', serverDetail: true },
          children: [
            { path: '', component: ServerOverviewTab },
            { path: 'stats', component: ServerStatsTab },
            { path: 'console', component: ServerConsoleTab },
            { path: 'settings', redirect: (to) => `/me/servers/${to.params.id}` },
          ],
        },
        // The account's own mods and games on NovaWorld; a project by its key.
        { path: 'projects', component: ProjectsPage, meta: { title: 'Projects' } },
        { path: 'projects/new', component: NewProjectPage, meta: { title: 'New project' } },
        { path: 'projects/:key', component: ProjectPage, meta: { title: 'Project' } },
        // Stripe's Checkout and billing portal return here
        // (?checkout=success&session_id=..., ?checkout=cancel), the portal's
        // cancel flow for a static address too.
        { path: 'billing', component: BillingPage, meta: { title: 'Billing' } },
        { path: 'support', redirect: (to) => ({ path: '/me/billing', query: to.query }) },
        { path: 'addresses', redirect: { path: '/me/billing', hash: '#addresses' } },
        // Who is logged in shows under the side nav, with the log out.
        { path: 'account', redirect: '/me' },
        // The hosted server's page before an account could have several:
        // old links land on the list.
        { path: 'server', redirect: '/me/servers' },
      ],
    },
    // The admin (utils/adminNav.ts): Overview, Players, Servers, Products,
    // Billing and System, their tabs as child paths, and the pages of one
    // player, server, machine, product, job, program or AWS resource. The
    // service's findings link to these paths (fleet_health.cpp).
    {
      path: '/admin',
      component: AdminLayout,
      meta: { wide: true },
      children: [
        { path: '', component: OverviewPage, meta: { title: 'Admin' } },
        { path: 'players', component: OnlineTab, meta: { title: 'Admin: Players online' } },
        { path: 'players/accounts', component: AccountsTab, meta: { title: 'Admin: Accounts' } },
        { path: 'players/cd-keys', component: CdKeysTab, meta: { title: 'Admin: CD keys' } },
        { path: 'players/:id(\\d+)', component: PlayerPage, meta: { title: 'Admin: Player' } },
        { path: 'servers', component: ServersTab, meta: { title: 'Admin: Servers' } },
        { path: 'servers/machines', component: MachinesTab, meta: { title: 'Admin: Machines' } },
        { path: 'servers/roll', component: RollTab, meta: { title: 'Admin: Roll the fleet' } },
        {
          path: 'servers/:id(\\d+)',
          component: ServerDetailLayout,
          props: { scope: 'admin' },
          meta: { title: 'Admin: Server', serverDetail: true },
          children: [
            { path: '', component: ServerOverviewTab },
            { path: 'stats', component: ServerStatsTab },
            { path: 'console', component: ServerConsoleTab },
            { path: 'logs', component: ServerLogsTab },
          ],
        },
        { path: 'machines/:id(\\d+)', component: MachinePage, meta: { title: 'Admin: Machine' } },
        { path: 'products', component: ProductsPage, meta: { title: 'Admin: Products' } },
        // A product by its project key, or by its id when it has none.
        { path: 'products/:ref', component: ProductPage, meta: { title: 'Admin: Product' } },
        { path: 'billing', component: SubscriptionsTab, meta: { title: 'Admin: Subscriptions' } },
        { path: 'billing/addresses', component: AddressesTab, meta: { title: 'Admin: Static addresses' } },
        { path: 'billing/costs', component: CostsTab, meta: { title: 'Admin: AWS costs' } },
        { path: 'system', component: NovaWorldTab, meta: { title: 'Admin: NovaWorld' } },
        { path: 'system/infrastructure', component: InfrastructureTab, meta: { title: 'Admin: Infrastructure' } },
        { path: 'system/jobs', component: JobsTab, meta: { title: 'Admin: Background jobs' } },
        { path: 'jobs/:id(\\d+)', component: JobPage, meta: { title: 'Admin: Job' } },
        { path: 'programs/:name', component: ProgramPage, meta: { title: 'Admin: Program' } },
        { path: 'resources/:id(rds|cache|nlb)', component: ResourcePage, meta: { title: 'Admin: AWS resource' } },
        // The pages' paths before the admin became six sections: old links
        // and bookmarks land on their new places.
        { path: 'overview', redirect: '/admin' },
        { path: 'users', redirect: '/admin/players/accounts' },
        { path: 'users/:id(\\d+)', redirect: (to) => `/admin/players/${to.params.id}` },
        { path: 'cd-keys', redirect: '/admin/players/cd-keys' },
        { path: 'fleet', redirect: (to) => (to.query.tab === 'roll' ? '/admin/servers/roll' : { path: '/admin/servers', query: to.query }) },
        { path: 'fleet/machines/:id(\\d+)', redirect: (to) => `/admin/machines/${to.params.id}` },
        { path: 'fleet/:id(\\d+)', redirect: (to) => `/admin/servers/${to.params.id}` },
        { path: 'fleet/:id(\\d+)/:tab', redirect: (to) => `/admin/servers/${to.params.id}${KEPT_SERVER_TABS.has(String(to.params.tab)) ? `/${to.params.tab}` : ''}` },
        { path: 'usage', redirect: '/admin/servers' },
        { path: 'settings', redirect: '/admin/servers/machines' },
        { path: 'addresses', redirect: '/admin/billing/addresses' },
        { path: 'costs', redirect: '/admin/billing/costs' },
        { path: 'novaworld', redirect: '/admin/system' },
        { path: 'jobs', redirect: (to) => ({ path: '/admin/system/jobs', query: to.query }) },
        { path: 'infrastructure', redirect: '/admin/system/infrastructure' },
        { path: 'infrastructure/programs/:name', redirect: (to) => `/admin/programs/${to.params.name}` },
        { path: 'infrastructure/resources/:id(rds|cache|nlb)', redirect: (to) => `/admin/resources/${to.params.id}` },
      ],
    },
    // Anything else.
    { path: '/:rest(.*)*', component: NotFoundPage, meta: { title: 'Page not found' } },
  ],
  // Back and Forward return to where the page was; a #section link scrolls to
  // it below the fixed header; a server page's tab change keeps the scroll (the
  // header and the tabs stay put), as does a change of the jobs page's tab or
  // filter (its query); any other navigation starts at the top.
  scrollBehavior(to, from, savedPosition) {
    if (savedPosition) return savedPosition;
    if (to.hash) return { el: to.hash, top: 72 };
    if (to.path === from.path) return false;
    const onServerPage = to.matched.some((record) => record.meta.serverDetail);
    if (onServerPage && to.params.id === from.params.id) return false;
    return { top: 0 };
  },
});

router.beforeEach(requireUser);

// Each page names itself in the tab; the home page keeps the site's title.
const SITE_TITLE = document.title;
router.afterEach((to) => {
  const title = to.meta.title as string | undefined;
  document.title = title ? `${title} | OpenNova` : SITE_TITLE;
});

export default router;
