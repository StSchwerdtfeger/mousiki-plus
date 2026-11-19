-- WezTerm settings for Mousiki Plus on Windows, taken from the Windows Terminal settings.json of the "PowerShell" profile (pwsh.exe, retrowave
-- colours, CaskaydiaMono Nerd Font Mono, acrylic, 20 % opacity, the same copy / paste keys).
-- Copy this file to  %USERPROFILE%\.wezterm.lua  (or  %USERPROFILE%\.config\wezterm\wezterm.lua).
-- Your PowerShell profile ($PROFILE), oh-my-posh and the modules are loaded by pwsh.exe itself, so they work here as in Windows Terminal.
local wezterm = require 'wezterm'
local act = wezterm.action
local config = wezterm.config_builder()

-- Shell: PowerShell 7 from the Store, like the profile (the profile's own switch -NoProfileLoadTime is kept).
config.default_prog = { wezterm.home_dir .. '\\AppData\\Local\\Microsoft\\WindowsApps\\Microsoft.PowerShell_8wekyb3d8bbwe\\pwsh.exe', '-NoProfileLoadTime' }

-- Font: the Nerd Font of the profile (oh-my-posh needs its glyphs; it has the full braille dots Mousiki's disk and pictures use).
config.font = wezterm.font('CaskaydiaMono Nerd Font Mono')
config.font_size = 12.0

-- Colours: the "retrowave" scheme.
config.colors = {
  foreground = '#46BDFF', background = '#000000',
  cursor_bg = '#FFFFFF', cursor_border = '#FFFFFF', cursor_fg = '#000000',
  selection_bg = '#FFFFFF', selection_fg = '#000000',
  ansi    = { '#181A1F', '#FF16B0', '#929292', '#FCEE54', '#46BDFF', '#FF92DF', '#DF81FC', '#FFFFFF' },
  brights = { '#FF16B0', '#F85353', '#FCEE54', '#FFFFFF', '#46BDFF', '#FF92DF', '#FF901F', '#FFFFFF' },
  tab_bar = { background = '#2E2836' },
}

-- Window: acrylic with 20 % opacity, no scroll bar, 32 rows, tab bar only with several tabs.
-- (Windows Terminal had 20 % opacity with acrylic; that makes every colour and the pictures look dim, so it is off here -- set 0.2 and 'Acrylic' to get it back.)
config.window_background_opacity = 1.0
-- config.win32_system_backdrop = 'Acrylic'
config.enable_scroll_bar = false
config.initial_rows = 32
config.initial_cols = 120
config.use_fancy_tab_bar = false
config.hide_tab_bar_if_only_one_tab = true
config.window_close_confirmation = 'NeverPrompt'
config.window_padding = { left = 8, right = 8, top = 8, bottom = 8 }
config.default_cursor_style = 'BlinkingBar'

-- Keys like the profile: Ctrl+C copies a selection (else it is the interrupt), Ctrl+V pastes, Ctrl+Shift+F searches, Alt+Shift+D splits.
config.keys = {
  { key = 'c', mods = 'CTRL', action = wezterm.action_callback(function(window, pane)
      local sel = window:get_selection_text_for_pane(pane)
      if sel and sel ~= '' then window:perform_action(act.CopyTo 'Clipboard', pane); window:perform_action(act.ClearSelection, pane)
      else window:perform_action(act.SendKey { key = 'c', mods = 'CTRL' }, pane) end
    end) },
  { key = 'v', mods = 'CTRL', action = act.PasteFrom 'Clipboard' },
  { key = 'F', mods = 'CTRL|SHIFT', action = act.Search 'CurrentSelectionOrEmptyString' },
  { key = 'D', mods = 'ALT|SHIFT', action = act.SplitHorizontal { domain = 'CurrentPaneDomain' } },
}
config.selection_word_boundary = ' \t\n{}[]()"\'`'

-- Terminal pictures. WezTerm on Windows speaks Sixel, and Mousiki uses it (nothing to set). The Kitty graphics protocol does not get through the
-- Windows pseudo console (ConPTY) that WezTerm runs programs in on Windows, so it is NOT forced here: forcing it makes the videos, MilkDrop and the
-- cover disk vanish. On Linux / macOS you can try it with:  config.enable_kitty_graphics = true  and  MOUSIKI_GFX = 'kitty'  below.
config.set_environment_variables = {
  MOUSIKI_GFX = 'sixel',   -- 'sixel' (Windows), or 'kitty' on Linux / macOS with enable_kitty_graphics = true
}

-- Speed: the GPU front end and a frame rate that matches a fast screen (the default of 60 limits animation).
config.front_end = 'WebGpu'   -- 'OpenGL' if WebGpu gives trouble on your graphics driver
config.max_fps = 120

return config
