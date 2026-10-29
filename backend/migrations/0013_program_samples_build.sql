-- The ONE build each program's task runs, as the task itself says with every minute's sample
-- (apps/novaworld_common/program/program_usage.h): its one_build()'s version ("0.0.22"; a staging
-- build's "0.0.22.123") and its commit's short sha ("a1b2c3d"). The admin's infrastructure page
-- shows them as each program's build (apps/novaworld_web/infra/usage_routes.h). '' in a row a
-- task wrote before the columns, or one built with no stamp. The hourly rollup keeps neither.
ALTER TABLE program_samples ADD COLUMN one_version text NOT NULL DEFAULT '';
ALTER TABLE program_samples ADD COLUMN one_commit text NOT NULL DEFAULT '';
