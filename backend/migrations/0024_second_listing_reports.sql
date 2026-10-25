-- A slot's report gains its running server's second listing, as its agent last read it
-- (hosted/protocol/agent_protocol.h SecondListingStatus, opennova-serve's --second-listing-file):
-- a hosted server's listing on NovaLogic's NovaWorld, for an owner who asked
-- (hosted_servers.list_on_novalogic, ADR 0068), so its page shows whether and why it is listed
-- there. Its state (null without a report: none asked, or an agent or server from before it),
-- the try under way or the next of the run's most and a retry's time, the reason a retry waits
-- or the listing ended with its NWEC tag ('' for none), and the PlayerList slots of the players
-- that service sent, comma-separated ('' for none). Service policy, not a port.
ALTER TABLE fleet_slot_reports
    ADD COLUMN second_listing_state text
        CHECK (second_listing_state IN ('off', 'trying', 'listed', 'retrying', 'not_listed')),
    ADD COLUMN second_listing_try bigint,
    ADD COLUMN second_listing_tries bigint,
    ADD COLUMN second_listing_next_try_at timestamptz,
    ADD COLUMN second_listing_reason text NOT NULL DEFAULT '',
    ADD COLUMN second_listing_tag text NOT NULL DEFAULT '',
    ADD COLUMN second_listing_slots text NOT NULL DEFAULT '';
