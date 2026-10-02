// Unit tests for mnu_xml: the structural translation of retail's reader
// [orig: NapiXML_ParseElementTree @ 0x769d70; XML_ParseCharEntity @ 0x769cc0].
// Each case pins one rule of docs/mnu/menu-re.md "The reader" (the 2026-09-23 grill,
// set B2); the corpus differential against the skeptic's port is mnu_compat.
#include <climits>
#include <cstdlib>
#include <iostream>
#include <string>

#include <formats/mnu/mnu_xml.h>

namespace {

using opennova::mnu_xml::Document;
using opennova::mnu_xml::Node;
using opennova::mnu_xml::Text;

#define CHECK(cond, msg)                                                   \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::cerr << "FAIL: " << msg << " at line " << __LINE__ << "\n";     \
      return false;                                                        \
    }                                                                      \
  } while (0)

Text W(const char *ascii) { return opennova::mnu_xml::widen(ascii); }

bool read(const char *xml, Document &doc, std::string &err) {
  return opennova::mnu_xml::parse(W(xml), doc, err);
}

std::string token_of(const Node &n, const char *name) {
  Text tok;
  const auto *a = n.attr(name);
  if (!a || !a->token(tok)) return "<none>";
  return opennova::mnu_xml::ascii(tok);
}

// No trim and no collapse; the text on both sides of a child element is joined.
bool test_text_kept_whole() {
  Document doc;
  std::string err;
  CHECK(read("<A> x <B>y\r\n z</B> w </A>", doc, err), err);
  const Node &a = *doc.roots[0];
  CHECK(a.text == W(" x  w "), "text around the child joined, untrimmed");
  CHECK(a.children.size() == 1 && a.children[0]->text == W("y\r\n z"), "no collapse");
  // Whitespace between top-level elements is skipped; anything else there fails the file.
  CHECK(read("  \r\n<A/>\t", doc, err), "whitespace at the top level");
  CHECK(!read("<A></A>x", doc, err) && doc.roots.empty(), "text at the top level fails");
  return true;
}

// A quoted value ends at the first quote of either kind and keeps it; every consumer
// takes the first run of non-'"' characters (wcstok), so a single-quoted keyword keeps
// its quote and never matches.
bool test_attribute_values_raw() {
  Document doc;
  std::string err;
  CHECK(read("<A a=\"x y\" b='z' c=\"m'n\" d=\"&amp;\" e=unq f=g\"h>", doc, err), err);
  const Node &a = *doc.roots[0];
  CHECK(a.attributes.size() == 6, "six attributes");
  CHECK(a.attr("a")->value == W("x y\""), "the closing quote is kept");
  CHECK(token_of(a, "a") == "x y", "the token drops it");
  CHECK(token_of(a, "b") == "z'", "a single-quoted value keeps its closing quote");
  CHECK(a.attr("c")->value == W("m'"), "either quote ends the value");
  CHECK(token_of(a, "d") == "&amp;", "no entity decoding in attributes");
  CHECK(token_of(a, "e") == "unq", "an unquoted value ends at whitespace");
  CHECK(token_of(a, "f") == "g", "an unquoted value ends at '\"'");
  Text none;
  CHECK(read("<A x=\"\"/>", doc, err) && !doc.roots[0]->attributes[0].token(none), "an empty value has no token");
  return true;
}

// A bare attribute has no value; '=' must follow the name; whitespace before '>' adds a
// nameless attribute; the first authored duplicate is what a whole-list walk keeps.
bool test_attribute_forms() {
  Document doc;
  std::string err;
  CHECK(read("<A HIDDEN n = \"x\" t=\"1\" t=\"2\" >", doc, err), err);
  const Node &a = *doc.roots[0];
  CHECK(a.attr("HIDDEN") && !a.attr("HIDDEN")->has_value, "bare");
  CHECK(a.attr("n") && !a.attr("n")->has_value, "a spaced '=' leaves the name bare");
  // "n = \"x\"": a bare n, a nameless attribute with an empty value (the '=' then the
  // space ends it), then a bare attribute named "x" with its quotes.
  CHECK(a.attributes.size() == 7, "the '=' pieces and the space before '>' are attributes");
  CHECK(a.attributes[2].name.empty() && a.attributes[2].has_value && a.attributes[2].value.empty(),
        "a nameless attribute");
  CHECK(a.attributes[3].name == W("\"x\"") && !a.attributes[3].has_value, "the quoted text as a name");
  CHECK(a.attributes.back().name.empty() && !a.attributes.back().has_value, "whitespace before '>'");
  CHECK(token_of(a, "t") == "1", "attr(): the first authored");
  Text tok;
  CHECK(a.last_attr("t")->token(tok) && tok == W("2"), "last_attr(): the last authored");
  CHECK(token_of(a, "T") == "1", "names ignore case");
  return true;
}

