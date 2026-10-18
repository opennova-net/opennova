#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace opennova::novaworld_server {

// Tiny `{{VAR}}` substitution renderer for the legacy NW*.dll templates.
//
// Mirrors the approach from worktree-net2/apps/novaworld/src/main.cpp's
// `render_template_text()` (lines 770-801) and onnet's render_template()
// (with the subset of Jinja2 features the templates actually need —
// just simple `{{NAME}}` lookups).
//
// Important: templates contain TWO marker syntaxes:
//   `{{VAR}}`  — server-side; substituted here.
//   `@VAR@`    — IB3 markup; parsed by the GAME CLIENT after download.
// We pass the `@VAR@` markers through verbatim.
//
// Behavior:
//   - Scans for `{{` and matching `}}`.
//   - Trims interior whitespace from the key (e.g. `{{ FOO }}` → key="FOO").
//   - Looks up the key in `vars` (case-sensitive). On hit, substitutes the
//     value, escaped for HTML (html_escape). On miss, leaves the literal
//     `{{KEY}}` text in place (matches net2 behavior; surfaces unset vars
//     during dev).
//
// Every value is text, never markup: the page names a request carries (IN,
// OUT, MSGBASE), a listed server's own name (SERVER_NAME), an admin's
// maintenance message (MESSAGE) and the rest can add no element or attribute
// to the page. No template variable carries markup; one that had to would
// need its own raw opt-in.

using TemplateVars = std::map<std::string, std::string>;

// `text` with each of & < > " ' as its entity (&amp; &lt; &gt; &quot; &#39;)
// and every other byte as it is. In a page's text a stock client's page
// parser turns those entities back into the characters they name [orig:
// XML_DecodeEntityReference @0x655330, the named table at 0x84C1E8 holding
// &quot; &amp; &lt; &gt;, and &#NN; through atol, whose one caller is the
// text loop of CUIHTMLParser_BuildContentNodeFromHTML @0x656080 (the call @0x656161),
// for each '&' outside a tag], so a value there shows as itself. Inside a
// tag (an attribute value, such as the login form's VALUE="{{IN}}") nothing
// decodes them, so an escaped character would arrive as its entity; the
// values that land there are bare page names, which hold none of the five.
// Every value the service fills in for a stock client (A-P keys, session tags,
// page names, numbers, addresses and URLs, names with spaces) holds none of
// the five and goes out byte for byte.
std::string html_escape(std::string_view text);

std::string render_template(std::string_view tmpl, const TemplateVars &vars);

// Whether `name` is a bare file name a request may name: one or more bytes of
// [A-Za-z0-9_.-], not starting with '.'. So no '/', '\', drive colon, "." or
// "..", and no hidden file. Every retail template name (jop_2_msg.htm,
// dfx2_0_join.joi, ...) is one.
bool is_bare_file_name(std::string_view name);

// The regular file `relative` names under `root`, or nullopt. Request-derived
// names reach the disk only through here. `relative` is '/'-separated
// segments, each a bare file name (is_bare_file_name: no empty segment, so no
// absolute path; no "..", no backslash), and the weakly canonical path must
// stay under the canonical `root`, so a symlink that leads out is refused too.
// A name the rule refuses is logged (escaped) and answers as a missing file.
std::optional<std::filesystem::path> file_under_root(
		const std::filesystem::path &root, std::string_view relative);

// The template file `name` names: one bare file name under `templates_dir`
// (file_under_root with no '/'), or nullopt.
std::optional<std::filesystem::path> template_file(
		const std::string &templates_dir, std::string_view name);

// Convenience: read a template file from disk and render it.
// Returns empty string if the file is missing, or if `filename` is not a bare
// file name under `templates_dir` (template_file).
std::string render_template_file(const std::string &templates_dir,
                                 const std::string &filename,
                                 const TemplateVars &vars);

} // namespace opennova::novaworld_server
