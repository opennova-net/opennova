-- The credentials generation: one counter per account that every login's
-- grants are tied to, so a password reset or a ban ends them all, the ones a
-- login still in flight would grant included.
--
-- players.credentials_gen moves (+1) with every admin update that sets a
-- password or a status other than 'active' (apps/novaworld_server/auth.cpp,
-- update_user), in the statement that makes the change. A login reads it with
-- the password hash, before bcrypt runs, and what it grants is written only
-- while the account still has it (and, for the two sessions, is active),
-- tested by the insert itself: the website session (create_web_session), the
-- retail login's active_user_sessions row (register_active_user_session) and a
-- known login address (record_login_address). So a reset that commits while a
-- password is being checked lets that login grant nothing.
--
-- active_user_sessions.credentials_gen is the generation its retail login was
-- made under. Its LOGINSESSIONTAG names the account to /NWJoin.dll and
-- /NWAccount.dll only while the two agree (get_user_by_login_session_tag), and
-- the PERSISTENTEXPRESSLOGINDATA pins (in memory) carry theirs the same way.

ALTER TABLE players ADD COLUMN credentials_gen INTEGER NOT NULL DEFAULT 0;
ALTER TABLE active_user_sessions ADD COLUMN credentials_gen INTEGER NOT NULL DEFAULT 0;
