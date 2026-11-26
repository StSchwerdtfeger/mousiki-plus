// Windows only: a CreateProcess attribute list that limits which handles the new process inherits (see the long comment below).
// Used wherever a child is started with bInheritHandles=TRUE.
#pragma once
#if defined(_WIN32)
#include <windows.h>
#include <vector>

namespace muisc {

#ifndef PROC_THREAD_ATTRIBUTE_HANDLE_LIST
#define PROC_THREAD_ATTRIBUTE_HANDLE_LIST \
    ProcThreadAttributeValue(9, FALSE, TRUE, FALSE)
#endif

// RAII wrapper around a PROC_THREAD_ATTRIBUTE_LIST restricted to an explicit
// set of handles.
//
// Plain `bInheritHandles=TRUE` (what the first version of this file used)
// inherits *every* inheritable handle open anywhere in the process at the
// instant CreateProcess snapshots the handle table -- not just the ones this
// call put in STARTUPINFO. mousiki spawns subprocesses from several threads
// concurrently by design (the background per-track metadata sweep in
// launch_row_meta_resolver(), a track's own ffprobe/ffmpeg calls, yt-dlp
// resolution, the lyrics helper), so two CreateProcess calls landing close
// together is routine, not an edge case. When that happens, a handle one
// thread just created (or is about to close) for its own spawn can get
// duplicated into a completely different, concurrently-starting child --
// there's no synchronization protecting the process-wide handle table
// between "open the pipe" and "call CreateProcess". Whichever spawn ends up
// with an extra, unexpected write handle to its stdout pipe never sees EOF
// on that pipe, because something it doesn't know about is still holding it
// open; ReadFile() in ChildProcess::read() then blocks forever. If that
// happens to the metadata probe inside App::launch_load_async() -- which
// playback genuinely waits on before calling Player::play() -- the result
// looks exactly like "the track just never starts", intermittently, with no
// error anywhere.
//
// An explicit handle list closes this off completely: CreateProcess is told
// precisely which handles this one call may inherit, so what any other
// thread is doing to the process-wide handle table at the same moment
// becomes irrelevant.
class HandleInheritList {
public:
    explicit HandleInheritList(std::vector<HANDLE> handles) : handles_(std::move(handles)) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        buf_.resize(size);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf_.data());
        if (!InitializeProcThreadAttributeList(list_, 1, 0, &size)) {
            list_ = nullptr;
            return;
        }
        if (!UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       handles_.data(), handles_.size() * sizeof(HANDLE),
                                       nullptr, nullptr)) {
            DeleteProcThreadAttributeList(list_);
            list_ = nullptr;
        }
    }
    ~HandleInheritList() { if (list_) DeleteProcThreadAttributeList(list_); }
    HandleInheritList(const HandleInheritList&) = delete;
    HandleInheritList& operator=(const HandleInheritList&) = delete;

    bool valid() const { return list_ != nullptr; }
    LPPROC_THREAD_ATTRIBUTE_LIST get() const { return list_; }

private:
    std::vector<HANDLE> handles_;   // must outlive list_ -- UpdateProcThreadAttribute stores the pointer, not a copy
    std::vector<char> buf_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

} // namespace muisc
#endif
