#pragma once
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// The Mousiki Plus Help Wizard: the user guide as a set of tutorial pages
// inside the program, so the manual is never further away than SHIFT+w.
//
// This module holds nothing but the TEXT. Both modes (the music player in
// src/ and the radio in src_radio/) draw it with their own frame, their own
// box characters and their own palette -- the radio deliberately uses the
// MUSIC PLAYER's colours here, so the wizard looks the same in both modes
// (see player_wizard_palette() in src_radio/radio_settings.h).
//
// Markup. The pages are pre-formatted: the lines are laid out by hand,
// diagrams included, and must never be re-wrapped. Two kinds of markup tell
// the renderer how to colour a line; both are stripped before drawing, so a
// line's visible width is its width after stripping.
//
//   A three-character prefix sets the colour of the whole line:
//     "@H " heading        -> header colour
//     "@D " diagram        -> border colour (the boxes drawn below)
//     "@K " key hints      -> legend colour
//     "@A " callout        -> accent colour (the arrows that point at things)
//     (no prefix)          -> ordinary text in the list colour
//
//   Inside any line, a span between the guillemets << and >> is drawn in the
//   accent colour: "@D | LOCAL AUDIO FILES |  <<-- your music folders>>".
//   They are chosen because no diagram here uses them for anything else.
//
// Every line is authored to at most kContentWidth columns, so the wizard fits
// an 80-column terminal without truncation. The welcome page is the one
// exception -- see welcome_lines() -- because the disk sits beside its text
// rather than above it.
// ---------------------------------------------------------------------------

namespace wizard {

// Master switch. false: the wizard never opens (not on the first start, not on
// SHIFT+w) in the music player and in the radio, and its rows are left out of
// the player's cheat sheet and REFERENCE tab. All pages and the drawing code
// stay in the build untouched -- set this to true to bring it back.
inline constexpr bool kEnabled = false;

// Which mode's topic list to serve. The welcome page is the same in both.
enum class Flavor { Player, Radio };

// One page of a tutorial. The arrow keys step through these.
struct Page {
    std::vector<std::string> lines;
};

// One entry of the wizard's contents list, with the pages behind it.
struct Topic {
    std::string title;   // shown in the contents list and as the page heading
    std::string blurb;   // one short line, shown behind the title in the list
    std::vector<Page> pages;
};

// The widest line any page may use. The frame adds its own border and padding.
constexpr int kContentWidth = 72;

// The width the welcome page's paragraph wraps to. Narrower than
// kContentWidth because the disk sits to its left; see welcome_lines().
constexpr int kWelcomeTextWidth = 42;

// The welcome page's text, drawn to the right of the spinning disk (which the
// renderer centres vertically against this block). `version` is the running
// build's version (MOUSIKI_VERSION), so this page never has to be touched for
// a release. The first line is a "@H " heading and the last a "@K " legend
// line; the renderer draws them, like every other wizard line, through the
// same markup described above.
std::vector<std::string> welcome_lines(const std::string& version);

// The topics of one mode, in contents order. Built once, then reused.
const std::vector<Topic>& topics(Flavor f);

// Total number of pages across every topic of one mode (the welcome page is
// not counted -- it is page 0 and belongs to no topic).
int page_count(Flavor f);

} // namespace wizard
