// Unit tests for mnu_xml: forgiving XML parser matching NovaLogic's MNU parser.
#include <cstdlib>
#include <iostream>
#include <string>

#include "mnu_xml/mnu_xml.h"

namespace {

#define CHECK(cond, msg)                                      \
  do {                                                        \
    if (!(cond)) {                                            \
      std::cerr << "FAIL: " << msg << " at line " << __LINE__ \
                << "\n";                                      \
      return false;                                           \
    }                                                         \
  } while (0)

// Test basic element parsing.
bool test_basic_element() {
  const std::string xml = "<ROOT><CHILD>text</CHILD></ROOT>";
  mnu_xml::Document doc;
  std::string err;

  CHECK(mnu_xml::parse(xml, doc, err), "parse failed: " + err);
  CHECK(doc.roots.size() == 1, "expected 1 root");
  CHECK(doc.roots[0]->tag == "ROOT", "expected ROOT tag");
  CHECK(doc.roots[0]->children.size() == 1, "expected 1 child");
  CHECK(doc.roots[0]->children[0]->tag == "CHILD", "expected CHILD tag");
  CHECK(doc.roots[0]->children[0]->get_direct_text() == "text",
        "expected text content");

  return true;
}

// Test quoted attributes.
bool test_quoted_attributes() {
  const std::string xml = R"(<TAG attr1="value1" attr2='value2'/>)";
  mnu_xml::Document doc;
  std::string err;

  CHECK(mnu_xml::parse(xml, doc, err), "parse failed: " + err);
  CHECK(doc.roots.size() == 1, "expected 1 root");
  CHECK(doc.roots[0]->attr("attr1") == "value1", "attr1 mismatch");
  CHECK(doc.roots[0]->attr("attr2") == "value2", "attr2 mismatch");

  return true;
}

// Test unquoted attributes (game's parser allows this).
bool test_unquoted_attributes() {
  const std::string xml = R"(<WINDOW type=button name=SINGLE_PLAYER/>)";
  mnu_xml::Document doc;
  std::string err;

  CHECK(mnu_xml::parse(xml, doc, err), "parse failed: " + err);
  CHECK(doc.roots.size() == 1, "expected 1 root");
  CHECK(doc.roots[0]->attr("type") == "button", "type mismatch");
  CHECK(doc.roots[0]->attr("name") == "SINGLE_PLAYER", "name mismatch");

  return true;
}

// Test bare boolean attributes (game's parser allows this).
bool test_bare_boolean_attributes() {
  const std::string xml = R"(<WINDOW HIDDEN DISABLE/>)";
  mnu_xml::Document doc;
  std::string err;

  CHECK(mnu_xml::parse(xml, doc, err), "parse failed: " + err);
  CHECK(doc.roots.size() == 1, "expected 1 root");
  CHECK(doc.roots[0]->attr_bool("HIDDEN") == true, "HIDDEN should be true");
  CHECK(doc.roots[0]->attr_bool("DISABLE") == true, "DISABLE should be true");
  CHECK(doc.roots[0]->attr_bool("VISIBLE") == false,
        "VISIBLE should be false (not present)");

  return true;
}

// Test case-insensitive matching.
bool test_case_insensitive() {
  const std::string xml = R"(<Root><Child Attr="value">TEXT</Child></Root>)";
  mnu_xml::Document doc;
  std::string err;

  CHECK(mnu_xml::parse(xml, doc, err), "parse failed: " + err);
  CHECK(doc.find_root("ROOT") != nullptr, "case-insensitive root lookup");
  CHECK(doc.find_root("root") != nullptr, "case-insensitive root lookup");

  auto root = doc.find_root("ROOT");
  CHECK(root->find_child("CHILD") != nullptr, "case-insensitive child lookup");
  CHECK(root->find_child("child") != nullptr, "case-insensitive child lookup");

  auto child = root->find_child("child");
  CHECK(child->attr("ATTR") == "value", "case-insensitive attr lookup");
  CHECK(child->attr("attr") == "value", "case-insensitive attr lookup");

  return true;
}

// Test HTML-style comments.
bool test_comments() {
  const std::string xml = R"(
    <!-- This is a comment -->
    <ROOT>
      <!--- Another comment -->
      <CHILD/>
    </ROOT>
  )";
  mnu_xml::Document doc;
  std::string err;

