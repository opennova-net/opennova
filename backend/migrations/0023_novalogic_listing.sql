-- A hosted server's owner asked for it to be listed on NovaLogic's NovaWorld as well as this
-- service's (apps/novaworld_common/hosted/model/hosted_settings.h HostedSettings::
-- list_on_novalogic): opennova-serve's second listing (--also-master-host), rendered at each
-- start while the service offers it (ONNET_NOVALOGIC_LISTING with the project's NovaLogic
-- account). Service policy, not a port.
ALTER TABLE hosted_servers
    ADD COLUMN list_on_novalogic boolean NOT NULL DEFAULT false;