// A tag name runs to whitespace or '>': <X/> names an element "X/"; nothing closes
// by itself; any close tag pops one level, its name never compared, and its
// attributes go to the element created last.
bool test_element_grammar() {
  Document doc;
  std::string err;
  CHECK(read("<A><X/><Y a=\"1\"/><Z></Q></W></B></A>", doc, err), err);
  const Node &a = *doc.roots[0];
  CHECK(a.children.size() == 1 && a.children[0]->tag == W("X/"), "<X/> is an element named X/");
  const Node &x = *a.children[0];
  CHECK(x.children.size() == 1 && x.children[0]->tag == W("Y"), "X/ stays open");
  const Node &y = *x.children[0];
  CHECK(token_of(y, "a") == "1" && y.children.size() == 1, "<Y a=\"1\"/> is Y, still open");
  CHECK(doc.roots.size() == 1, "</Q></W></B></A> pop four levels by count");
  CHECK(read("<A><B></B c=\"1\"></A>", doc, err), err);
  CHECK(token_of(*doc.roots[0]->children[0], "c") == "1", "a close tag's attribute lands on the last element");
  CHECK(read("< A >x</ A>", doc, err) && doc.roots[0]->tag == W("A"), "whitespace after '<' is skipped");
  // Declarations are elements too, and never close: they hold the rest of the file.
  CHECK(read("<?xml version=\"1.0\"?>\r\n<SCREEN></SCREEN>", doc, err), err);
  CHECK(doc.roots.size() == 1 && doc.roots[0]->tag == W("?xml") && doc.roots[0]->children.size() == 1,
        "the declaration holds the SCREEN");
  CHECK(!read("</A>", doc, err), "a close tag at the top level (retail crashes)");
  return true;
}

// A comment's terminator search starts where the tag name stopped, so <!--x--> with no
// whitespace runs on to the next "-->".
bool test_comments() {
  Document doc;
  std::string err;
  CHECK(read("<A><!-- one <B> --><C/></A>", doc, err), err);
  CHECK(doc.roots[0]->children.size() == 1 && doc.roots[0]->children[0]->tag == W("C/"), "a comment");
  CHECK(read("<A><!--x--><B></B><!-- y --><C></C></A>", doc, err), err);
  CHECK(doc.roots[0]->children.size() == 1 && doc.roots[0]->children[0]->tag == W("C"),
        "<!--x--> swallows through the next -->");
  return true;
}

// [orig: XML_ParseCharEntity @ 0x769cc0] Entities in element text only: the named
// table matched whole (the first seven ignoring case), no &apos;, an unknown one is a
// bare '&'; "&#" is decimal with no ';' needed, its low byte sign-extended.
bool test_entities() {
  Document doc;
  std::string err;
  CHECK(read("<A>&QUOT;&amp;&lt;&GT;&nbsp;&copy;&Eacute;&eacute;</A>", doc, err), err);
  const Text want = {0x22, 0x26, 0x3c, 0x3e, 0x20, 0xa9, 0xc9, 0xe9};
  CHECK(doc.roots[0]->text == want, "the named table");
  CHECK(read("<A>&EACUTE; &apos; &foo; &amp</A>", doc, err), err);
  CHECK(doc.roots[0]->text == W("&EACUTE; &apos; &foo; &amp"), "case-sensitive Latin-1, no apos, unknown, no ';'");
  CHECK(read("<A>&#65;&#66 C&#x41;&#233;&#-23;</A>", doc, err), err);
  const Text numeric = {'A', 'B', 'C', 0xFFE9, 0xFFE9};
  CHECK(doc.roots[0]->text == numeric, "decimal, no ';' (the space eaten), hex is 0, sign-extended low byte");
  CHECK(read("<R><A>x&#65</A><B>y;</B></A>", doc, err), err);
  CHECK(doc.roots[0]->children.size() == 1 && doc.roots[0]->children[0]->text == W("xA"),
        "a numeric entity swallows markup up to ';'");
  CHECK(!read("&amp;<A/>", doc, err), "an entity at the top level (retail crashes)");
  return true;
}

// <RAW_TEXT> (case-sensitive, a prefix) appends its body to the current element's text
// verbatim, the first character before the terminator check (so an empty one runs on).
bool test_raw_text() {
  Document doc;
  std::string err;
  CHECK(read("<A>x<RAW_TEXT>&amp; <b></RAW_TEXT>y</A>", doc, err), err);
  CHECK(doc.roots[0]->text == W("x&amp; <b>y") && doc.roots[0]->children.empty(), "the body is text");
  CHECK(read("<A><raw_text>z</raw_text></A>", doc, err) && doc.roots[0]->children.size() == 1,
        "raw_text is an ordinary element");
  CHECK(!read("<A><RAW_TEXT></RAW_TEXT></A>", doc, err), "an empty RAW_TEXT runs to the end");
  CHECK(!read("<RAW_TEXT>x</RAW_TEXT>", doc, err), "RAW_TEXT outside any element");
  return true;
}