  CHECK(mnu_xml::parse(xml, doc, err), "parse failed: " + err);
  CHECK(doc.roots.size() == 1, "expected 1 root");
  CHECK(doc.roots[0]->find_child("CHILD") != nullptr, "expected CHILD");

  return true;
}

// Test XML entities.
bool test_entities() {
  const std::string xml = R"(<TAG attr="&lt;&gt;&amp;&quot;">text &amp; more</TAG>)";
  mnu_xml::Document doc;
  std::string err;

  CHECK(mnu_xml::parse(xml, doc, err), "parse failed: " + err);
  CHECK(doc.roots[0]->attr("attr") == "<>&\"", "attr entities not decoded");
  CHECK(doc.roots[0]->get_direct_text() == "text & more",
        "text entities not decoded");

  return true;
}

// The faithful entity policy [orig: XML_ParseCharEntity @ 0x769cc0; table
// @ 0x85a628]: decimal-only numerics, NO &apos;, Latin-1 named set, and unknown
// entities preserved verbatim (so a literal "&" or unrecognized "&foo;"
// survives a round-trip instead of being lost).
bool test_entity_policy() {
  mnu_xml::Document doc;
  std::string err;

  // Decimal numeric -> low byte; &#x.. is NOT hex (engine _wtol stops at 'x').
  CHECK(mnu_xml::parse("<T>&#65;&#169;</T>", doc, err), "decimal parse: " + err);
  const std::string dec = doc.roots[0]->get_direct_text();
  CHECK(dec.size() == 2 && dec[0] == 'A' &&
            static_cast<unsigned char>(dec[1]) == 0xA9,
        "&#65; -> 'A', &#169; -> 0xA9 (copyright low byte)");

  // &apos; is NOT a recognized entity: it stays verbatim.
  mnu_xml::Document doc2;
  CHECK(mnu_xml::parse("<T>it&apos;s</T>", doc2, err), "apos parse: " + err);
  CHECK(doc2.roots[0]->get_direct_text() == "it&apos;s",
        "&apos; is preserved verbatim (engine has no apos entry)");

  // An unknown named entity and a bare '&' both survive (no '?' substitution,
  // no infinite loop).
  mnu_xml::Document doc3;
  CHECK(mnu_xml::parse("<T>a &foo; b &c d</T>", doc3, err), "unknown parse: " + err);
  CHECK(doc3.roots[0]->get_direct_text() == "a &foo; b &c d",
        "unknown entity and bare '&' preserved verbatim");

  // Named Latin-1 entity decodes to its byte; nbsp -> 0x20; case matters for
  // the accented set.
  mnu_xml::Document doc4;
  CHECK(mnu_xml::parse("<T>&copy;&nbsp;&Agrave;</T>", doc4, err), "named parse: " + err);
  const std::string named = doc4.roots[0]->get_direct_text();
  CHECK(named.size() == 3 && static_cast<unsigned char>(named[0]) == 0xA9 &&
            named[1] == ' ' && static_cast<unsigned char>(named[2]) == 0xC0,
        "&copy;->0xA9, &nbsp;->space, &Agrave;->0xC0");

  return true;
}

// Test nested elements (like MNU WINDOW hierarchy).
bool test_nested_elements() {
  const std::string xml = R"(
    <SCREEN>
      <WINDOW name="MAIN">
        <WINDOW name="BUTTONS">
          <WINDOW name="BTN1"/>
          <WINDOW name="BTN2"/>
        </WINDOW>
      </WINDOW>
    </SCREEN>
  )";
  mnu_xml::Document doc;
  std::string err;

  CHECK(mnu_xml::parse(xml, doc, err), "parse failed: " + err);

  auto screen = doc.find_root("SCREEN");
  CHECK(screen != nullptr, "expected SCREEN");

  auto main = screen->find_child("WINDOW");
  CHECK(main != nullptr, "expected main WINDOW");
  CHECK(main->attr("name") == "MAIN", "expected MAIN");

  auto buttons = main->find_child("WINDOW");
  CHECK(buttons != nullptr, "expected BUTTONS");
  CHECK(buttons->attr("name") == "BUTTONS", "expected BUTTONS");

  auto all_children = buttons->find_children("WINDOW");
  CHECK(all_children.size() == 2, "expected 2 button windows");

  return true;
}

