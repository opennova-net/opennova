-- A settings edit's command the server refused (an ERROR or USAGE reply): the console drain
-- stops the edit there, saves the fields its earlier commands set, and names this one, so the
-- route says which change was refused instead of reading as saved (hosted/model/
-- admin_link_status.h AdminCommandResult::refused_command). '' for every other command.
ALTER TABLE hosted_console_commands ADD COLUMN refused_command text NOT NULL DEFAULT '';
