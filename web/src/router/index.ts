import { createRouter, createWebHistory } from 'vue-router';
import LandingPage from '../pages/LandingPage.vue';
import RegisterPage from '../pages/RegisterPage.vue';
import LobbyPage from '../pages/LobbyPage.vue';
import ExpansionsPage from '../pages/ExpansionsPage.vue';
import AdminExpansionsPage from '../pages/AdminExpansionsPage.vue';
import AdminUsersPage from '../pages/AdminUsersPage.vue';
import ModToolsPage from '../pages/ModToolsPage.vue';
import Revx02Page from '../pages/expansions/Revx02Page.vue';
import Onjo01Page from '../pages/expansions/Onjo01Page.vue';
import Ondx01Page from '../pages/expansions/Ondx01Page.vue';

const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', component: LandingPage },
    { path: '/register', component: RegisterPage },
    { path: '/lobby', component: LobbyPage },
    { path: '/expansions', component: ExpansionsPage },
    { path: '/expansions/revx02', component: Revx02Page },
    { path: '/expansions/onjo01', component: Onjo01Page },
    { path: '/expansions/ondx01', component: Ondx01Page },
    { path: '/mod-tools', component: ModToolsPage },
    { path: '/admin', component: AdminExpansionsPage },
    { path: '/admin/expansions', component: AdminExpansionsPage },
    { path: '/admin/users', component: AdminUsersPage }
  ],
  scrollBehavior() {
    return { top: 0 };
  }
});

export default router;