// Test self-closing tags.
bool test_self_closing() {
  const std::string xml = R"(<ROOT><EMPTY/><ALSO_EMPTY /></ROOT>)";
  mnu_xml::Document doc;
  std::string err;

  CHECK(mnu_xml::parse(xml, doc, err), "parse failed: " + err);
  CHECK(doc.roots[0]->children.size() == 2, "expected 2 children");
  CHECK(doc.roots[0]->find_child("EMPTY") != nullptr, "expected EMPTY");
  CHECK(doc.roots[0]->find_child("ALSO_EMPTY") != nullptr,
        "expected ALSO_EMPTY");

  return true;
}

// Test text content with whitespace normalization.
bool test_whitespace_normalization() {
  const std::string xml = R"(<TAG>  hello   world  </TAG>)";
  mnu_xml::Document doc;
  std::string err;

  CHECK(mnu_xml::parse(xml, doc, err), "parse failed: " + err);
  // With default options (normalize + trim), whitespace is collapsed and
  // trimmed.
  CHECK(doc.roots[0]->get_direct_text() == "hello world",
        "whitespace not normalized");

  return true;
}

// Test UTF-8 BOM handling.
bool test_utf8_bom() {
  // UTF-8 BOM is EF BB BF
  std::string xml = "\xEF\xBB\xBF<ROOT/>";
  mnu_xml::Document doc;
  std::string err;

  CHECK(mnu_xml::parse(xml, doc, err), "parse failed: " + err);
  CHECK(doc.roots.size() == 1, "expected 1 root");
  CHECK(doc.roots[0]->tag == "ROOT", "expected ROOT tag");

  return true;
}

// Test attr_int helper.
bool test_attr_int() {
  const std::string xml = R"(<TAG num="42" invalid="abc" empty=""/>)";
  mnu_xml::Document doc;
  std::string err;

  CHECK(mnu_xml::parse(xml, doc, err), "parse failed: " + err);
  CHECK(doc.roots[0]->attr_int("num") == 42, "attr_int failed");
  CHECK(doc.roots[0]->attr_int("invalid", -1) == -1,
        "attr_int should return default for invalid");
  CHECK(doc.roots[0]->attr_int("empty", -1) == -1,
        "attr_int should return default for empty");
  CHECK(doc.roots[0]->attr_int("missing", 99) == 99,
        "attr_int should return default for missing");

  return true;
}

