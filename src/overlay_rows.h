#pragma once
// The rows of the overlays whose rows are selected by a number, with names instead of bare numbers. The order in each enum IS the order on
// screen; the "...Count" at the end follows by itself. Adding a row means adding a name here (at its place) and handling it in the switch
// of the overlay -- no other number has to be changed by hand.
#include <vector>

namespace muisc {

// The VIDEO SETTINGS overlay (&), top to bottom.
enum VMenuRow : int {
    kVmVideoMode,       // Use Video in Lyrics Field
    kVmLyrics,          // Include Lyrics
    kVmAutoVideo,       // Auto Video
    kVmCover,           // Cover, Disk or Thumbnail
    kVmCoverSat,        // Cover Gradient Saturation (0 .. 100 %)
    kVmSize,            // Video Size
    kVmRounded,         // Rounded Corners
    kVmEffect,          // Effect
    kVmBase,            // Base Layer (right under the effect it belongs to)
    kVmFxContrast,      // Effect Contrast (how hard the blacks of the photographic looks are)
    kVmContrast,
    kVmBrightness,
    kVmColors,
    kVmChroma,          // Chromatic Aberration
    kVmDither,          // Dither Pattern
    kVmGradSat,         // Video Gradient Saturation (0 .. 100 %)
    kVmGradVivid,       // Video Gradient Vividness (0 .. 100 %)
    kVmResolution,
    kVmFrameRate,       // Frame Rate (25 | 60)
    kVmBuffer,
    kVmClap,            // Show Clap Loading
    kVmLastSetting = kVmClap,    // (rows up to here change a setting with the arrow keys; the rest are the INDIVIDUAL SETTINGS MANAGEMENT rows and buttons)
    kVmIndSet,          // Set current values as individual settings (only when the settings were changed: that is a temporary layer)
    kVmIndRevert,       // Reset to individual settings (drops the temporary changes)
    kVmIndReset,        // Reset individual values to default settings (the track's individual values are removed)
    kVmIndCopy,         // Set individual settings to general values
    kVmUndo,            // Undo change
    kVmIndOn,           // Use Individual Settings (the playing track: on / off, the values are kept)
    kVmFullScreen,      // [ Full Screen Video ]
    kVmCouple,          // [ Manually Couple Local Files ]
    kVmQuickSearch,     // [ Quick Search/Down. YT ]
    kVmFetchMenu,       // [ General Fetch Videos Menu ]
    kVmDeleteVideo,     // [ Delete Current Video ]
    kVmRefetchVideo,    // [ Refetch this Video ]
    kVmRowCount
};

// VIDEO OPTIONS (the Video key in the big video) lists the rows of VIDEO SETTINGS (except Full Screen Video) by the same ids; this one is only there:
inline constexpr int kVmMp4AudioRow = kVmRowCount;   // Use mp4 audio for this song (a song paired with a video; listed after "Use individual settings for this track")

// The COVER STYLE OVERLAY (meta editor / SHIFT+s of the main screen). Which rows are listed depends on the Video mode and on the track
// (style_rows() below); the selection is an index into that list, never a bare number.
enum StyleRow : int {
    kStLook,            // Look of this track
    kStCoverContrast,
    kStCoverBrightness,
    kStCoverColors,
    kStCoverGradSat,    // Cover Gradient Saturation
    kStCoverVivid,      // Cover Gradient Vividness
    kStCoverChroma,
    kStCoverOrThumb,    // Use Disk/Cover/Thumbnail                (Video mode)
    kStVideoOn,         // Video of this track                    (Video mode)
    kStVideoLyrics,     // Use Lyrics on Video
    kStVideoSize,
    kStVideoEffect,
    kStVideoBase,
    kStVideoFxContrast,
    kStVideoContrast,
    kStVideoBrightness,
    kStVideoColors,
    kStVideoChroma,
    kStVideoDither,
    kStVideoGradSat,
    kStVideoResolution,
    kStVideoRounded,
    kStMp4Audio,        // Use mp4 audio for this song            (a song paired with a video)
    kStResetCover,      // Reset cover to default settings: the track's own cover style is removed (a button)
    kStSetCover,        // Set individual cover settings as default settings: the general cover values become the track's own (a button)
    kStCoverUndo,       // Undo change: the last cover change (a value, a button, the album look) is taken back (a button)
    kStMilkdropGo,      // Go to MilkDrop settings: the MilkDrop settings of this track (projectM installed)
    kStAlbum            // Look of the whole album (always the last row)
};
inline bool style_row_is_video(StyleRow r) { return r >= kStVideoOn && r <= kStMp4Audio; }   // the rows of the "Video" block
inline std::vector<StyleRow> style_rows(bool video_mode, bool paired, bool milkdrop = false) {
    std::vector<StyleRow> v = {kStLook, kStCoverContrast, kStCoverBrightness, kStCoverColors, kStCoverGradSat, kStCoverVivid, kStCoverChroma};
    (void)video_mode; (void)paired;   // (the overlay is only about the cover now: the video settings of a track are in VIDEO SETTINGS / VIDEO OPTIONS)
    v.push_back(kStResetCover);
    v.push_back(kStSetCover);
    v.push_back(kStCoverUndo);
    (void)milkdrop;   // (no MilkDrop row: the MilkDrop settings of a track are in the visuals menu [:] of the big video overlay)
    v.push_back(kStAlbum);
    return v;
}

}  // namespace muisc