// The end of the text: inside a tag name the file fails (E_FAIL); inside the
// attributes retail hangs, the reader stops with a note and keeps what it read.
bool test_end_of_text() {
  Document doc;
  std::string err;
  CHECK(!read("<A><B", doc, err), "inside a tag name");
  CHECK(read("<A><B x=\"1\"", doc, err), "inside the attributes");
  CHECK(doc.notes.size() == 1 && doc.notes[0].message.find("hangs") != std::string::npos, "the hang is noted");
  CHECK(doc.roots.size() == 1 && doc.roots[0]->children.size() == 1, "what was read is kept");
  CHECK(read("<A x=\"1", doc, err) && doc.notes.size() == 1, "inside a quoted value");
  CHECK(read("<A>\n<B>\n</B><!-- open", doc, err) && doc.notes.size() == 1, "inside a comment");
  CHECK(doc.roots[0]->children[0]->line == 2, "lines count from 1");
  const Text with_nul = {'<', 'A', '>', 'x', 0, 'y', '<', '/', 'A', '>'};
  CHECK(opennova::mnu_xml::parse(with_nul, doc, err) && doc.roots[0]->text == W("x"), "a NUL ends the text");
  return true;
}

// The CRT primitives [orig: CRT_iswctype @ 0x77f5b1; CRT_wcsicmp @ 0x77085b;
// CRT_wcstoxl @ 0x76e93b; CRT_wchartodigit @ 0x77fd43].
bool test_crt_primitives() {
  using namespace opennova::mnu_xml;
  CHECK(is_space(0x20) && is_space(0x0B) && is_space(0xA0) && !is_space(0x85) && is_space(0x3000),
        "iswspace: the CRT table and the Unicode spaces");
  CHECK(iequals(W("Static"), "STATIC") && !iequals(W("STATICS"), "STATIC"), "whole-word, ASCII fold");
  const Text accented = {0xC9, 'D', 'I', 'T'};
  CHECK(!iequals(accented, "\xE9" "DIT"), "only A-Z fold");
  CHECK(wcstol(W("  -42x"), 10) == -42 && wcstol(W("x"), 10) == 0 && wcstol(W("+7"), 10) == 7, "wcstol");
  CHECK(wcstol(W("99999999999"), 10) == LONG_MAX || wcstol(W("99999999999"), 10) == 0x7FFFFFFF, "saturates");
  CHECK(wcstol(W("-99999999999"), 10) == -0x7FFFFFFF - 1, "saturates low");
  CHECK(wcstoul(W("0xFF00FF00"), 16) == 0xFF00FF00UL && wcstoul(W("#FF"), 16) == 0, "wcstoul base 16, '#' rejected");
  CHECK(wcstoul(W("FF00FFzz"), 16) == 0xFF00FFUL, "a partial prefix is kept");
  const Text fullwidth = {0xFF11, 0xFF12};
  CHECK(wcstol(fullwidth, 10) == 12, "CRT_wchartodigit's Unicode digits");
  return true;
}

// The shared path key: a top-level SCREEN numbered, a WINDOW named.
bool test_path_key() {
  Document doc;
  std::string err;
  CHECK(read("<SCREEN></SCREEN><X></X><SCREEN><WINDOW NAME=\"main\" type=\"static\"><STRING trigger=\"a\"></STRING>"
             "</WINDOW></SCREEN>",
             doc, err),
        err);
  const Node &string = *doc.roots[2]->children[0]->children[0];
  CHECK(opennova::mnu_xml::path_key(string) == "/SCREEN[1]/WINDOW[MAIN]/STRING", "the element key");
  CHECK(opennova::mnu_xml::path_key(string, &string.attributes[0]) == "/SCREEN[1]/WINDOW[MAIN]/STRING@TRIGGER",
        "the attribute key");
  return true;
}

}  // namespace

int main() {
  int failed = 0;
#define RUN_TEST(name)                     \
  do {                                     \
    if (!name()) {                         \
      std::cerr << #name << " FAILED\n";   \
      ++failed;                            \
    } else {                               \
      std::cout << #name << " passed\n";   \
    }                                      \
  } while (0)

  RUN_TEST(test_text_kept_whole);
  RUN_TEST(test_attribute_values_raw);
  RUN_TEST(test_attribute_forms);
  RUN_TEST(test_element_grammar);
  RUN_TEST(test_comments);
  RUN_TEST(test_entities);
  RUN_TEST(test_raw_text);
  RUN_TEST(test_end_of_text);
  RUN_TEST(test_crt_primitives);
  RUN_TEST(test_path_key);

  if (failed > 0) {
    std::cerr << failed << " test(s) failed\n";
    return 1;
  }
  std::cout << "All mnu_xml tests passed\n";
  return 0;
}
