// The parts seam (common/test_parts.h): no selection runs every part in order; `--part <name>
// --parts <all>` runs that part alone, and only when <all> is the program's own list; a list
// that is not, or a part not in it, fails; the arguments before the selection are left to the
// caller.

#include "common/test_expect.h"
#include "common/test_parts.h"

#include <string>
#include <vector>

namespace {

std::string ran;

int part_a() { ran += "a"; return 0; }
int part_b() { ran += "b"; return 0; }
int part_b2() { ran += "B"; return 2; }
int part_c() { ran += "c"; return 0; }

const std::vector<test_parts::Part> kParts = {
    {"a", {part_a}}, {"b", {part_b, part_b2}}, {"c", {part_c}}};

// The result of running the parts over `args` (argv[0] added), with what ran and the arguments left.
int run(std::vector<std::string> args, int *left = nullptr) {
    args.insert(args.begin(), "test_parts_test");
    std::vector<char *> argv;
    for (std::string &arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    int argc = static_cast<int>(args.size());
    ran.clear();
    const int failures = test_parts::run(argc, argv.data(), kParts);
    if (left) *left = argc;
    return failures;
}

int test_every_part_in_order() {
    TEST_EXPECT(run({}) == 2 && ran == "abBc");
    return 0;
}

int test_one_part_against_the_list() {
    TEST_EXPECT(run({"--part", "b", "--parts", "a", "b", "c"}) == 2 && ran == "bB");
    TEST_EXPECT(run({"--part", "c", "--parts", "a", "b", "c"}) == 0 && ran == "c");
    return 0;
}

int test_a_list_not_the_programs_fails() {
    TEST_EXPECT(run({"--part", "a", "--parts", "a", "b"}) == 1 && ran.empty());
    TEST_EXPECT(run({"--part", "a", "--parts", "b", "a", "c"}) == 1 && ran.empty());
    TEST_EXPECT(run({"--part", "d", "--parts", "a", "b", "c"}) == 1 && ran.empty());
    TEST_EXPECT(run({"--part", "a"}) == 1 && ran.empty());
    return 0;
}

int test_the_callers_arguments_stay() {
    int left = 0;
    TEST_EXPECT(run({"dir", "--part", "a", "--parts", "a", "b", "c"}, &left) == 0 && ran == "a" && left == 2);
    TEST_EXPECT(run({"dir"}, &left) == 2 && ran == "abBc" && left == 2);
    return 0;
}

} // namespace

int main() {
    int failures = 0;
    failures += test_every_part_in_order();
    failures += test_one_part_against_the_list();
    failures += test_a_list_not_the_programs_fails();
    failures += test_the_callers_arguments_stay();
    if (failures == 0) std::printf("test_parts: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