// Test MNU-style XML patterns.
bool test_mnu_patterns() {
  // This mirrors actual MNU file patterns.
  const std::string xml = R"(
<SCREEN>
  <NAME>STARTUP</NAME>
  <MUSICVAR>1</MUSICVAR>
  <WINDOW type="window" name="MAIN">
    <APPEARANCE type="custom" state="default"></APPEARANCE>
    <POSITION>
      <LEFT>0</LEFT>
      <TOP>75</TOP>
      <RIGHT>800</RIGHT>
      <BOTTOM>525</BOTTOM>
    </POSITION>
    <CURSOR>
      <FILE>newarow1.tga</FILE>
      <FLAGS>STANDARD_TRANSPARENT</FLAGS>
    </CURSOR>
    <FONT>
      <NAME>Gunpl27b.fnt</NAME>
      <DEFAULT_FG>FFFFFF</DEFAULT_FG>
      <MOUSEOVER_FG>FF0000</MOUSEOVER_FG>
    </FONT>
    <WINDOW type="button" name="EXIT" HIDDEN DISABLE>
      <STRING type="id" justify="LEFT">MM_Exit</STRING>
      <SOUND state="mousein" trigger="MOUSE_OVER">menu.lwf</SOUND>
      <ACTION type="screen" file="sp.mnu">SINGLE_PLAYER</ACTION>
    </WINDOW>
  </WINDOW>
</SCREEN>
  )";
  mnu_xml::Document doc;
  std::string err;

  CHECK(mnu_xml::parse(xml, doc, err), "parse failed: " + err);

  auto screen = doc.find_root("SCREEN");
  CHECK(screen != nullptr, "expected SCREEN");

  auto name = screen->find_child("NAME");
  CHECK(name != nullptr, "expected NAME");
  CHECK(name->get_direct_text() == "STARTUP", "expected STARTUP");

  auto musicvar = screen->find_child("MUSICVAR");
  CHECK(musicvar != nullptr, "expected MUSICVAR");
  CHECK(musicvar->get_direct_text() == "1", "expected 1");

  auto main_window = screen->find_child("WINDOW");
  CHECK(main_window != nullptr, "expected WINDOW");
  CHECK(main_window->attr("name") == "MAIN", "expected MAIN");

  auto position = main_window->find_child("POSITION");
  CHECK(position != nullptr, "expected POSITION");

  auto left = position->find_child("LEFT");
  CHECK(left != nullptr, "expected LEFT");
  CHECK(left->get_direct_text() == "0", "expected 0");

  auto cursor = main_window->find_child("CURSOR");
  CHECK(cursor != nullptr, "expected CURSOR");

  auto file = cursor->find_child("FILE");
  CHECK(file != nullptr, "expected FILE");
  CHECK(file->get_direct_text() == "newarow1.tga", "expected newarow1.tga");

  auto font = main_window->find_child("FONT");
  CHECK(font != nullptr, "expected FONT");

  auto font_name = font->find_child("NAME");
  CHECK(font_name != nullptr, "expected font NAME");
  CHECK(font_name->get_direct_text() == "Gunpl27b.fnt",
        "expected Gunpl27b.fnt");

  auto default_fg = font->find_child("DEFAULT_FG");
  CHECK(default_fg != nullptr, "expected DEFAULT_FG");
  CHECK(default_fg->get_direct_text() == "FFFFFF", "expected FFFFFF");

  auto btn_window = main_window->find_child("WINDOW");
  CHECK(btn_window != nullptr, "expected button WINDOW");
  CHECK(btn_window->attr("type") == "button", "expected type=button");
  CHECK(btn_window->attr("name") == "EXIT", "expected name=EXIT");
  CHECK(btn_window->attr_bool("HIDDEN") == true, "expected HIDDEN");
  CHECK(btn_window->attr_bool("DISABLE") == true, "expected DISABLE");

  auto string_el = btn_window->find_child("STRING");
  CHECK(string_el != nullptr, "expected STRING");
  CHECK(string_el->attr("type") == "id", "expected type=id");
  CHECK(string_el->get_direct_text() == "MM_Exit", "expected MM_Exit");

  auto sound = btn_window->find_child("SOUND");
  CHECK(sound != nullptr, "expected SOUND");
  CHECK(sound->attr("state") == "mousein", "expected state=mousein");
  CHECK(sound->get_direct_text() == "menu.lwf", "expected menu.lwf");

  auto action = btn_window->find_child("ACTION");
  CHECK(action != nullptr, "expected ACTION");
  CHECK(action->attr("type") == "screen", "expected type=screen");
  CHECK(action->attr("file") == "sp.mnu", "expected file=sp.mnu");
  CHECK(action->get_direct_text() == "SINGLE_PLAYER",
        "expected SINGLE_PLAYER");

  return true;
}

}  // namespace

int main() {
  int failed = 0;

#define RUN_TEST(name)                     \
  do {                                     \
    std::cout << "Running " #name "... "; \
    if (name()) {                          \
      std::cout << "OK\n";                 \
    } else {                               \
      std::cout << "FAILED\n";             \
      ++failed;                            \
    }                                      \
  } while (0)

  RUN_TEST(test_basic_element);
  RUN_TEST(test_quoted_attributes);
  RUN_TEST(test_unquoted_attributes);
  RUN_TEST(test_bare_boolean_attributes);
  RUN_TEST(test_case_insensitive);
  RUN_TEST(test_comments);
  RUN_TEST(test_entities);
  RUN_TEST(test_entity_policy);
  RUN_TEST(test_nested_elements);
  RUN_TEST(test_self_closing);
  RUN_TEST(test_whitespace_normalization);
  RUN_TEST(test_utf8_bom);
  RUN_TEST(test_attr_int);
  RUN_TEST(test_mnu_patterns);

  if (failed > 0) {
    std::cerr << "\n" << failed << " test(s) FAILED\n";
    return EXIT_FAILURE;
  }

  std::cout << "\nAll tests passed!\n";
  return EXIT_SUCCESS;
}
