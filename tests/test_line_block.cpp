// Whole lines in the script editor: clipboard text of marked lines, taking lines out, pasting several lines.
#include "test_framework.h"
#include "line_block.h"

using namespace muisc;
namespace lb = muisc::lineblock;

TEST(lineblock_clean_text) {
    CHECK(lb::clean_text("a\r\nb\rc\n") == "a\nb\nc\n");
    CHECK(lb::clean_text("a\tb\x01") == "a b");
    CHECK(lb::is_multiline("a\nb"));
    CHECK(lb::is_multiline("a\n"));
    CHECK(!lb::is_multiline("abc"));
}

TEST(lineblock_join_and_erase) {
    std::vector<std::string> l = {"one", "two", "three", "four"};
    CHECK(lb::join_lines(l, 1, 2) == "two\nthree\n");
    CHECK(lb::join_lines(l, -5, 99) == "one\ntwo\nthree\nfour\n");
    int at = 0;
    CHECK(lb::erase_lines(l, 1, 2, at) == 2);
    CHECK((l == std::vector<std::string>{"one", "four"}) && at == 1);
    CHECK(lb::erase_lines(l, 1, 1, at) == 1);
    CHECK((l == std::vector<std::string>{"one"}) && at == 0);
    lb::erase_lines(l, 0, 0, at);
    CHECK((l == std::vector<std::string>{""}) && at == 0);   // one empty line is always left
}

TEST(lineblock_paste_whole_lines) {
    std::vector<std::string> l = {"a", "b"};
    int cy = 1; size_t cx = 1;
    CHECK(lb::paste_lines(l, cy, cx, "x\ny\n", 500) == 2);
    CHECK((l == std::vector<std::string>{"a", "x", "y", "b"}) && cy == 3 && cx == 0);   // above the caret's line
    CHECK(lb::paste_lines(l, cy, cx, "single", 500) == -1);   // one line: the caller pastes it into the line
}

TEST(lineblock_paste_in_the_middle_and_limit) {
    std::vector<std::string> l = {"hello world"};
    int cy = 0; size_t cx = 5;
    CHECK(lb::paste_lines(l, cy, cx, "AAA\nBBB\nCCC", 500) == 2);
    CHECK((l == std::vector<std::string>{"helloAAA", "BBB", "CCC world"}) && cy == 2 && cx == 3);
    std::vector<std::string> m = {"1", "2", "3"};
    cy = 0; cx = 0;
    CHECK(lb::paste_lines(m, cy, cx, "a\nb\nc\nd\n", 5) == 2);   // only two lines of room
    CHECK(m.size() == 5);
    cy = 0;
    CHECK(lb::paste_lines(m, cy, cx, "e\nf\n", 5) == 0);          // none
    CHECK(m.size() == 5);
}
