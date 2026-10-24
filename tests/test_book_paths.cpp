// The AUDIO BOOK PATH and the music folders: the same folder is a conflict, a book folder inside a music folder is fine.
#include "test_framework.h"
#include "settings.h"

using namespace muisc;

TEST(same_folder_path_ignores_spelling) {
    CHECK(same_folder_path("/a/b", "/a/b/"));
    CHECK(same_folder_path("/a/b/../b", "/a/b"));
    CHECK(!same_folder_path("/a/b", "/a/bc"));
    CHECK(!same_folder_path("", ""));
    CHECK(!same_folder_path("/a", ""));
}

TEST(book_path_conflict_only_for_the_same_folder) {
    const std::vector<std::string> music{"/music", "/other/"};
    CHECK(book_path_conflicts(music, "/music/"));
    CHECK(book_path_conflicts(music, "/other"));
    CHECK(!book_path_conflicts(music, "/music/books"));   // inside a music folder: allowed
    CHECK(!book_path_conflicts(music, "/"));              // around it: not the same folder
    CHECK(!book_path_conflicts(music, ""));
}
