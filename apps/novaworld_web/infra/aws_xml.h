#pragma once

// A reader for the XML AWS's Query APIs answer (Elastic Load Balancing, RDS,
// ElastiCache, EC2: aws_api.h, infra_status.h): elements, the text directly
// inside each and its children, nothing more. Attributes are read past (the
// replies carry none but xmlns), a name's namespace prefix is dropped, the
// prolog, comments and a DOCTYPE are skipped, CDATA counts as text, and the five
// named entities and numeric references are decoded. Nesting is capped at
// kMaxXmlDepth; anything malformed (an end tag that does not match, text past
// the root) is refused. Platform plumbing, not a port.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::infra {

inline constexpr size_t kMaxXmlDepth = 64;

struct XmlNode {
	std::string name; // the local name
	std::string text; // the character data directly inside, decoded
	std::vector<XmlNode> children;

	// The first child named `child_name`; nullptr for none.
	const XmlNode *child(std::string_view child_name) const;
	// That child's text, trimmed; "" for none.
	std::string child_text(std::string_view child_name) const;
	// The descendant down `path` (each step the first child so named); nullptr
	// when a step is missing.
	const XmlNode *at(std::initializer_list<std::string_view> path) const;
	// The first element named `element_name` here or below, depth first;
	// nullptr for none.
	const XmlNode *find(std::string_view element_name) const;
};

// `text` read into `root` (its root element); false with `error` when it is no
// well-formed document of elements.
bool parse_xml(std::string_view text, XmlNode &root, std::string &error);

// A Query reply's time, "2026-10-18T12:00:00Z" (a fraction allowed: CloudWatch's
// GetMetricData and DescribeAlarms), as Unix seconds; -1 for text that is none.
int64_t iso_seconds(std::string_view text);

} // namespace opennova::novaworld_server::infra
