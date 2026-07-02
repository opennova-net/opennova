#pragma once

#include <map>
#include <string>
#include <string_view>

namespace opennova::server {

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
//     value. On miss, leaves the literal `{{KEY}}` text in place
//     (matches net2 behavior; surfaces unset vars during dev).
//   - Does NOT do HTML escaping (templates are pre-trusted; user-supplied
//     vars never injected for Phase E).

using TemplateVars = std::map<std::string, std::string>;

std::string render_template(std::string_view tmpl, const TemplateVars &vars);

// Convenience: read a template file from disk and render it.
// Returns empty string if the file is missing.
std::string render_template_file(const std::string &templates_dir,
                                 const std::string &filename,
                                 const TemplateVars &vars);

} // namespace opennova::server
