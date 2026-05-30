// {hot} accelerator-marker stripping, including position reporting and the
// no-marker / leading / trailing / UTF-8 cases.
#include "rtxt_test_util.h"

using namespace opennova::rtxt;
using rtxt_test::expect;

int main() {
  int idx = -2;

  if (!expect(strip_hotkey("Quit", idx) == "Quit", "plain text should be unchanged")) return 1;
  if (!expect(idx == -1, "plain text should report hotkey index -1")) return 1;

  if (!expect(strip_hotkey("{hot}New Game", idx) == "New Game", "leading marker should be stripped")) return 1;
  if (!expect(idx == 0, "leading marker should report index 0")) return 1;

  if (!expect(strip_hotkey("Save {hot}As", idx) == "Save As", "mid-string marker should be stripped")) return 1;
  if (!expect(idx == 5, "mid-string marker index should be its byte position")) return 1;

  if (!expect(strip_hotkey("Exit{hot}", idx) == "Exit", "trailing marker should be stripped")) return 1;
  if (!expect(idx == 4, "trailing marker should report its position")) return 1;

  // Only the first marker is stripped; a second literal stays in the text.
  if (!expect(strip_hotkey("{hot}A{hot}B", idx) == "A{hot}B", "only the first marker should be stripped")) return 1;
  if (!expect(idx == 0, "first marker index should win")) return 1;

  // UTF-8 bytes before the marker keep their byte offset.
  std::string utf8 = std::string("\xC3\xA9") + "{hot}X";  // "é{hot}X"
  if (!expect(strip_hotkey(utf8, idx) == std::string("\xC3\xA9") + "X", "UTF-8 prefix should survive")) return 1;
  if (!expect(idx == 2, "marker index should be the byte offset past the 2-byte é")) return 1;

  std::printf("OK: rtxt strip_hotkey handles all marker positions and reports byte index\n");
  return 0;
}
