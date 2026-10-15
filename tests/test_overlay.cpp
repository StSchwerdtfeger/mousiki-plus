// Shared overlay arithmetic: the theme picker's scrollbar and the preview's size rule.
#include "test_framework.h"
#include "overlay_common.h"

using namespace muisc::overlay;

TEST(overlay_no_scrollbar_when_everything_fits) {
    Thumb t;
    CHECK(!scroll_thumb(0, 20, 20, t));
    CHECK(!scroll_thumb(0, 20, 5, t));
}

TEST(overlay_thumb_stays_inside_the_track) {
    for (int n = 11; n < 90; n += 7)
        for (int first = 0; first <= n - 10; ++first) {
            Thumb t;
            CHECK(scroll_thumb(first, 10, n, t));
            CHECK(t.len >= 1 && t.at >= 0 && t.at + t.len <= 10);
        }
    Thumb top, bottom;
    scroll_thumb(0, 10, 40, top);
    scroll_thumb(30, 10, 40, bottom);
    CHECK_EQ(top.at, 0);
    CHECK_EQ(bottom.at + bottom.len, 10);
}
