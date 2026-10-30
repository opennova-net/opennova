#include <editor/session/novaworld_standing.h>

#include <editor/model/field_text.h>
#include <editor/novaworld/novaworld_account.h>
#include <editor/session/view/novaworld_view.h>

namespace opennova::editor {

namespace {

NovaWorldStandingKind kind_of(const NovaWorldView &c) {
	using Kind = NovaWorldStandingKind;
	if (c.key.empty())
		return c.former_key.empty() ? Kind::None : Kind::WasOnNovaWorld;
	if (c.head_missing)
		return Kind::NotFound;
	if (!c.tracked)
		return c.setting_up			 ? Kind::SettingUp
				: !c.head_error.empty() ? Kind::Unreachable
										: Kind::NotSetUp;
	if (!c.conflicts.empty())
		return Kind::Conflicts;
	if (!c.head_known)
		return c.head_error.empty() ? Kind::Unchecked : Kind::Unreachable;
	if (novaworld_holds_no_revision(c))
		return Kind::NoRevision;
	if (c.head > c.revision)
		return Kind::Behind;
	if (!c.changes.empty())
		return Kind::Changed;
	return c.checked ? Kind::UpToDate : Kind::Unchecked;
}

} // namespace

bool novaworld_holds_no_revision(const NovaWorldView &novaworld) {
	return novaworld.tracked && novaworld.revision == 0 && novaworld.head_known && novaworld.head > 0;
}

NovaWorldStanding novaworld_standing(const NovaWorldView &c) {
	using Kind = NovaWorldStandingKind;
	NovaWorldStanding out;
	out.kind = kind_of(c);
	if (out.kind == Kind::None)
		return out;
	if (out.kind == Kind::WasOnNovaWorld) {
		out.line = "Was on " + (c.former_host.empty() ? std::string("NovaWorld") : c.former_host) +
				(c.former_revision ? " at r" + std::to_string(c.former_revision) : std::string()) + ".";
		out.tip = "Rejoin takes this folder back to its project, with nothing downloaded.";
		return out;
	}
	out.changed = c.changes.size();
	out.incoming = c.tracked && c.head_known && c.head > c.revision ? c.head - c.revision : 0;

	const std::string host = novaworld_host(c.server);
	const std::string revision = "r" + std::to_string(c.revision);
	const std::string changed_words = out.changed ? ", " + std::to_string(out.changed) + " changed" : "";
	std::string brief; // the status bar's words after the service
	switch (out.kind) {
		case Kind::None:
		case Kind::WasOnNovaWorld:
			break;
		case Kind::SettingUp:
			out.line = "Setting up...";
			out.tip = "The folder becomes a working copy on " + host + ": its files are then pushed and pulled.";
			brief = "setting up";
			break;
		case Kind::NotSetUp:
			out.line = "Not set up here yet.";
			out.tip = "Check sets the folder up as a working copy on " + host + ".";
			brief = "not set up";
			break;
		case Kind::NotFound:
			out.line = "Not on " + host + ", or private.";
			out.tip = c.head_error;
			brief = "not found";
			break;
		case Kind::Unreachable:
			out.line = c.tracked ? revision + ", NovaWorld not reached" : "NovaWorld not reached";
			out.tip = c.head_error;
			brief = c.tracked ? revision + ", not reached" : "not reached";
			break;
		case Kind::Conflicts:
			out.line = revision + ", " + counted(c.conflicts.size(), "conflict");
			out.tip = "A pull from " + host + " waits for you to settle each one.";
			brief = out.line;
			break;
		case Kind::NoRevision:
			out.line = "No revision here yet: Pull r" + std::to_string(c.head) + " first.";
			out.tip = "The folder's files differ from r" + std::to_string(c.head) + " on " + host +
					": a pull takes its files and stops on each file both hold otherwise, for you to settle. "
					"Push waits until then.";
			brief = "no revision yet";
			break;
		case Kind::Behind:
			out.line = revision + ", r" + std::to_string(c.head) + " on NovaWorld" + changed_words;
			out.tip = "This folder is at " + revision + ", and " + host + " has r" + std::to_string(c.head) + ".";
			brief = revision + changed_words + ", " + std::to_string(out.incoming) + " incoming";
			break;
		case Kind::Unchecked:
			out.line = revision + ", not checked yet";
			out.tip = "The revision this folder was last pulled or pushed at on " + host + ".";
			brief = revision;
			break;
		case Kind::Changed:
			out.line = revision + changed_words;
			out.tip = "The revision this folder was last pulled or pushed at on " + host + ", and your changes since.";
			brief = out.line;
			break;
		case Kind::UpToDate:
			out.line = revision + ", up to date";
			out.tip = "The folder holds " + host + "'s newest revision, with no change of its own.";
			brief = revision;
			break;
	}
	out.status = host.empty() ? brief : host + ": " + brief;
	if (!c.other_copies.empty()) {
		const NovaWorldView::OtherCopy &other = c.other_copies.front();
		out.elsewhere = "At r" + std::to_string(other.revision) + " on " + other.host;
		out.tip += " " + out.elsewhere + ".";
	}
	return out;
}

const char *novaworld_standing_name(NovaWorldStandingKind kind) {
	switch (kind) {
		case NovaWorldStandingKind::None:
			return "none";
		case NovaWorldStandingKind::WasOnNovaWorld:
			return "was_on_novaworld";
		case NovaWorldStandingKind::SettingUp:
			return "setting_up";
		case NovaWorldStandingKind::NotSetUp:
			return "not_set_up";
		case NovaWorldStandingKind::NotFound:
			return "not_found";
		case NovaWorldStandingKind::Unreachable:
			return "unreachable";
		case NovaWorldStandingKind::Conflicts:
			return "conflicts";
		case NovaWorldStandingKind::NoRevision:
			return "no_revision";
		case NovaWorldStandingKind::Behind:
			return "behind";
		case NovaWorldStandingKind::Unchecked:
			return "unchecked";
		case NovaWorldStandingKind::Changed:
			return "changed";
		case NovaWorldStandingKind::UpToDate:
			return "up_to_date";
	}
	return "none";
}

} // namespace opennova::editor
