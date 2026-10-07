"""
Master PDF - a simple PDF editor in the style of Windows 98. Runs 100% locally.
Setup:   pip install -r requirements.txt
Run:     python master_pdf.py
"""
import colorsys
import contextlib
import io
import json
import math
import os
import queue
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unicodedata
import weakref
import tkinter as tk
from tkinter import ttk, filedialog
from tkinter import font as tkfont

from PIL import Image, ImageChops, ImageDraw, ImageOps, ImageTk

import pymupdf  # PyMuPDF: reads, draws and writes the PDFs

try:  # adds drag & drop from Explorer
    from tkinterdnd2 import TkinterDnD, DND_FILES
    BaseTk, HAS_DND = TkinterDnD.Tk, True
except ImportError:
    BaseTk, HAS_DND = tk.Tk, False

def resource_path(name):
    """Path to a bundled file, both when run as a script and inside the .exe."""
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, name)


# XP-ish palette
BG, BLUE, DARK = "#ECE9D8", "#245EDC", "#0A246A"


# ---- themes ----
# The app is written with the Windows 98 Ivory (beige) colours. In any other theme, every
# colour it gives Tk - when making a widget, changing one, or drawing on a canvas - goes
# through theme_color() first, so the whole app changes without each colour being handled
# one by one. Some colours depend on their role: white is a box's background in one place and
# a 3D edge's highlight in another. A new theme is just a new table in THEME_COLORS.
THEME = "xp"  # the theme the widgets are in now (they're made in "xp", then switched)
DARK_MODE = False  # THEME == "dark"
DARK_FACE, DARK_BOX, DARK_TEXT = "#353535", "#1E1E1E", "#E8E8E8"
THEME_NAMES = {"98": "Windows 98", "xp": "Windows 98 Ivory", "dark": "Windows 98 Dark",
               "pink": "Windows 98 Pink", "jungle": "Windows 98 Jungle",
               "vapor": "Windows 98 Vapor", "mono": "Windows 98 Black and White"}
DEFAULT_THEME = "98"
THEME_COLORS = {  # theme -> ({written colour: its colour in this theme}, {role: {...}})
    "xp": ({}, {}),
    "98": ({  # Windows 95 / 98: grey face, white boxes, black / grey / white 3D edges
        "#ece9d8": "#C0C0C0",  # window / button face (BG)
        "#f5f3e8": "#C8C8C8",  # pressed button face
        "#8e8c82": "#808080",  # 3D edges' shadow
        "#efefef": "#E0E0E0",  # scrollbar track
        "#ebe8d7": "#BFBFBF",  # under the menu bar's line: the face, a touch darker
        "#f7f6f0": "#DFDFDF",  # the lists' scrollbar trough
        "#ffffe1": "#DEDEDE",  # hover tooltip (details of a file): light grey, not yellow
        "#d4d0c8": "#C3C3C3",  # an inactive window's title text: neutral grey
    }, {}),
    "dark": ({
        "#ece9d8": DARK_FACE,  # window / button face (BG)
        "#f5f3e8": "#474747",  # pressed button face
        "#ffffe1": "#404040",  # tooltip: a dark grey
        "#efefef": "#2b2b2b",  # scrollbar track
        "#cccccc": "#5b5b5b",  # scrollbar thumb
        "#a6a6a6": "#777777",  # ... under the mouse
        "#606060": "#8c8c8c",  # ... held down / ruler ticks
        "#dadada": "#454545",  # scrollbar arrow under the mouse
        "#5f5f5f": "#c2c2c2",  # scrollbar arrows
        "#b0b0b0": "#626262",  # picture box border
        "#a0a0a0": "#6c6c6c",  # timeline lines
        "#666666": "#b3b3b3",  # grey text
        "#888888": "#8f8f8f",  # hint text
        "#999999": "#727272",  # greyed-out text
        "#303030": "#d4d4d4",  # timeline numbers
        "#c00000": "#ff7a7a",  # red messages
        "#e4e4e4": "#3c3c3c",  # picture still loading
        "#d8d8d8": "#3a3a3a",  # filmstrip still loading
        "#9a9a9a": "#1c1c1c",  # parts of the filmstrip left out
        "#f0f0f0": "#3b3b3b",  # right-click menu's inner frame
        "#ebe8d7": "#333333",  # under the menu bar's line: a touch darker in dark mode
        "#0000ee": "#8AB4FF",  # links (About's GitHub link): a light blue that reads on dark
        "#f7f6f0": "#262626",  # the lists' scrollbar trough
        "#7b7b7b": "#202020",  # the grey workspace behind the pages
        "#4b4b4b": "#0a0a0a",  # the pages' shadow
    }, {  # colours that change differently depending on what they're for
        "text": {"#000000": DARK_TEXT, "#ffffff": "#ffffff"},  # (white text stays white)
        "box": {"#ffffff": DARK_BOX, "#000000": DARK_TEXT},  # box backgrounds; black shapes
        "edge": {"#ffffff": "#5e5e5e", "#8e8c82": "#1b1b1b", "#000000": "#000000"},  # 3D edges
    }),
    "pink": ({  # pink windows, teal title bars (colours taken from the reference picture)
        "#ece9d8": "#F3C0C9",  # window / button face (BG)
        "#f5f3e8": "#F6CFD6",  # pressed button face
        "#8e8c82": "#E0566E",  # 3D edges' shadow
        "#efefef": "#F9DFE3",  # scrollbar track
        "#ebe8d7": "#E9B3BD",  # under the menu bar's line: the face, a touch darker
        "#f7f6f0": "#F9DFE2",  # the lists' scrollbar trough
        "#ffffe1": "#CBFEFE",  # hover tooltip: light cyan
        "#999999": "#DE4B65",  # greyed-out text
        "#808080": "#DD4A64",  # ... (the right-click menu's)
        "#cccccc": "#EDA9B5",  # scrollbar thumb
        "#a6a6a6": "#E7919F",  # ... under the mouse
        "#606060": "#B84A5D",  # ... held down / ruler ticks
        "#dadada": "#F7D6DC",  # scrollbar arrow under the mouse
        "#5f5f5f": "#84414D",  # scrollbar arrows
        "#b0b0b0": "#E7A2AE",  # picture box border
        "#a0a0a0": "#E29AA7",  # timeline lines
        "#e4e4e4": "#F7E3E6",  # picture still loading
        "#d8d8d8": "#F2D2D8",  # filmstrip still loading
        "#888888": "#BD6878",  # hint text
        "#666666": "#7E3F4B",  # grey text
        "#7da2ce": "#6FC9BE",  # a picture box under the mouse
        "#245edc": "#00A9A6",  # the dragged selection rectangle
        "#5a8be0": "#3CC4C0",  # GIF Maker's playhead
        "#1f4fae": "#008C89",  # ... its edge
        "#a9c4f5": "#B5EFEB",  # ... its shine
        "#7b7b7b": "#C98896",  # the workspace behind the pages
    }, {
        "box": {"#ffffff": "#FFF0F4"},  # box backgrounds: a very light pink
        "edge": {"#ffffff": "#F8DDDE", "#000000": "#704049"},  # 3D edges: light / darkest
    }),
    "jungle": ({  # Windows 98's Jungle desktop theme (colours taken from the reference picture)
        "#ece9d8": "#B69F67",  # window / button face (BG)
        "#f5f3e8": "#C1AB75",  # pressed button face
        "#8e8c82": "#685832",  # 3D edges' shadow
        "#efefef": "#DFD0B8",  # scrollbar track
        "#ebe8d7": "#AA935C",  # under the menu bar's line: the face, a touch darker
        "#f7f6f0": "#DFD0B7",  # the lists' scrollbar trough
        "#ffffe1": "#EDE2C6",  # hover tooltip: light khaki
        "#999999": "#D2BE90",  # greyed-out text
        "#808080": "#D1BD8F",  # ... (the right-click menu's)
        "#cccccc": "#C9B587",  # scrollbar thumb
        "#a6a6a6": "#B19B63",  # ... under the mouse
        "#606060": "#5E4F2C",  # ... held down / ruler ticks
        "#dadada": "#E8DCC6",  # scrollbar arrow under the mouse
        "#5f5f5f": "#4A3C1C",  # scrollbar arrows
        "#b0b0b0": "#B8A67A",  # picture box border
        "#a0a0a0": "#A8955F",  # timeline lines
        "#e4e4e4": "#EEE7D8",  # picture still loading
        "#d8d8d8": "#E4D9C2",  # filmstrip still loading
        "#888888": "#6F5B30",  # hint text
        "#666666": "#4A3A18",  # grey text
        "#7da2ce": "#B0402A",  # a picture box under the mouse
        "#245edc": "#7C0000",  # the dragged selection rectangle
        "#5a8be0": "#C04818",  # GIF Maker's playhead
        "#1f4fae": "#600000",  # ... its edge
        "#a9c4f5": "#FFA858",  # ... its shine
        "#7b7b7b": "#6E5D35",  # the workspace behind the pages
    }, {
        "box": {"#ffffff": "#FBF5E6"},  # box backgrounds: a very light khaki
        "edge": {"#ffffff": "#D8C592", "#000000": "#281602"},  # 3D edges: light / darkest
    }),
    "vapor": ({  # lavender and violet, vaporwave style (colours taken from the reference picture)
        "#ece9d8": "#DCB2FE",  # window / button face (BG): lavender
        "#f5f3e8": "#E6C8FE",  # pressed button face
        "#8e8c82": "#B06AF8",  # 3D edges' shadow: purple
        "#efefef": "#EAD5FE",  # scrollbar track
        "#ebe8d7": "#CFA0FB",  # under the menu bar's line: the face, a touch darker
        "#f7f6f0": "#EAD5FD",  # the lists' scrollbar trough
        "#ffffe1": "#F3E4FF",  # hover tooltip: pale lavender
        "#999999": "#A585E6",  # greyed-out text
        "#808080": "#A484E5",  # ... (the right-click menu's)
        "#cccccc": "#D3A6FD",  # scrollbar thumb
        "#a6a6a6": "#C394FA",  # ... under the mouse
        "#606060": "#7A4FE8",  # ... held down / ruler ticks
        "#dadada": "#EFDDFF",  # scrollbar arrow under the mouse
        "#5f5f5f": "#5A3FD0",  # scrollbar arrows
        "#b0b0b0": "#C9A0FA",  # picture box border
        "#a0a0a0": "#BE92F8",  # timeline lines
        "#e4e4e4": "#F1E2FF",  # picture still loading
        "#d8d8d8": "#E9D3FE",  # filmstrip still loading
        "#888888": "#8B6FD8",  # hint text
        "#666666": "#5A3FC0",  # grey text
        "#c00000": "#C0004E",  # red messages: a raspberry red that reads on lavender
        "#0000ee": "#3B29F6",  # links: the title bar's blue-violet
        "#7da2ce": "#9C7BF8",  # a picture box under the mouse
        "#245edc": "#6F3DFF",  # the dragged selection rectangle
        "#5a8be0": "#A02CF4",  # GIF Maker's playhead
        "#1f4fae": "#5A26E8",  # ... its edge
        "#a9c4f5": "#E6B8FF",  # ... its shine
        "#7b7b7b": "#9F78E0",  # the workspace behind the pages
    }, {
        "text": {"#000000": "#3A2BC4"},  # text: indigo, not black
        "box": {"#ffffff": "#F7EEFF", "#000000": "#3A2BC4"},  # boxes: very pale lavender;
        # black shapes (the title buttons' symbols, arrows, ticks): indigo like the text
        "edge": {"#ffffff": "#F6F0FF", "#000000": "#5524E0"},  # 3D edges: light / darkest
    }),
    "mono": ({  # black and white, like Windows on a monochrome screen (a grey only where
        # something has to look greyed out). (Colours a hair off pure black / white are
        # still black / white to the eye: each colour here must come from just one.)
        "#ece9d8": "#FEFEFE",  # window / button face (BG): white
        "#f5f3e8": "#FDFDFD",  # pressed button face
        "#8e8c82": "#FCFCFD",  # 3D edges' shadow: white, so bottom / right edges look just
        # like the top / left ones (black would make them a line thicker)
        "#efefef": "#FCFCFC",  # scrollbar track (drawn checkered, see FlatScrollbar)
        "#ebe8d7": "#020202",  # under the menu bar's line: black
        "#f7f6f0": "#FBFBFB",  # the lists' scrollbar trough
        "#ffffe1": "#FAFAFA",  # hover tooltip: white
        "#999999": "#818181",  # greyed-out text: grey
        "#808080": "#7F7F7F",  # ... (the right-click menu's)
        "#cccccc": "#F9F9F9",  # scrollbar thumb
        "#a6a6a6": "#F8F8F8",  # ... under the mouse
        "#606060": "#030303",  # ... held down / ruler ticks
        "#dadada": "#F7F7F7",  # scrollbar arrow under the mouse
        "#5f5f5f": "#040404",  # scrollbar arrows
        "#b0b0b0": "#050505",  # picture box border
        "#a0a0a0": "#060606",  # timeline lines
        "#e4e4e4": "#F6F6F6",  # picture still loading
        "#d8d8d8": "#F5F5F5",  # filmstrip still loading
        "#888888": "#7E7E7E",  # hint text: grey
        "#666666": "#070707",  # grey text: black
        "#c00000": "#080808",  # red messages: black (they say what went wrong)
        "#0000ee": "#090909",  # links: black, underlined
        "#7da2ce": "#0A0A0A",  # a picture box under the mouse
        "#245edc": "#0B0B0B",  # the dragged selection rectangle
        "#5a8be0": "#0C0C0C",  # GIF Maker's playhead
        "#1f4fae": "#0D0D0D",  # ... its edge
        "#a9c4f5": "#F4F4F4",  # ... its shine
        "#9a9a9a": "#7D7D7D",  # parts of the filmstrip left out
        "#303030": "#0E0E0E",  # timeline numbers
        "#7b7b7b": "#F3F3F3",  # the workspace behind the pages: white
        "#4b4b4b": "#101010",  # the pages' shadow: black
    }, {
        # 3D edges: the light side is black too - white wouldn't show on the white face, so
        # buttons, tabs and boxes get a full black outline, like monochrome Windows'
        "edge": {"#ffffff": "#0F0F0F"},
    }),
}
# what each theme's title bars and selections look like: the inactive title bar (left,
# right), the title text (active, inactive), and a selection's background and text
THEME_LOOK = {
    "xp": (("#808080", "#A8A8A8"), ("#FFFFFF", "#D4D0C8"), ("#316AC5", "#FFFFFF")),
    "98": (("#808080", "#A8A8A8"), ("#FFFFFF", "#C3C3C3"), ("#316AC5", "#FFFFFF")),
    "dark": (("#808080", "#A8A8A8"), ("#FFFFFF", "#D4D0C8"), ("#316AC5", "#FFFFFF")),
    "pink": (("#00B8A8", "#F18EA1"), ("#FFFFFF", "#00544C"), ("#A1DAD1", "#000000")),
    "jungle": (("#7D7040", "#7D7040"), ("#FFA040", "#903018"), ("#800000", "#FFA040")),
    "vapor": (("#D365FE", "#ECC0FE"), ("#FFFFFF", "#F8EEFF"), ("#6F3DFF", "#FFFFFF")),
    "mono": (("#FFFFFF", "#FFFFFF"), ("#FFFFFF", "#000000"), ("#000000", "#FFFFFF")),
}


CUSTOM_INACTIVE = (("#808080", "#A8A8A8"), "#C3C3C3")  # the Custom theme's inactive title bar


def caption_inactive():
    """The current theme's inactive title bar colours (left, right) - in the Custom theme,
    whatever its appearance, Windows 98's grey."""
    return CUSTOM_INACTIVE[0] if CUSTOM_SELECT else THEME_LOOK[THEME][0]


def title_text(active):
    """The current theme's title text colour. The Custom theme's: white or black, whichever
    reads on its title bar colour (a white bar: black text); inactive, light grey."""
    if CUSTOM_SELECT:
        return CUSTOM_SELECT[1] if active else CUSTOM_INACTIVE[1]
    return THEME_LOOK[THEME][1][0 if active else 1]


CUSTOM_SELECT = None  # the Custom theme's selection (its title bar colour), while it's in use


def select_colors():
    """The current theme's selection: (background, text) - the Custom theme's is its title
    bar colour."""
    return CUSTOM_SELECT or THEME_LOOK[THEME][2]


def selection_for(color):
    """A selection in this colour: (the colour, white or black text - whichever reads).
    (The black is #010101: the dark theme turns plain black text light.)"""
    r, g, b = (int(color[i:i + 2], 16) for i in (1, 3, 5))
    return color.upper(), "#010101" if 0.299 * r + 0.587 * g + 0.114 * b > 150 else "#FFFFFF"
_ROLES = ("text", "box", "edge", "face")
_TO = {t: {r: {k: v.lower() for k, v in {**any_, **roles.get(r, {})}.items()} for r in _ROLES}
       for t, (any_, roles) in THEME_COLORS.items()}  # all lowercase, so both ways look up alike
_FROM = {t: {r: {v: k for k, v in m.items()} for r, m in rm.items()} for t, rm in _TO.items()}
for _t, _rm in _TO.items():  # every theme colour must lead back to one written colour
    for _r, _m in _rm.items():
        assert len(set(_m.values())) == len(_m) and not set(_m.values()) & set(_m) - {"#000000", "#ffffff"}, (_t, _r)
_NAMES = {"white": "#ffffff", "black": "#000000"}


def _norm(color):
    c = _NAMES.get(str(color).lower(), str(color).lower())
    return "#" + "".join(ch * 2 for ch in c[1:]) if len(c) == 4 and c[0] == "#" else c


def theme_color(color, role="face", theme=None):
    """A written (Ivory) colour in a theme - the current one unless given (unchanged if
    the theme doesn't change it)."""
    theme = theme or THEME
    return _TO[theme][role].get(_norm(color), color) if isinstance(color, str) else color


def written_color(color, role="face", theme=None):
    """The other way: a theme's colour back to the colour the app wrote."""
    theme = theme or THEME
    return _FROM[theme][role].get(_norm(color), color) if isinstance(color, str) else color


def thin_shadow():
    """The top / left of a 1 px sunken edge: the 3D shadow colour - but in Black and White,
    where the shadow is white (so edges look the same on all sides), black."""
    return EDGE_DARK if THEME == "mono" else EDGE_SHADOW


def untranslated(color, role):
    """color as it is, in the current theme: the theme changes some colours given to Tk
    (black shapes, say) - one of those is nudged by 1, so it's shown as given."""
    c = _norm(color)
    if THEME == "xp" or c not in _TO[THEME][role]:
        return color
    return "#%06X" % (int(c[1:], 16) ^ 1)


def themed(color, role="face"):
    """A colour for the current theme - for pictures drawn with Pillow, which Tk doesn't see."""
    return theme_color(color, role)


# which role a widget's colour options play; a widget's own background depends on its kind:
# frames' white is a 3D edge (the window / page borders), labels' and canvases' is a box
_OPT_ROLE = {"fg": "text", "foreground": "text", "activeforeground": "text",
             "disabledforeground": "text", "insertbackground": "text",
             "selectforeground": "text", "activebackground": "face",
             "highlightbackground": "face", "troughcolor": "face", "selectcolor": "box",
             "readonlybackground": "box"}
_BOX_WIDGETS = {"canvas", "label", "entry", "listbox", "text"}
_EDGE_WIDGETS = {"frame", "toplevel", "labelframe"}


def _widget_role(widget, opt):
    if opt in ("bg", "background"):
        kind = getattr(widget, "widgetName", "frame")
        return "box" if kind in _BOX_WIDGETS else "edge" if kind in _EDGE_WIDGETS else "face"
    return _OPT_ROLE.get(opt)


def _item_role(item_type, opt):
    if opt == "outline" or item_type == "line":
        return "edge"
    return "text" if item_type == "text" else "box"


def _map_opts(opts, role_of):
    if not isinstance(opts, dict):
        return opts
    out = dict(opts)
    for k, v in opts.items():
        role = role_of(k.lstrip("-"))
        if role:
            out[k] = theme_color(v, role)
    return out


_orig_options = tk.Misc._options
_orig_create = tk.Canvas._create
_orig_itemconfigure = tk.Canvas.itemconfigure


def _mono_box(widget, opts):
    """Black and White: a sunken box (2 px, Windows' dark top / left and white bottom /
    right - the white can't show on white) becomes a plain 1 px black line all round, with
    1 px of face round it so it's the same size. The widget remembers it was sunken, to be
    put back in other themes (see App.set_theme)."""
    if not isinstance(opts, dict) or opts.get("relief") != "sunken":
        return opts
    bd = opts.get("bd", opts.get("borderwidth"))
    if str(bd) != "2":
        return opts
    widget._sunken = int(str(opts.get("highlightthickness", 0) or 0))  # (its own, to put back)
    out = {k: v for k, v in opts.items() if k not in ("bd", "borderwidth")}
    out.update(relief="solid", bd=1, highlightthickness=1, highlightbackground=BG,
               highlightcolor=theme_color(BG))  # (a focus ring the face's colour: unseen)
    return out


def mono_boxes(widget, mono):
    """Switch one existing sunken box to Black and White's plain black line, or back."""
    try:
        if mono and str(widget.cget("relief")) == "sunken" and str(widget.cget("bd")) == "2":
            widget._sunken = int(str(widget.cget("highlightthickness")))
            widget.configure(relief="solid", bd=1, highlightthickness=1, highlightbackground=BG,
                             highlightcolor=theme_color(BG))
        elif not mono and hasattr(widget, "_sunken") and str(widget.cget("relief")) == "solid":
            widget.configure(relief="sunken", bd=2, highlightthickness=widget._sunken)
    except (tk.TclError, ValueError):
        pass


def _themed_options(self, cnf, kw=None):  # every widget made or changed
    if THEME == "mono":
        cnf, kw = _mono_box(self, cnf), _mono_box(self, kw)
    if THEME != "xp":
        cnf, kw = (_map_opts(o, lambda k: _widget_role(self, k)) for o in (cnf, kw))
    return _orig_options(self, cnf, kw)


def _themed_create(self, item_type, args, kw):  # every shape drawn on a canvas
    if THEME != "xp":
        pick = lambda k: _item_role(item_type, k) if k in ("fill", "outline") else None  # noqa: E731
        kw = _map_opts(kw, pick)
        args = list(args)
        if args and isinstance(args[-1], dict):
            args[-1] = _map_opts(args[-1], pick)
    return _orig_create(self, item_type, args, kw)


def _themed_itemconfigure(self, tag_or_id, cnf=None, **kw):  # every shape changed
    if THEME != "xp" and (cnf or kw):
        item_type = self.type(tag_or_id)
        pick = lambda k: _item_role(item_type, k) if k in ("fill", "outline") else None  # noqa: E731
        cnf, kw = _map_opts(cnf, pick), _map_opts(kw, pick)
    return _orig_itemconfigure(self, tag_or_id, cnf, **kw)


tk.Misc._options = _themed_options
tk.Canvas._create = _themed_create
tk.Canvas.itemconfigure = tk.Canvas.itemconfig = _themed_itemconfigure

_SYSTEM_DARK = {  # Tk's own default colours ("SystemButtonText"...) in dark mode
    "fg": DARK_TEXT, "foreground": DARK_TEXT, "activeforeground": DARK_TEXT,
    "insertbackground": DARK_TEXT, "disabledforeground": "#7a7a7a", "selectcolor": DARK_BOX,
    "troughcolor": "#262626", "highlightbackground": DARK_FACE, "activebackground": "#474747",
    "bg": DARK_FACE, "background": DARK_FACE}
SYSTEM_COLORS = {  # the Tk default colours each theme changes (the others keep Windows' own)
    "dark": _SYSTEM_DARK,
    "pink": {"disabledforeground": "#DE4B65", "selectcolor": "#FFF0F4",  # greyed text; tick boxes
             "selectbackground": "#A1DAD1", "selectforeground": "#000000"},  # selected text
    "jungle": {"disabledforeground": "#D2BE90", "selectcolor": "#FBF5E6",
               "selectbackground": "#800000", "selectforeground": "#FFA040"},
    "vapor": {"disabledforeground": "#A585E6", "selectcolor": "#F7EEFF",
              "selectbackground": "#6F3DFF", "selectforeground": "#FFFFFF",
              "fg": "#3A2BC4", "foreground": "#3A2BC4", "activeforeground": "#3A2BC4"},
    "mono": {"disabledforeground": "#808080", "selectcolor": "#FFFFFF",
             "selectbackground": "#000000", "selectforeground": "#FFFFFF"},
}
_SYSTEM_OPTS = list(dict.fromkeys(o for colors in SYSTEM_COLORS.values() for o in colors))


def system_colors(theme):
    """The Tk default colours a theme changes, with its selection as it is now (the Custom
    theme's follows its title bar)."""
    colors = dict(SYSTEM_COLORS.get(theme, {}))
    if CUSTOM_SELECT or "selectbackground" in colors:
        colors["selectbackground"], colors["selectforeground"] = select_colors()
    return colors
_system_colors = {}  # (widget, option) -> the Tk default it had before, to put back


def retheme(widget, old, new):
    """Switch one existing widget (and a canvas's drawings) from theme old to theme new."""
    system = system_colors(new)

    def convert(val, role):
        return theme_color(written_color(val, role, old), role, new)
    for opt in _SYSTEM_OPTS:
        try:
            val = str(widget.cget(opt))
        except (tk.TclError, ValueError):
            continue
        key = (str(widget), opt)
        if val.lower().startswith("system"):  # a Tk default colour
            if opt in system:
                _system_colors[key] = val
                to = system[opt]
            else:
                continue
        elif key in _system_colors:  # a Tk default another theme changed
            to = system[opt] if opt in system else _system_colors.pop(key)
        else:
            role = _widget_role(widget, opt)
            to = convert(val, role) if role and opt in _SYSTEM_DARK else val
        if to != val:
            try:
                widget.configure({opt: to})
            except tk.TclError:
                pass
    if isinstance(widget, tk.Canvas):
        for item in widget.find_all():
            item_type = widget.type(item)
            for opt in ("fill", "outline"):
                try:
                    val = widget.itemcget(item, opt)
                except tk.TclError:
                    continue
                to = convert(val, _item_role(item_type, opt)) if val else val
                if to != val:
                    widget.itemconfigure(item, {opt: to})


# ---- updates: new versions come from this project's GitHub Releases ----
APP_VERSION = "dev"  # GitHub puts the release's version here when it builds the app
GITHUB_REPO = "bocchhii/master-pdf"


def parse_version(text):
    """ "v2.10.1" -> (2, 10, 1), so versions compare as numbers; None if there's none."""
    nums = re.findall(r"\d+", text or "")
    return tuple(int(n) for n in nums[:3]) if nums else None


def fetch_latest_release():
    """(version, release notes, installer download link) of the newest release, or None
    (no internet, GitHub unreachable...). Only reads the public release list."""
    import urllib.request
    req = urllib.request.Request(
        f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest",
        headers={"User-Agent": "MasterPDF", "Accept": "application/vnd.github+json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            data = json.load(r)
    except Exception:
        return None
    url = next((a.get("browser_download_url") for a in data.get("assets", [])
                if a.get("name", "").lower().endswith("setup.exe")), None)
    version = (data.get("tag_name") or "").lstrip("vV")
    return (version, data.get("body") or "", url) if url and parse_version(version) else None


def run_installer_after_exit(setup):
    """Start the downloaded installer - silently, over this version - but only once this app
    has completely closed.
    Why wait: the installed .exe is a one-file bundle, so two processes run it - a small
    launcher (which unpacks the app to a temp folder) and the app itself. If the installer
    starts while they're still running, its "close running apps" step (Windows' Restart
    Manager) catches the launcher half-way through closing and leaves it stuck, holding the
    .exe open; the silent installer then can't replace the file and quietly gives up - the
    app closes, and reopens as the old version. So a small hidden helper waits for the app
    and its launcher to exit (ending the launcher if it's stuck - all it has left to do is
    delete its temp folder, which the helper then does), and only then runs the installer.
    The installer logs to %TEMP%\\MasterPDF-update.log."""
    args = ["/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CLOSEAPPLICATIONS",
            "/LOG=" + os.path.join(tempfile.gettempdir(), "MasterPDF-update.log")]
    launcher = 0
    if getattr(sys, "frozen", False):  # the launcher: our parent, running the same .exe
        try:
            import ctypes
            from ctypes import wintypes
            k = ctypes.windll.kernel32
            h = k.OpenProcess(0x1000, False, os.getppid())  # PROCESS_QUERY_LIMITED_INFORMATION
            if h:
                buf, size = ctypes.create_unicode_buffer(1024), wintypes.DWORD(1024)
                if k.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
                    if os.path.normcase(buf.value) == os.path.normcase(sys.executable):
                        launcher = os.getppid()
                k.CloseHandle(h)
        except Exception:
            launcher = 0
    q = lambda s: "'" + str(s).replace("'", "''") + "'"  # noqa: E731 - a PowerShell string
    unpacked = getattr(sys, "_MEIPASS", "")
    script = "\r\n".join([
        "$ErrorActionPreference = 'SilentlyContinue'",
        f"Wait-Process -Id {os.getpid()} -Timeout 60",  # the app: closing right now
        f"$launcher = {launcher}",
        "if ($launcher) {",
        "    Wait-Process -Id $launcher -Timeout 15",  # normally gone within a second or two
        "    $p = Get-Process -Id $launcher",
        f"    if ($p -and $p.Path -eq {q(sys.executable)}) {{",  # stuck: end it and tidy up
        "        Stop-Process -Id $launcher -Force; Start-Sleep -Milliseconds 500",
        f"        if ({q(unpacked)}) {{ Remove-Item -LiteralPath {q(unpacked)} -Recurse -Force }}",
        "    }",
        "}",
        "Start-Sleep -Milliseconds 300",
        f"Start-Process -FilePath {q(setup)} -ArgumentList "
        + ",".join(q(a) for a in args[:-1]) + "," + q('"' + args[-1] + '"'),
    ])
    helper = os.path.join(tempfile.gettempdir(), "MasterPDF-update.ps1")
    try:
        with open(helper, "w", encoding="utf-8-sig") as f:
            f.write(script)
        subprocess.Popen(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                          "-WindowStyle", "Hidden", "-File", helper],
                         creationflags=0x08000000 | 0x00000200)  # no window, own group
    except Exception:  # no PowerShell?: start the installer straight away, as before
        subprocess.Popen([setup] + args)


def plain_notes(markdown):
    """GitHub release notes are written in Markdown; show them as tidy plain text."""
    lines = []
    for line in (markdown or "").replace("\r\n", "\n").split("\n"):
        line = re.sub(r"^\s*#+\s*", "", line)  # "## Title" -> "Title"
        line = re.sub(r"^(\s*)[-*]\s+", "\\1\u2022  ", line)  # "- item" -> a bullet point
        line = re.sub(r"\*\*(.+?)\*\*|__(.+?)__", lambda m: m[1] or m[2], line)  # bold
        line = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", line)  # [text](link) -> text
        lines.append(line.replace("`", ""))
    return "\n".join(lines).strip()


def notes_dialog(parent, heading, notes):
    """A message box with a heading and a scrollable block of text (the release notes)."""
    owner = parent.winfo_toplevel()
    win = tk.Toplevel(owner)
    win.configure(bg=BG)
    win.resizable(False, False)
    win.transient(owner)
    body = ClassicWindow(win, "Master PDF", win.destroy, resizable=False,
                         taskbar=False).body
    tk.Label(body, text=heading, bg=BG, font=(FONT[0], FONT[1], "bold"), justify="left",
             anchor="w").pack(fill="x", padx=12, pady=(12, 6))
    box = tk.Frame(body, bg=BG)
    box.pack(padx=12)
    text = tk.Text(box, width=58, height=14, wrap="word", font=FONT, bg="white", fg="black",
                   relief="sunken", bd=2, padx=6, pady=4, highlightthickness=0)
    sb = FlatScrollbar(box, command=text.yview)
    sb.config(height=1)  # stretch to the text box's height instead of setting it
    text.config(yscrollcommand=sb.set)
    text.insert("1.0", notes or "(no release notes)")
    text.config(state="disabled")  # read only
    text.pack(side="left")
    sb.pack(side="left", fill="y")
    row = tk.Frame(body, bg=BG)
    row.pack(pady=(10, 12))
    ok = xp_button(row, "OK", win.destroy)
    ok.pack()
    win.bind("<Return>", lambda e: win.destroy())
    win.bind("<Escape>", lambda e: win.destroy())
    win.update_idletasks()
    x = owner.winfo_rootx() + (owner.winfo_width() - win.winfo_reqwidth()) // 2
    y = owner.winfo_rooty() + (owner.winfo_height() - win.winfo_reqheight()) // 3
    win.geometry(f"+{max(0, x)}+{max(0, y)}")
    win.focus_force()
    ok.focus_set()
    win.grab_set()
    play_sound("done")
    win.wait_window()


SETTINGS_PATH = os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"),
                             "Master PDF", "settings.json")
CUSTOM_ICON = os.path.join(os.path.dirname(SETTINGS_PATH), "custom_icon.png")  # Settings > App icon


def app_folder():
    """Where Master PDF is: the installed .exe's folder, or the script's."""
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))


APPEARANCE_NAMES = {"98": "Light grey", "xp": "Ivory", "dark": "Dark grey", "pink": "Pink",
                    "jungle": "Jungle", "vapor": "Vapor", "mono": "Black and white"}  # Settings
CUSTOM_THEME = "custom"  # the Theme menu's last choice: your own appearance + title bar


def saved_theme(settings):
    """The Theme menu's choice: a built-in theme, or "custom". (Older settings: "dark": true
    was dark mode; a title bar colour chosen before there was a Custom theme becomes it.)"""
    theme = settings.get("theme")
    if theme == CUSTOM_THEME and settings.get("custom_theme"):
        return theme
    if theme not in THEME_NAMES:
        theme = "dark" if settings.get("dark") else DEFAULT_THEME
    if settings.get("palette") and "custom_theme" not in settings:
        return CUSTOM_THEME
    return theme


def theme_look(settings, choice=None):
    """(appearance, title bar palette name, custom colour) for a Theme menu choice."""
    choice = choice or saved_theme(settings)
    if choice != CUSTOM_THEME:
        return choice, THEME_PALETTE[choice], None
    ct = settings.get("custom_theme")
    if ct is None:  # (from older settings: the title bar colour chosen then)
        theme = settings.get("theme") if settings.get("theme") in THEME_NAMES else (
            "dark" if settings.get("dark") else DEFAULT_THEME)
        ct = {"appearance": theme, "palette": settings.get("palette"),
              "custom_color": settings.get("custom_color")}
    appearance = ct.get("appearance") if ct.get("appearance") in THEME_NAMES else DEFAULT_THEME
    palette = ct.get("palette")
    if palette not in TITLE_PALETTES and palette != CUSTOM_PALETTE:
        palette = THEME_PALETTE[appearance]
    return appearance, palette, ct.get("custom_color")


def load_settings():
    try:
        with open(SETTINGS_PATH, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_settings(**values):
    """Remember choices (like dark mode) for next time; quietly skipped if it can't."""
    try:
        data = {**load_settings(), **values}
        os.makedirs(os.path.dirname(SETTINGS_PATH), exist_ok=True)
        with open(SETTINGS_PATH, "w", encoding="utf-8") as f:
            json.dump(data, f)
    except OSError:
        pass


def all_widgets(root):
    out, todo = [], [root]
    while todo:
        w = todo.pop()
        out.append(w)
        todo.extend(w.winfo_children())
    return out
# MS Sans Serif: the Windows 95 / 98 dialog font (a crisp bitmap font; comes with Windows)
FONT = ("MS Sans Serif", 8)

def unique_path(folder, base, ext):
    """folder/base+ext, or "base (2)", (3)... so an existing file is never overwritten."""
    dst, n = os.path.join(folder, base + ext), 2
    while os.path.exists(dst):
        dst = os.path.join(folder, f"{base} ({n}){ext}")
        n += 1
    return dst


CAPTION_ACTIVE = ("#000080", "#1084D0")  # Windows 98: navy fading to light blue
# Settings > Color palette: the active title bar's colours (left, right)
TITLE_PALETTES = {
    "Windows 98 blue": ("#000080", "#1084D0"),
    "Teal": ("#006A6A", "#2AA8A8"),
    "Plum": ("#4B1466", "#9A58BE"),
    "Maroon": ("#700000", "#B83A3A"),
    "Forest green": ("#1F5A1F", "#5E9E4A"),
    "Rose": ("#861E4E", "#D06A92"),
    "Slate": ("#2E3C4C", "#7E8EA0"),
    "Charcoal": ("#101010", "#4A4A4A"),
    "Teal and pink": ("#00BDBA", "#DC97B8"),  # Windows 98 Pink's
    "Jungle black": ("#000000", "#000000"),  # Windows 98 Jungle's: plain black
    "Vapor violet": ("#3B29F6", "#DCC2FE"),  # Windows 98 Vapor's
    "Black": ("#000000", "#000000"),  # Windows 98 Black and White's
}
CUSTOM_PALETTE = "Custom..."  # the last choice in the list: pick any colour
THEME_PALETTE = {"98": "Windows 98 blue", "xp": "Windows 98 blue", "dark": "Windows 98 blue",
                 "pink": "Teal and pink", "jungle": "Jungle black",
                 "vapor": "Vapor violet", "mono": "Black"}  # unless one's chosen


def palette_colors(name, custom_color=None):
    """A palette's title bar colours (left, right); Custom... is made from its one colour."""
    if name == CUSTOM_PALETTE and custom_color:
        return custom_palette(custom_color)
    return TITLE_PALETTES.get(name) or TITLE_PALETTES["Windows 98 blue"]


def custom_palette(color):
    """A title bar from one chosen colour: it on the left, fading to a lighter shade of it on
    the right, like the ready-made palettes."""
    rgb = [int(color[i:i + 2], 16) for i in (1, 3, 5)]
    light = [round(v + (255 - v) * 0.35) for v in rgb]
    return color.upper(), "#" + "".join(f"{v:02X}" for v in light)


class ClassicWindow:
    """Replaces Windows' own title bar with a classic one: navy bar, bold white title, no icon,
    small raised _ / [] / X buttons, and a 3D window border. Windows' bar is removed
    (overrideredirect), so this also does its jobs: move by dragging the bar, double-click to
    maximize, minimize to the taskbar, resize from the border, grey bar when not active.
    Other systems keep their normal title bar. Put the window's contents in .body."""
    TITLE_H, BTN_W, BTN_H, GRIP, CORNER = 26, 21, 19, 5, 16
    alive = weakref.WeakSet()  # (every one open: a theme change redraws them all at once)

    def __init__(self, win, title, on_close, resizable=True, min_size=(240, 120), taskbar=True):
        self.win, self.title, self.on_close = win, title, on_close
        self.resizable, self.min_size = resizable, min_size
        self.maximized, self.normal_geo, self.active = False, None, True
        self.pressed, self.down = None, False  # title-bar button held down, and shown down?
        self.on_update = None  # set_update_button: shows the green "update" button when set
        self.on_help = None  # set by WhatsThis: shows the ? button (dialogs) when set
        self.icon = None  # set_icon: a small picture before the title
        self._move, self._resize, self._grad = None, None, None
        win.title(title)  # still shown on the taskbar and in Alt+Tab
        if sys.platform != "win32":
            self.body = tk.Frame(win, bg=BG)
            self.body.pack(fill="both", expand=True)
            return
        win.overrideredirect(True)
        # 3D border from nested frames, each showing its colour on two sides:
        # outside: light-grey top/left, black bottom/right; inside: white top/left, grey bottom/right
        edges = []
        parent = win
        for bg, padx, pady in zip(self.border(), (0, (0, 1), (1, 0), (0, 1), (1, 0)),
                                  (0, (0, 1), (1, 0), (0, 1), (1, 0))):
            f = tk.Frame(parent, bg=bg)
            f.pack(fill="both", expand=True, padx=padx, pady=pady)
            edges.append(f)
            parent = f
        # width=1: the bar stretches to the window instead of setting its width
        # cursor="arrow": widgets without their own cursor take their parent's, so without this
        # everything inside would pick up the border's resize cursor
        self.bar = tk.Canvas(parent, height=self.TITLE_H, width=1, highlightthickness=0,
                             bg=CAPTION_ACTIVE[0], cursor="arrow")
        self.bar.pack(fill="x", padx=1, pady=1)
        self.body = tk.Frame(parent, bg=BG, cursor="arrow")
        self.body.pack(fill="both", expand=True, padx=1, pady=(0, 1))
        ClassicWindow.alive.add(self)

        self.bar.bind("<Configure>", lambda e: self.draw())
        self.bar.bind("<ButtonPress-1>", self.bar_press)
        self.bar.bind("<B1-Motion>", self.bar_drag)
        self.bar.bind("<ButtonRelease-1>", self.bar_release)
        self.bar.bind("<Double-Button-1>", self.bar_double)
        self.edges = edges
        self.border_colors()
        if resizable:  # the border is handled by watching the mouse (see watch_edges)
            self._zone_shown, self._was_down = "", False
            self._borrowed = {}  # widget -> its own cursor, while it shows a resize one
            # the moment the mouse moves (not at the next look, up to 30 ms later): the right
            # cursor, so it never flickers between the arrow and the resize one
            win.bind("<Motion>", lambda e: None if self._resize else self.follow_pointer(), add="+")
            # a press on the border starts resizing at once: waiting for the next look missed
            # a quick press-and-drag (the mouse had already left the border by then)
            win.bind("<ButtonPress-1>", self.border_press, add="+")
            win.after(100, self.watch_edges)
        win.bind("<FocusIn>", lambda e: self.set_active(True), add="+")
        win.bind("<FocusOut>", lambda e: win.after(30, self.check_active), add="+")
        win.bind("<Alt-F4>", lambda e: self.on_close())
        if taskbar:
            win.after(1, self.show_on_taskbar)
        else:  # a dialog: hand the keyboard (and the blue title) back to its window when it closes
            win.bind("<Destroy>", lambda e: e.widget is win and self.give_back_focus(), add="+")
            win.after(1, self.stay_above_owner)

    @staticmethod
    def border():  # the border frames' colours, outside in
        return [EDGE_DARK, BG, EDGE_SHADOW, EDGE_LIGHT, BG]

    def border_colors(self):
        """The window border's colours for the theme. Its frames each show on two sides, so
        the classic 3D border differs top / left from bottom / right; in Black and White it's
        the same on every side - white outside, then a black line."""
        colors = self.border()
        if THEME == "mono":
            colors[0], colors[2] = BG, EDGE_DARK  # (bottom / right: like the top / left)
        for frame, color in zip(getattr(self, "edges", ()), colors):
            tk.Frame.configure(frame, bg=color)

    def stay_above_owner(self):
        """Without Windows' title bar, Tk no longer tells Windows who owns a dialog, so it could
        open behind the main window. Set the owner so it always stays in front of it."""
        try:
            import ctypes
            self.win.update_idletasks()
            owner = ctypes.windll.user32.GetParent(self.win.master.winfo_toplevel().winfo_id())
            ctypes.windll.user32.SetWindowLongPtrW(self.hwnd(), -8, owner)  # GWLP_HWNDPARENT
            self.win.lift()
        except Exception:
            pass

    def give_back_focus(self):
        try:
            self.win.master.focus_force()
        except tk.TclError:
            pass  # the whole app is closing

    # ---- Windows plumbing ----
    def hwnd(self):
        import ctypes
        return ctypes.windll.user32.GetParent(self.win.winfo_id())

    def show_on_taskbar(self):
        """Windows without a title bar are left off the taskbar; put this one back on it,
        with a minimize box so clicking its taskbar button minimizes/restores it."""
        try:
            import ctypes
            self.win.update_idletasks()
            u, h = ctypes.windll.user32, self.hwnd()
            ex = u.GetWindowLongW(h, -20)  # GWL_EXSTYLE
            u.SetWindowLongW(h, -20, (ex & ~0x80) | 0x40000)  # - TOOLWINDOW + APPWINDOW
            st = u.GetWindowLongW(h, -16)  # GWL_STYLE
            u.SetWindowLongW(h, -16, st | 0x20000 | 0x80000)  # + MINIMIZEBOX + SYSMENU
            # re-show so the taskbar notices - through Windows directly, because Tk's own
            # withdraw/deiconify would put the old style back
            u.ShowWindow(h, 0)  # SW_HIDE
            u.ShowWindow(h, 5)  # SW_SHOW
        except Exception:
            pass

    def minimize(self):
        try:
            import ctypes
            ctypes.windll.user32.ShowWindow(self.hwnd(), 6)  # SW_MINIMIZE
        except Exception:
            pass

    def work_area(self, point=None):
        """The area of a screen not covered by the taskbar: x, y, w, h - the screen the window
        is on (mostly), or the one at point (x, y) on the desktop. With more than one screen,
        each has its own."""
        try:
            import ctypes
            from ctypes import wintypes

            class MONITORINFO(ctypes.Structure):
                _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT),
                            ("rcWork", wintypes.RECT), ("dwFlags", wintypes.DWORD)]
            u = ctypes.windll.user32
            if point is None:
                mon = u.MonitorFromWindow(self.hwnd(), 2)  # MONITOR_DEFAULTTONEAREST
            else:
                u.MonitorFromPoint.argtypes = [wintypes.POINT, wintypes.DWORD]
                mon = u.MonitorFromPoint(wintypes.POINT(*point), 2)
            mi = MONITORINFO()
            mi.cbSize = ctypes.sizeof(MONITORINFO)
            if not u.GetMonitorInfoW(ctypes.c_void_p(mon), ctypes.byref(mi)):
                raise OSError
            r = mi.rcWork
            return r.left, r.top, r.right - r.left, r.bottom - r.top
        except Exception:
            return 0, 0, self.win.winfo_screenwidth(), self.win.winfo_screenheight()

    @staticmethod
    def screen_at(x, y):
        """Is there a screen at (x, y) on the desktop?"""
        try:
            import ctypes
            from ctypes import wintypes
            u = ctypes.windll.user32
            u.MonitorFromPoint.argtypes = [wintypes.POINT, wintypes.DWORD]
            return bool(u.MonitorFromPoint(wintypes.POINT(x, y), 0))  # MONITOR_DEFAULTTONULL
        except Exception:
            return False

    def snap_target(self, px, py):
        """Where the window would snap, dragged to (px, py): at the top of a screen, ("max",
        rect) in its middle third, ("tall-left" / "tall-right", rect) in its left / right
        third; ("left" / "right", rect) at its left / right edge - or None. Only at a
        screen's outer edges: an edge that leads onto another screen is left free, so the
        window can be dragged across."""
        x, y, w, h = self.work_area((px, py))
        mw = self.min_size[0]
        edge = 1  # how close counts as "at" the edge
        free = lambda qx, qy: not self.screen_at(qx, qy)  # noqa: E731 (nothing beyond)
        left = px <= x + edge and free(x - 3, py)
        right = px >= x + w - 1 - edge and free(x + w + 2, py)
        top = py <= y + edge and free(px, y - 3)
        hw = min(max(w // 2, mw), w)  # half the screen (never below the smallest size)
        if left:
            return "left", (x, y, hw, h)
        if right:
            return "right", (x + w - hw, y, hw, h)
        if top:  # the top in three: in its left / right third the window keeps its width
            # and gets as tall as the screen, at that side; in the middle, maximized
            ww = min(self.win.winfo_width(), w)
            if px < x + w / 3:
                return "tall-left", (x, y, ww, h)
            if px >= x + w * 2 / 3:
                return "tall-right", (x + w - ww, y, ww, h)
            return "max", (x, y, w, h)
        return None

    def toggle_maximize(self):
        if not self.resizable:
            return
        if self.maximized:  # back to a snapped half if it was one, else the size from before
            self.win.geometry(self._half_geo if getattr(self, "half", False) else self.normal_geo)
        else:
            if getattr(self, "half", False):  # (a snapped half keeps its size from before
                self._half_geo = self.win.geometry()  # the snap, for dragging it out later)
            else:
                self.normal_geo = self.win.geometry()
            x, y, w, h = self.work_area()
            self.win.geometry(f"{w}x{h}+{x}+{y}")
        self.maximized = not self.maximized
        self.draw()

    # ---- active / inactive ----
    def check_active(self):
        try:
            f = self.win.focus_get()
            top = f.winfo_toplevel() if f is not None else None
            # its own right-click menu counts as still in the window, like Windows' menus
            self.set_active(top is self.win or (
                getattr(top, "keeps_owner_active", False) and top.master.winfo_toplevel() is self.win))
        except (KeyError, tk.TclError):
            pass

    def set_active(self, active):
        if active != self.active:
            self.active = active
            self.draw()

    def set_icon(self, path, size=16):
        """Show the picture at path, size x size, at the left of the title bar."""
        self.icon = ImageTk.PhotoImage(Image.open(path).convert("RGBA").resize((size, size), Image.LANCZOS))
        if hasattr(self, "bar"):
            self.draw()

    def set_update_button(self, on_click):
        """Show a green download-arrow button left of _ that calls on_click (None hides it)."""
        self.on_update = on_click
        if hasattr(self, "bar"):
            self.draw()

    # ---- drawing ----
    def buttons(self):
        """[(kind, x0, y0)] from the right: X, [] and _ side by side with no gaps; X 3 px
        from the bar's right edge, all centred up and down. The update button, when shown,
        sits a little apart to the left of them, and the ? button (What's This?) left of
        everything - so the two never overlap."""
        W = self.bar.winfo_width()
        margin = 3  # from the bar's right edge
        y = (self.TITLE_H - self.BTN_H + 1) // 2  # centred; an odd spare pixel goes above
        x = W - margin - self.BTN_W
        out = [("close", x, y)]
        if self.resizable:
            x -= self.BTN_W
            out.append(("restore" if self.maximized else "max", x, y))
            x -= self.BTN_W
            out.append(("min", x, y))
        if self.on_update:  # its own spot, 8 px apart, just left of _
            x -= self.BTN_W + 8
            out.append(("update", x, y))
        if self.on_help:  # "What's This?": 2 px left of what's there (X on dialogs; _ or the
            x -= self.BTN_W + 2  # update button on the main window), like Windows 98
            out.append(("help", x, y))
        return out

    def draw(self):
        c, W, H = self.bar, self.bar.winfo_width(), self.TITLE_H
        c.delete("all")
        a, b = CAPTION_ACTIVE if self.active else caption_inactive()
        key = (W, a, b)
        if self._grad is None or self._grad[0] != key:
            ramp = Image.new("RGB", (256, 1))
            ca, cb = [int(a[i:i + 2], 16) for i in (1, 3, 5)], [int(b[i:i + 2], 16) for i in (1, 3, 5)]
            ramp.putdata([tuple(round(p + (q - p) * i / 255) for p, q in zip(ca, cb))
                          for i in range(256)])
            self._grad = (key, ImageTk.PhotoImage(ramp.resize((max(W, 1), H))))
        c.create_image(0, 0, image=self._grad[1], anchor="nw")
        x = 6
        if self.icon:
            c.create_image(x, H // 2, image=self.icon, anchor="w")
            x += self.icon.width() + 5
        c.create_text(x, H // 2, text=self.title, anchor="w", font=(FONT[0], 10, "bold"),
                      fill=title_text(self.active))
        for kind, x, y in self.buttons():
            self.draw_button(kind, x, y, self.pressed == kind and self.down)

    def draw_button(self, kind, x, y, down):
        c, w, h = self.bar, self.BTN_W, self.BTN_H
        c.create_rectangle(x, y, x + w, y + h, fill=BG, outline="")
        tl, br = (EDGE_DARK, EDGE_LIGHT) if down else (EDGE_LIGHT, EDGE_DARK)
        c.create_line(x, y + h - 1, x, y, x + w - 1, y, fill=tl)  # outer edge
        c.create_line(x, y + h - 1, x + w - 1, y + h - 1, x + w - 1, y - 1, fill=br)
        if down:
            c.create_line(x + 1, y + h - 2, x + 1, y + 1, x + w - 2, y + 1, fill=EDGE_SHADOW)
        else:
            c.create_line(x + 1, y + h - 2, x + w - 2, y + h - 2, x + w - 2, y, fill=EDGE_SHADOW)
        o = 1 if down else 0  # the symbol shifts when pressed, like a real button
        x, y = x + o, y + o

        def px(x0, y0, x1, y1):  # filled black block [x0, x1) x [y0, y1)
            c.create_rectangle(x + x0, y + y0, x + x1, y + y1, fill="#000000", outline="")
        # symbols drawn pixel by pixel for a 21 x 19 button, so they stay crisp. The face inside
        # the 3D edge is x 1-18, y 1-16, so its middle is (9.5, 8.5); every symbol is centred
        # on that and ends on the same bottom line (y 13), like classic Windows lines up _ and []
        if kind == "min":
            px(6, 12, 14, 14)  # x 6-13, on the bottom line
        elif kind == "max":
            px(5, 4, 15, 6)  # thick top, like the classic one; x 5-14, y 4-13
            px(5, 6, 6, 14)
            px(14, 6, 15, 14)
            px(5, 13, 15, 14)
        elif kind == "restore":  # two overlapping windows, x 4-15, y 3-13
            px(8, 3, 16, 5)  # back window
            px(15, 5, 16, 11)
            px(13, 10, 15, 11)
            px(8, 5, 9, 7)
            px(4, 7, 13, 9)  # front window
            px(4, 9, 5, 14)
            px(12, 9, 13, 14)
            px(4, 13, 13, 14)
        elif kind == "help":  # a bold ?, x 6-13, y 3-14
            px(7, 3, 13, 5)  # top of the hook
            px(6, 4, 8, 7)  # its left end
            px(12, 4, 14, 8)  # right side
            px(10, 7, 13, 9)  # curling back in
            px(9, 8, 11, 11)  # stem
            px(9, 12, 11, 14)  # the dot
        elif kind == "update":  # arrow down onto a line, x 5-14, y 2-13, 2-px strokes like X
            px(9, 2, 11, 9)  # shaft, ending inside the head so the tip stays sharp
            for i in range(5):  # head: drawn with the X's strokes
                px(5 + i, 6 + i, 7 + i, 7 + i)
                px(13 - i, 6 + i, 15 - i, 7 + i)
            px(6, 12, 14, 14)  # the line: the same as _
        else:  # close: Windows 98's X - 2-px arms, a little wider than tall; x 5-14, y 5-13
            for i in range(9):
                px(5 + i, 5 + i, 7 + i, 6 + i)
                px(13 - i, 5 + i, 15 - i, 6 + i)

    # ---- title bar mouse ----
    def button_at(self, x, y):
        return next((k for k, bx, by in self.buttons()
                     if bx <= x < bx + self.BTN_W and by <= y < by + self.BTN_H), None)

    def bar_press(self, e):
        kind = self.button_at(e.x, e.y)
        self._snap = None  # (the screen's work area, while dragged to its top: see bar_drag)
        if kind:
            self.pressed, self.down = kind, True
            self.draw()
        elif self.maximized or getattr(self, "half", False):  # dragging it will bring back
            self._unsnap = (e.x_root, e.y_root, e.x, e.y) if self.resizable else None  # its size
        else:  # start moving the window
            self._move = (e.x_root - self.win.winfo_x(), e.y_root - self.win.winfo_y())
            self._before_snap = self.win.geometry()  # the size to go back to if it's snapped

    def bar_drag(self, e):
        unsnap = getattr(self, "_unsnap", None)
        if unsnap and max(abs(e.x_root - unsnap[0]), abs(e.y_root - unsnap[1])) >= 4:
            self.drag_out_of_maximized(e, unsnap)
        if self._move:
            self.win.geometry(f"+{e.x_root - self._move[0]}+{e.y_root - self._move[1]}")
            if self.resizable:  # like Windows 10 / 11: dragged to the screen's left or right
                # edge, the window will fill that half of it; to the top, all of it - shown
                # by an outline where it'll go
                snap = self.snap_target(e.x_root, e.y_root)
                if snap != self._snap:
                    self._snap = snap
                    self.show_outline(snap[1] if snap else None)
        elif self.pressed:  # like a real button: pops back up if the mouse slides off it
            down = self.button_at(e.x, e.y) == self.pressed
            if down != self.down:
                self.down = down
                self.draw()

    def bar_release(self, e):
        self._move, self._unsnap = None, None
        snap = getattr(self, "_snap", None)
        if snap:  # let go at a screen edge: the window fills the outline
            self._snap = None
            self.show_outline(None)
            kind, (x, y, w, h) = snap
            self.normal_geo = self._before_snap  # (un-snapping brings back the old size)
            self.win.geometry(f"{w}x{h}+{x}+{y}")
            self.maximized, self.half = kind == "max", kind != "max"
            self.draw()
        kind, self.pressed = self.pressed, None
        if kind:
            self.draw()
            if self.button_at(e.x, e.y) == kind:  # released on the same button: do it
                {"close": self.on_close, "min": self.minimize, "max": self.toggle_maximize,
                 "restore": self.toggle_maximize, "update": self.on_update,
                 "help": self.on_help}[kind]()

    def drag_out_of_maximized(self, e, start):
        """A maximized window's title bar dragged: it goes back to its size from before, under
        the mouse - the same spot of the title bar stays under it, like Windows 10 / 11 - and
        carries on being dragged."""
        self._unsnap, self.half = None, False
        m = re.match(r"(\d+)x(\d+)", self.normal_geo or "")
        if not m:
            return
        w, h = int(m.group(1)), int(m.group(2))
        x0, y0, sx, sy = start
        frac = sx / max(1, self.bar.winfo_width())  # how far across the title bar it was held
        nx = e.x_root - round(frac * w)
        ny = e.y_root - sy - (self.bar.winfo_rooty() - self.win.winfo_rooty())
        self.maximized = False
        self.win.geometry(f"{w}x{h}+{nx}+{ny}")
        self.draw()
        self._move = (e.x_root - nx, e.y_root - ny)
        self._before_snap = self.normal_geo

    def bar_double(self, e):
        if not self.button_at(e.x, e.y):
            self.toggle_maximize()

    # ---- resizing from the border ----
    # Windows' own resize cursors: IDC_SIZENS, IDC_SIZEWE, IDC_SIZENWSE, IDC_SIZENESW
    SIZE_CURSORS = {"n": 32645, "s": 32645, "e": 32644, "w": 32644,
                    "nw": 32642, "se": 32642, "ne": 32643, "sw": 32643}
    TK_CURSORS = {"n": "size_ns", "s": "size_ns", "e": "size_we", "w": "size_we",  # the same
                  "nw": "size_nw_se", "se": "size_nw_se", "ne": "size_ne_sw", "sw": "size_ne_sw"}

    def watch_edges(self):
        """Tk doesn't reliably tell the border frames when the mouse is on them (moving out
        from the inside, they're never 'entered'), which left the resize cursor stuck and made
        the right/bottom edges unclickable. So the border asks Windows directly, about 30 times
        a second: where is the mouse, and is the left button down?"""
        try:
            import ctypes
            from ctypes import wintypes
            u = ctypes.windll.user32
            if not self.win.winfo_exists():
                return
        except (tk.TclError, Exception):
            return
        u.LoadCursorW.argtypes, u.LoadCursorW.restype = [wintypes.HINSTANCE, ctypes.c_void_p], ctypes.c_void_p
        u.SetCursor.argtypes = [ctypes.c_void_p]
        left = 0x02 if u.GetSystemMetrics(23) else 0x01  # the "left" button, even if swapped
        state = u.GetAsyncKeyState(left)
        down = bool(state & 0x8000)
        px, py = self.win.winfo_pointerxy()
        if self._resize:
            if down:
                self.resize_to(px, py)
            else:  # let go: the window takes the outline's size
                self._resize = None
                self.finish_resize()
        else:
            z = self.follow_pointer()
            if z and (down or state & 1) and not self._was_down:  # pressed on the border
                w = self.win
                self._resize = (z, px, py, w.winfo_x(), w.winfo_y(),
                                w.winfo_width(), w.winfo_height())
        self._was_down = down
        self.win.after(15 if self._resize else 30, self.watch_edges)

    def border_press(self, e):
        """A press anywhere in the window: on the border, it starts a resize (and not, on the
        title bar's edge, a move)."""
        if self._resize or self.maximized:
            return
        z = self.zone(e.x_root, e.y_root)
        if z:
            w = self.win
            self._move, self.half = None, False  # (resized: no longer a snapped half)
            self._resize = (z, e.x_root, e.y_root, w.winfo_x(), w.winfo_y(),
                            w.winfo_width(), w.winfo_height())

    def follow_pointer(self):
        """The resize cursor while the mouse is on the border (the arrow back once it's off),
        and the same for Tk: the border frames, and whatever part of the window the mouse is
        on - the border's inner pixels are over the title bar or the page, which would
        otherwise put their own cursor back each time the mouse moves. Returns the border
        zone the mouse is on ("" if none)."""
        import ctypes
        from ctypes import wintypes
        u = ctypes.windll.user32
        u.LoadCursorW.argtypes, u.LoadCursorW.restype = [wintypes.HINSTANCE, ctypes.c_void_p], ctypes.c_void_p
        u.SetCursor.argtypes = [ctypes.c_void_p]
        try:
            z = "" if self.maximized or not self.pointer_on_window() else self.zone()
            under = self.win.winfo_containing(*self.win.winfo_pointerxy()) if z else None
        except (tk.TclError, KeyError):
            return ""
        want = self.TK_CURSORS.get(z, "")
        if z:
            u.SetCursor(u.LoadCursorW(None, self.SIZE_CURSORS[z]))
        elif self._zone_shown:  # just left the border: normal arrow back
            u.SetCursor(u.LoadCursorW(None, 32512))  # IDC_ARROW
        if z != self._zone_shown:  # tell Tk the same, so it never sets a different one
            for f in self.edges:
                f.config(cursor=want)
        for w, own in list(self._borrowed.items()):  # off the border, or another part now:
            if w is not under or not z:  # its own cursor back
                try:
                    w.config(cursor=own)
                except tk.TclError:
                    pass
                del self._borrowed[w]
        if z and under is not None and under not in self.edges:
            try:
                if under not in self._borrowed:
                    self._borrowed[under] = under.cget("cursor")
                if str(under.cget("cursor")) != want:
                    under.config(cursor=want)
            except tk.TclError:
                pass
        self._zone_shown = z
        return z

    def pointer_on_window(self):
        """Is the mouse over this window (and not over another window covering it)?"""
        import ctypes
        from ctypes import wintypes
        u = ctypes.windll.user32
        pt = wintypes.POINT()
        u.GetCursorPos(ctypes.byref(pt))
        return u.GetAncestor(u.WindowFromPoint(pt), 2) == self.hwnd()  # GA_ROOT

    def zone(self, px=None, py=None):
        """Which part of the border the mouse (or the point px, py on the screen) is on:
        "n", "se"... ("" if none)."""
        w = self.win
        if px is None:
            px, py = w.winfo_pointerxy()
        x, y = px - w.winfo_rootx(), py - w.winfo_rooty()
        W, H, G, C = w.winfo_width(), w.winfo_height(), self.GRIP, self.CORNER
        v = "n" if y < G else "s" if y >= H - G else ""
        h = "w" if x < G else "e" if x >= W - G else ""
        if v and not h:  # near a corner along the top/bottom edge
            h = "w" if x < C else "e" if x >= W - C else ""
        if h and not v:
            v = "n" if y < C else "s" if y >= H - C else ""
        return v + h

    def resize_to(self, mx, my):
        """Follow the mouse (at mx, my) with the edge/corner grabbed in self._resize - with
        an outline, like Windows 98 (its "show window contents while dragging" was off): Tk
        lays a window out again in several quick passes, so resizing the window itself all
        the way showed parts vanishing, smears and the screen behind it. The window takes
        the outline's size once, when the mouse lets go (finish_resize)."""
        z, px, py, x, y, W, H = self._resize
        dx, dy = mx - px, my - py
        mw, mh = self.min_size
        if "e" in z:
            W = max(mw, W + dx)
        if "s" in z:
            H = max(mh, H + dy)
        if "w" in z:
            nw = max(mw, W - dx)
            x, W = x + W - nw, nw
        if "n" in z:
            nh = max(mh, H - dy)
            y, H = y + H - nh, nh
        self.show_outline((x, y, W, H), z)

    _CHECKS = {}  # (width, height) -> a checkered picture for the outline's sides

    def show_outline(self, rect, zone=""):
        """Windows 98's resize outline: a 4 px checkered frame where the window will be (rect:
        x, y, w, h), drawn by four small borderless windows on top. None: take it away."""
        bars = getattr(self, "_bars", None)
        if rect is None:
            for bar in bars or ():
                bar.destroy()
            self._bars, self._target = None, None
            return
        self._target = rect
        T = 4
        if not bars:
            sw, sh = self.win.winfo_screenwidth(), self.win.winfo_screenheight()
            bars = []
            for w, h in ((sw, T), (sw, T), (T, sh), (T, sh)):
                key = (w, h)
                if key not in self._CHECKS:
                    img = Image.new("RGB", key)
                    img.putdata([(0, 0, 0) if (i % w + i // w) % 2 else (255, 255, 255)
                                 for i in range(w * h)])
                    self._CHECKS[key] = ImageTk.PhotoImage(img)
                bar = tk.Toplevel(self.win)
                bar.overrideredirect(True)
                bar.attributes("-topmost", True)
                tk.Label(bar, image=self._CHECKS[key], bd=0, highlightthickness=0,
                         cursor=self.TK_CURSORS.get(zone, "")).pack()
                bars.append(bar)
            self._bars = bars
        x, y, W, H = rect
        for bar, (bx, by, bw, bh) in zip(bars, ((x, y, W, T), (x, y + H - T, W, T),
                                               (x, y + T, T, H - 2 * T),
                                               (x + W - T, y + T, T, H - 2 * T))):
            bar.geometry(f"{bw}x{max(bh, 1)}+{bx}+{by}")
        bars[0].update_idletasks()

    def finish_resize(self):
        """The mouse let go: the window takes the outline's size, laid out and drawn at once."""
        rect = getattr(self, "_target", None)
        self.show_outline(None)
        if rect:
            x, y, W, H = rect
            self.win.geometry(f"{W}x{H}+{x}+{y}")
            self.win.update_idletasks()


class ClassicProgress(tk.Canvas):
    """Classic Windows progress bar: a thin sunken box that fills left to right with blocks
    in the title bar's colour, one whole block at a time. Takes value= / maximum= like
    ttk.Progressbar."""
    H, BLOCK, GAP, PAD = 20, 8, 2, 2

    def __init__(self, parent, maximum=100, value=0):
        super().__init__(parent, height=self.H, width=1, bg=BG, highlightthickness=0)
        self._max, self._value, self._shown = float(maximum), float(value), None
        self.bind("<Configure>", lambda e: self.draw(force=True))

    def configure(self, cnf=None, **kw):
        if "maximum" in kw:
            self._max = float(kw.pop("maximum"))
        if "value" in kw:
            self._value = float(kw.pop("value"))
        if cnf or kw:
            super().configure(cnf, **kw)
        self.draw()

    config = configure

    def draw(self, force=False):
        W, H = self.winfo_width(), self.winfo_height()
        x0, x1 = 1 + self.PAD, W - 1 - self.PAD
        step = self.BLOCK + self.GAP
        total = max(1, (x1 - x0 + self.GAP) // step)
        frac = min(max(self._value / self._max, 0), 1) if self._max > 0 else 0
        n = int(frac * total + 1e-9)  # whole blocks only, like the real thing
        if n == self._shown and not force:
            return  # nothing visible changed: skip redrawing
        self._shown = n
        self.delete("all")
        self.create_line(0, H - 1, 0, 0, W - 1, 0, fill=thin_shadow())  # sunken edge
        self.create_line(1, H - 1, W - 1, H - 1, W - 1, 0, fill=EDGE_LIGHT)
        # the blocks: the title bar's colour (its strong end), in every theme - navy in
        # Windows 98, teal in Pink, black in Jungle, your colour in Custom...
        color = untranslated(CAPTION_ACTIVE[0], "box")
        for i in range(n):
            bx = x0 + i * step
            self.create_rectangle(bx, 1 + self.PAD, bx + self.BLOCK, H - 1 - self.PAD,
                                  fill=color, outline="")


_TK_FONTS = {}


def font_of(font, widget):
    """A tkfont.Font for a font description (kept, so each is made once per Tk)."""
    key = (id(widget.tk), str(font))
    if key not in _TK_FONTS:
        _TK_FONTS[key] = tkfont.Font(widget, font=font)
    return _TK_FONTS[key]


def engraved_colors():
    """(light, dark) of greyed-out text: the theme's 3D edge colours, like Windows 98. (In
    the dark theme its edges are too close to the face: there, like the other themes' look -
    readable text with a soft copy 1 px down and right - a mid grey over a near-black.)"""
    if THEME == "mono":  # (black would look like normal text; the white copy can't show)
        return "#FFFFFF", "#808080"
    if THEME == "dark":
        return "#636363", "#1A1A1A"
    return theme_color(EDGE_LIGHT, "edge"), theme_color(EDGE_SHADOW, "edge")


def draw_engraved(canvas, x, y, text, font=FONT, underline=-1, anchor="nw", width=0,
                  justify="left"):
    """Greyed-out text the way Windows 98 draws it, engraved into the face: the text in the
    edges' shadow colour over a copy in their light colour 1 px down and right. (width:
    wrap at that many pixels; underline only with anchor "nw".)"""
    f = font_of(font, canvas)
    for d, color in zip((1, 0), engraved_colors()):
        canvas.create_text(x + d, y + d, text=text, font=font, fill=color, anchor=anchor,
                           width=width, justify=justify)
        if 0 <= underline < len(text) and anchor == "nw":
            ux = x + d + f.measure(text[:underline])
            uy = y + d + f.metrics("ascent") + 1
            canvas.create_rectangle(ux, uy, ux + f.measure(text[underline]), uy + 1,
                                    fill=color, outline="")


class EngravedLabel(tk.Canvas):
    """A greyed-out note such as "(not used by PNG)", its text engraved into the face like
    Windows 98's greyed-out text. Works like a tk.Label for its text, anchor ("center" or
    "w"), wraplength and justify, and is the same size. Given another text colour with fg
    (a red error message), it shows plain text in that colour instead."""
    GREYS = ("#666666", "#888888", "#999999")  # (the greys the notes used to be: engraved)

    def __init__(self, parent, text="", font=FONT, anchor="center", wraplength=0,
                 justify="left", fg=None):
        super().__init__(parent, bg=BG, highlightthickness=0, bd=0, width=0, height=0)
        self.opts = {"text": text, "font": font, "anchor": anchor, "wraplength": wraplength,
                     "justify": justify, "fg": fg}
        self.bind("<Configure>", lambda e: self.draw(resize=False))
        self.draw()

    def configure(self, cnf=None, **kw):
        kw = {**(cnf or {}), **kw}
        for key in list(kw):
            if key in self.opts:
                self.opts[key] = kw.pop(key)
        if kw:
            super().configure(kw)
        self.draw()
    config = configure

    def cget(self, key):
        return self.opts[key] if key in self.opts else super().cget(key)

    def draw(self, resize=True):
        self.delete("all")
        o = self.opts
        text = str(o["text"])
        if resize:  # a label's size, so the notes sit where the old labels did
            if not text:
                tk.Canvas.configure(self, width=0, height=0)
                return
            ref = tk.Label(self, text=text, font=o["font"], wraplength=o["wraplength"],
                           justify=o["justify"])
            tk.Canvas.configure(self, width=ref.winfo_reqwidth(), height=ref.winfo_reqheight())
            ref.destroy()
        if not text:
            return
        f = font_of(o["font"], self)
        empty = tk.Label(self, text="x", font=o["font"])  # the space a label leaves round
        inset = (empty.winfo_reqwidth() - f.measure("x")) // 2  # its text
        top = (empty.winfo_reqheight() - f.metrics("linespace")) // 2
        empty.destroy()
        width = int(o["wraplength"] or 0)
        if o["anchor"] == "w":
            x, anchor = inset, "nw"
        else:
            x, anchor = max(self.winfo_width(), int(self.cget("width"))) // 2, "n"
        fg = o["fg"]
        if fg and _norm(fg) not in self.GREYS:  # (an error: plain, in its own colour)
            self.create_text(x, top, text=text, font=o["font"], fill=fg, anchor=anchor,
                             width=width, justify=o["justify"])
        else:
            draw_engraved(self, x, top, text, o["font"], anchor=anchor, width=width,
                          justify=o["justify"])


class RaisedEdge(tk.Frame):
    """A raised 3D edge in the theme's own edge colours - 2 px light top / left, 1 px shadow
    and 2 px dark bottom / right, then 1 px of face - the same edge as the tabs and the page
    border. (Tk's own raised edge works its shadow out from the face colour, so it doesn't
    match them in themes with coloured edges.) Put the contents in .inner. It can also be
    shown pressed in, like a pushed button."""

    def __init__(self, parent, **kw):
        super().__init__(parent, bg=EDGE_DARK, **kw)  # outer bottom / right
        self.pressed = False
        self.top_left = tk.Frame(self, bg=EDGE_LIGHT)  # outer top / left
        self.top_left.pack(fill="both", expand=True, padx=(0, 2), pady=(0, 2))
        self.bottom_right = tk.Frame(self.top_left, bg=EDGE_SHADOW)  # inner bottom / right
        self.bottom_right.pack(fill="both", expand=True, padx=(2, 0), pady=(2, 0))
        self.inner_top_left = tk.Frame(self.bottom_right, bg=BG)  # inner top / left
        self.inner_top_left.pack(fill="both", expand=True, padx=(0, 1), pady=(0, 1))
        self.inner = tk.Frame(self.inner_top_left, bg=BG)
        self.inner.pack(fill="both", expand=True, padx=(1, 0), pady=(1, 0))

    def set_thin(self, thin):
        """A 1 px outline instead of 2 (Black and White's greyed-out buttons) - the size
        stays the same: the spare pixel goes inside, in the face colour."""
        if thin == getattr(self, "thin", False):
            return
        self.thin = thin
        a, b = (1, 2) if thin else (2, 1)
        self.top_left.pack_configure(padx=(0, a), pady=(0, a))
        self.bottom_right.pack_configure(padx=(a, 0), pady=(a, 0))
        self.inner_top_left.pack_configure(padx=(0, b), pady=(0, b))
        self.inner.pack_configure(padx=(b, 0), pady=(b, 0))

    def set_pressed(self, pressed):
        """Pressed in: shadow and dark top / left, light bottom / right (like Tk's sunken)."""
        if pressed == self.pressed:
            return
        self.pressed = pressed
        colors = ((EDGE_LIGHT, EDGE_SHADOW, BG, EDGE_DARK) if pressed and THEME != "mono" else
                  (EDGE_DARK, EDGE_LIGHT, EDGE_SHADOW, BG))  # (Black and White: the black
        # outline stays all round when pressed - only the text moves, down and right)
        for frame, color in zip((self, self.top_left, self.bottom_right, self.inner_top_left),
                                colors):
            tk.Frame.configure(frame, bg=color)

    def redraw(self):
        """Colours again after a theme change (the pressed look's colours aren't the ones
        the frames were made with)."""
        pressed, self.pressed = self.pressed, not self.pressed
        self.set_pressed(pressed)


class ClassicButton(RaisedEdge):
    """The app's raised button (Add, Convert, OK...): a flat Tk button inside a RaisedEdge,
    so it's shaded like the tabs in every theme. It pushes in while it's held down, and
    works like a tk.Button: its options (text, state, command...) and invoke() are the
    button's; pack / grid / place it like any widget."""

    def __init__(self, parent, **kw):
        super().__init__(parent)
        # (1 px of face round it: where Tk's button had its focus ring, so the size is the same)
        self.button = tk.Button(self.inner, relief="flat", bd=0, highlightthickness=0, bg=BG,
                                activebackground="#F5F3E8", **kw)
        self.button.pack(fill="both", expand=True, padx=1, pady=1)
        self.button.classic = self
        tags = list(self.button.bindtags())  # after Tk's own button bindings: follow its
        tags.insert(tags.index("Button") + 1, "ClassicButton")  # pressed / released state
        self.button.bindtags(tuple(tags))
        for seq in ("<ButtonPress-1>", "<ButtonRelease-1>", "<Enter>", "<Leave>"):
            self.button.bind_class("ClassicButton", seq, ClassicButton._follow)
        self.engraved = None  # greyed out: its text engraved, drawn over the button
        self.engrave()

    @staticmethod
    def _follow(e):
        classic = getattr(e.widget, "classic", None)
        if classic is not None:
            classic.follow()

    def follow(self):
        """Look pushed in while Tk's button is (held down with the mouse over it)."""
        try:
            down = str(self.button.cget("relief")) == "sunken"
            self.set_pressed(down)
            tk.Frame.configure(self.inner, bg="#F5F3E8" if down else BG)  # (its pressed face)
        except tk.TclError:  # (closed by its own click)
            pass

    def redraw(self):
        super().redraw()
        self.follow()
        self.engrave()

    def engrave(self):
        """Greyed out, a text button shows its text engraved into the face, like Windows 98
        (Tk would just draw it grey): drawn on a canvas over the button while it's off."""
        try:
            off = (str(self.button.cget("state")) == "disabled"
                   and not str(self.button.cget("image")))
        except tk.TclError:
            return
        self.set_thin(off and THEME == "mono")  # (Black and White: a thinner outline when off)
        if not off:
            if self.engraved is not None:
                self.engraved.place_forget()
            return
        if self.engraved is None:
            self.engraved = tk.Canvas(self.inner, bg=BG, highlightthickness=0, bd=0)
            self.engraved.bind("<Configure>", lambda e: self.draw_engraved())
        # (the button's own area: inside the 1 px of face round it)
        self.engraved.place(x=1, y=1, relwidth=1, relheight=1, width=-2, height=-2)
        self.draw_engraved()

    def draw_engraved(self):
        c = self.engraved
        c.delete("all")
        text, font = str(self.button.cget("text")), self.button.cget("font")
        f = font_of(font, c)
        draw_engraved(c, (c.winfo_width() - f.measure(text)) // 2,
                      (c.winfo_height() - f.metrics("linespace")) // 2, text, font,
                      int(self.button.cget("underline")))

    # its own options, not the button's: the cursor (What's This? sets and puts back each
    # widget's), and the colours (a theme change recolours the button on its own)
    OWN = {"cursor", "bg", "background", "fg", "foreground", "activebackground",
           "activeforeground", "disabledforeground", "highlightbackground", "highlightcolor"}

    def configure(self, cnf=None, **kw):
        kw = {**(cnf or {}), **kw}
        own = {k: kw.pop(k) for k in list(kw) if k in self.OWN}
        if own:
            tk.Frame.configure(self, own)
        if not kw:
            return None
        out = self.button.configure(kw)
        if {"state", "text", "font", "underline", "image"} & set(kw):
            self.engrave()
        return out
    config = configure

    def cget(self, key):
        return tk.Frame.cget(self, key) if key in self.OWN else self.button.cget(key)
    __getitem__ = cget

    def __setitem__(self, key, value):
        self.button.configure({key: value})

    def invoke(self):
        return self.button.invoke()

    def focus_set(self):  # the keyboard goes to the button (Enter / Space press it)
        self.button.focus_set()
    focus = focus_set


def xp_button(parent, text, cmd, bold=False):
    """The app's raised button (Add, Convert, OK...)."""
    return ClassicButton(parent, text=text, command=cmd, padx=8,
                         font=(FONT[0], FONT[1], "bold" if bold else "normal"))


# ---- sounds: original XP-style chimes, made by the app itself (no sound files shipped) ----
def make_sound(kind, rate=44100):
    """WAV bytes for "done" (a soft bell-like chime going up) or "error" (a short, warm, low
    chord going down), with a little room echo - in the spirit of Windows XP's sounds."""
    import numpy as np
    import wave
    if kind == "done":  # (start s, frequencies Hz), bright and gentle
        notes, length, decay = [(0.00, (784.0,)), (0.11, (1174.7,))], 1.0, 5.0
    else:  # a fifth, then a lower fifth
        notes, length, decay = [(0.00, (329.6, 493.9)), (0.13, (220.0, 329.6))], 1.0, 6.0
    t = np.arange(int(rate * length)) / rate
    out = np.zeros_like(t)
    for start, freqs in notes:
        tt = np.clip(t - start, 0, None)
        on = t >= start
        for i, f in enumerate(freqs):
            level = 1.0 if i == 0 else 0.45
            # bell-like: the note plus quieter overtones that fade faster
            tone = (np.sin(2 * np.pi * f * tt)
                    + 0.35 * np.sin(2 * np.pi * 2 * f * tt) * np.exp(-tt * decay * 1.5)
                    + 0.12 * np.sin(2 * np.pi * 3 * f * tt) * np.exp(-tt * decay * 3))
            out += on * level * tone * np.exp(-tt * decay) * np.minimum(tt / 0.004, 1)  # soft start
    for delay, gain in ((0.045, 0.25), (0.09, 0.14), (0.14, 0.08)):  # small-room echo
        d = int(rate * delay)
        out[d:] += gain * out[:-d]
    out *= 0.45 / np.max(np.abs(out))  # comfortable loudness, never clipping
    out *= np.minimum((len(t) - np.arange(len(t))) / (rate * 0.05), 1)  # fade out the tail
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes((out * 32767).astype("<i2").tobytes())
    return buf.getvalue()


_SOUNDS = {}
SOUNDS_ON = True  # Settings > Sound effects (read from the settings file at start)


def play_sound(kind):
    """Play "done" or "error" without waiting for it (Windows only; silently skipped if the
    sound can't play, e.g. no speakers)."""
    if sys.platform != "win32" or kind is None or not SOUNDS_ON:
        return

    def go():
        try:
            import winsound
            if kind not in _SOUNDS:
                _SOUNDS[kind] = make_sound(kind)
            winsound.PlaySound(_SOUNDS[kind], winsound.SND_MEMORY)
        except Exception:
            pass
    threading.Thread(target=go, daemon=True).start()


def dialog(title, message, buttons=("OK",), parent=None, sound=None):
    """Message box in the app's own style (same look as the Rename box), used instead of
    Windows' standard ones. Waits for an answer and returns the clicked button's text
    (None if closed). Enter = first button; Escape = No/Cancel if there is one, else close."""
    parent = parent or tk._default_root
    win = tk.Toplevel(parent)
    win.configure(bg=BG)
    win.resizable(False, False)
    win.transient(parent)
    result = [None]

    def choose(text):
        result[0] = text
        win.destroy()

    cancel = next((t for t in buttons if t in ("No", "Cancel")), None)
    body = ClassicWindow(win, title, lambda: choose(cancel), resizable=False, taskbar=False).body
    tk.Frame(body, bg=BG, width=280, height=0).pack()  # keeps short messages from looking cramped
    tk.Label(body, text=message, bg=BG, font=FONT, justify="left", wraplength=420
             ).pack(anchor="w", padx=12, pady=(12, 8))  # same margins as the Rename box
    row = tk.Frame(body, bg=BG)
    row.pack(pady=(4, 12))
    btns = [xp_button(row, text, lambda t=text: choose(t)) for text in buttons]
    for b in btns:
        b.pack(side="left", padx=4)
    win.bind("<Return>", lambda e: choose(buttons[0]))
    win.bind("<Escape>", lambda e: choose(cancel))

    win.update_idletasks()  # centre over the main window (or the screen if it's hidden)
    w, h = win.winfo_reqwidth(), win.winfo_reqheight()
    if parent.winfo_ismapped():
        x = parent.winfo_rootx() + (parent.winfo_width() - w) // 2
        y = parent.winfo_rooty() + (parent.winfo_height() - h) // 3
    else:
        x, y = (win.winfo_screenwidth() - w) // 2, (win.winfo_screenheight() - h) // 3
    win.geometry(f"+{max(0, x)}+{max(0, y)}")
    win.focus_force()  # windows without Windows' title bar don't take the keyboard by themselves
    btns[0].focus_set()
    win.grab_set()
    play_sound(sound)  # "done" / "error", as the box appears
    win.wait_window()
    return result[0]


def brush_icon():
    """A little Windows 98-style paintbrush (11 x 16), shaded to look 3D like the icons of
    the time - lit from the top left: the yellow-green handle bright on its left and dark
    olive on its right, the metal band light on top and shadowed below, the white bristles
    grey on their shaded side; black outline."""
    rows = ["....KKK....",
            "...KHYGK...",
            "...KHYGK...",
            "...KHYGK...",
            "...KHYGK...",
            "...KHYGK...",
            "..KKHYGKK..",
            ".KHHYYYGGK.",
            ".KKKKKKKKK.",
            ".KWSSSSSDK.",
            ".KDDDDDDDK.",
            ".KKKKKKKKK.",
            ".KWKWKWKLK.",
            ".KWKWKWKLK.",
            ".KWWWWWWLK.",
            ".KKKKKKKKK."]
    colors = {"K": (0, 0, 0, 255), "H": (255, 255, 96, 255), "Y": (192, 200, 0, 255),
              "G": (104, 104, 0, 255), "W": (255, 255, 255, 255), "S": (208, 208, 208, 255),
              "D": (128, 128, 128, 255), "L": (176, 176, 176, 255)}
    im = Image.new("RGBA", (11, len(rows)), (0, 0, 0, 0))
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            if ch in colors:
                im.putpixel((x, y), colors[ch])
    return ImageTk.PhotoImage(im)


class PopupMenu:
    """Right-click menu drawn by the app itself, laid out like Windows' own (raised 3D edge
    like the buttons', 21 px items, a line between groups, blue highlight). Windows' menus
    draw their frame in the system's light colour, which in dark mode showed as a thick
    white border; this one is coloured like everything else, so it's the same in both modes.
    Same calls as tk.Menu: add_command(label=, command=), add_separator(), tk_popup(x, y)."""

    def __init__(self, parent, on_close=None, indent=22, refill=None):
        self.parent = parent.winfo_toplevel()
        self.indent = indent  # space left and right of the items' text
        self.items = []
        self.top = None
        self.on_close = on_close  # called when the menu closes (the menu bar un-highlights)
        # refill: a list that stays open when an item is chosen (the Theme list) - the item
        # acts, then the list redraws itself from refill() (which item is pressed in...);
        # it closes when anything else is clicked
        self.refill = refill

    def add_command(self, label, command, checked=False, button=None):
        """checked: shown pressed in - e.g. the theme in use. button: (picture, command) for
        a small button at the item's right end (only on a checked item)."""
        self.items.append((label, command, checked, button))

    def add_separator(self):
        self.items.append(None)

    def tk_popup(self, x, y, min_width=0):
        top = self.top = tk.Toplevel(self.parent, bg=BG)
        top.keeps_owner_active = True  # its window stays "active" (blue title) while it's open
        top.withdraw()
        top.overrideredirect(True)
        top.transient(self.parent)
        # the same raised 3D edge as the buttons (RaisedEdge), light and shadow just as thick
        edge = RaisedEdge(top)
        edge.pack()
        self.body = tk.Frame(edge.inner, bg=BG)
        self.body.pack(padx=1, pady=1)
        self.min_width = min_width
        self.fill()
        top.update_idletasks()
        w, h = top.winfo_reqwidth(), top.winfo_reqheight()
        x = min(x, top.winfo_screenwidth() - w - 2)  # keep it on the screen
        y = y if y + h <= top.winfo_screenheight() else y - h
        top.geometry(f"+{max(0, x)}+{max(0, y)}")
        top.deiconify()
        top.lift()
        top.attributes("-topmost", True)
        top.focus_force()
        top.grab_set()  # clicks anywhere come here: outside the menu closes it
        top.bind("<ButtonPress>", self.on_press)
        top.bind("<Escape>", lambda e: self.close())
        top.bind("<FocusOut>", lambda e: top.after(50, self.check_focus))
        top.after(100, self.watch_foreground)

    def watch_foreground(self):
        """While open, ask Windows ~10 times a second whether this app is still in front: a
        popup like this isn't reliably told when another program takes over, and it would
        otherwise stay floating on top of that program."""
        if not self.top:
            return
        if sys.platform == "win32":
            import ctypes
            u, pid = ctypes.windll.user32, ctypes.c_ulong()
            u.GetWindowThreadProcessId(u.GetForegroundWindow(), ctypes.byref(pid))
            if pid.value != os.getpid():  # another program is in front now
                self.close(give_back=False)
                return
        self.top.after(100, self.watch_foreground)

    def on_press(self, e):
        top = self.top
        inside = (top.winfo_rootx() <= e.x_root < top.winfo_rootx() + top.winfo_width()
                  and top.winfo_rooty() <= e.y_root < top.winfo_rooty() + top.winfo_height())
        if not inside:
            self.close()

    def check_focus(self):  # switched to another program: close, like a real menu
        try:
            f = self.top.focus_get() if self.top else None
        except (KeyError, tk.TclError):
            f = None
        if self.top and (f is None or f.winfo_toplevel() is not self.top):
            self.close(give_back=False)  # the other program keeps the focus

    def choose(self, command, close=False):
        """An item was clicked. A list that stays open (refill) acts and redraws itself;
        any other menu - or close=True - closes first."""
        if self.refill and not close:
            command()
            if self.top:  # (still open: redraw it, e.g. with the new theme pressed in)
                self.items = []
                for item in self.refill():
                    if item is None:
                        self.add_separator()
                    else:
                        self.add_command(*item)
                self.fill()
                self.top.focus_force()  # keeps the keyboard (Esc) and the outside-click watch
            return
        self.close()
        command()

    def close(self, give_back=True):
        """Close the menu and hand the keyboard back to its window - the menu had it (for
        Escape), and without this the window stayed greyed out as if it had lost focus."""
        if self.top:
            top, self.top = self.top, None
            top.grab_release()
            top.destroy()
            if self.on_close:
                self.on_close()
            try:
                if give_back:
                    self.parent.focus_force()
                else:  # another program took over: let the window grey out its title bar
                    self.parent.event_generate("<FocusOut>")
            except tk.TclError:
                pass

    def grab_release(self):  # (tk.Menu has it; nothing to do here)
        pass

    def fill(self):
        """Make the items' rows (again, after a choice in a list that stays open)."""
        body = self.body
        for w in body.winfo_children():
            w.destroy()
        for item in self.items:
            if item is None:  # etched line: grey over white
                tk.Frame(body, bg="#A0A0A0", height=1).pack(fill="x", padx=1, pady=(3, 0))
                tk.Frame(body, bg="#FFFFFF", height=1).pack(fill="x", padx=1, pady=(0, 3))
                continue
            label, command, checked, button = item
            if checked:
                self.latched_row(body, label, command, button)
                continue
            row = tk.Label(body, text=label, bg=BG, fg="black", font=FONT, anchor="w",
                           padx=self.indent, pady=3)
            row.pack(fill="x")
            if command is None:  # not available: greyed out, like Windows' disabled items
                row.config(fg="#808080")
                continue
            row.bind("<Enter>", lambda e, r=row: r.config(
                bg=select_colors()[0], fg=select_colors()[1]))
            row.bind("<Leave>", lambda e, r=row: r.config(bg=BG, fg="black"))
            row.bind("<ButtonRelease-1>", lambda e, c=command: self.choose(c))
        tk.Frame(body, bg=BG, width=self.min_width, height=0).pack()

    def latched_row(self, body, label, command, button=None):
        """An item shown pressed in, exactly like a pressed menu bar button (Home): the same
        sunken edge on the plain face, the same size as the other items, its text in place.
        button: (picture, command) - a small clickable picture at its right end."""
        row = tk.Label(body, text=label, bg=BG, fg="black", font=FONT, anchor="w",
                       padx=self.indent, pady=3, relief="sunken", bd=2)
        row.pack(fill="x")
        if command is not None:
            row.bind("<ButtonRelease-1>", lambda e: self.choose(command))
        if button:
            picture, action = button
            icon = tk.Label(body, image=picture, bg=BG, bd=0, padx=0, pady=0)
            icon.place(in_=row, relx=1.0, rely=0.5, x=-5, anchor="e")

            def over(e):
                return 0 <= e.x < icon.winfo_width() and 0 <= e.y < icon.winfo_height()

            def show(pressed):  # pressed: the picture sinks 1 px down and right, like a button
                icon.place_configure(x=-4 if pressed else -5, y=1 if pressed else 0)

            def up(e):  # released on it: act (going somewhere else: the list closes)
                show(False)
                if over(e):
                    self.choose(action, close=True)
            icon.bind("<ButtonPress-1>", lambda e: show(True))
            # like every button: held down, it pops up while the mouse is off it and sinks
            # again when the mouse comes back
            icon.bind("<B1-Motion>", lambda e: show(over(e)))
            icon.bind("<ButtonRelease-1>", up)


class MenuBar(tk.Frame):
    """Classic menu bar under the title bar (File  Edit  View ... style): each title has its
    first letter underlined and works with a click or Alt + that letter. The titles are
    flat buttons that press in when clicked, like every other button in the app (the way
    Windows 98's menu bars did). menus: a list of (title, "menu", items) - items is a
    function returning the drop-down list's items, [(label, command or None = greyed out)
    or None = line]; the title stays pressed in while its list is open, and the list closes
    when a choice is made - or (title, "list", items): the same, but the list stays open while
    its choices are tried (the Theme list) - or (title, "page", command): acts when released."""

    def __init__(self, parent, menus):
        super().__init__(parent, bg=BG)
        self.labels = {}
        bold = tkfont.Font(family=FONT[0], size=FONT[1], weight="bold")
        for title, kind, action in menus:
            # each title sits in a box as wide as its bold version, so making the current
            # page's title bold (set_current) doesn't push the others along
            cell = tk.Frame(self, bg=BG)
            cell.pack(side="left")
            btn = tk.Button(cell, text=title, bg=BG, fg="black", font=FONT, underline=0,
                            relief="flat", bd=2, padx=4, pady=0, highlightthickness=0,
                            activebackground=BG, takefocus=0)
            cell.config(width=bold.measure(title) + 16, height=btn.winfo_reqheight())
            cell.pack_propagate(False)
            btn.pack(fill="both", expand=True)
            self.labels[title] = btn
            if kind == "page":  # a real button: presses in, acts when released on it
                btn.config(command=action)
                key = lambda e=None, b=btn: self.flash(b)  # noqa: E731
            else:  # presses in and opens its list straight away
                stay = kind == "list"
                btn.bind("<ButtonPress-1>", lambda e, b=btn, it=action, s=stay: self.open(b, it, s))
                key = lambda e=None, b=btn, it=action, s=stay: self.open(b, it, s)  # noqa: E731
            self.winfo_toplevel().bind(f"<Alt-{title[0].lower()}>", key)

    def set_current(self, title):
        """Show which page is open: its title in bold."""
        for name, btn in self.labels.items():
            btn.config(font=(FONT[0], FONT[1], "bold") if name == title else FONT)

    def flash(self, btn):  # Alt + letter on a page title: a short press, then act
        btn.config(relief="sunken")
        btn.after(120, lambda: (btn.config(relief="flat"), btn.invoke()))
        return "break"

    def open(self, btn, items, stay=False):
        # pressed in while its list is open - the same look as a pressed Home button
        btn.config(relief="sunken")
        # stay: the list stays open while you try its choices (e.g. themes): it closes when
        # you click anywhere else, or press Esc
        menu = PopupMenu(self, indent=8, on_close=lambda: btn.config(relief="flat"),
                         refill=items if stay else None)
        entries = items()
        for item in entries:
            if item is None:
                menu.add_separator()
            else:
                menu.add_command(*item)
        # the text starts 8 px in (left-aligned), but the list keeps the usual width: the
        # longest item with 22 px on both sides (+ each item's 2 px border), like the
        # right-click menus
        font = tkfont.Font(font=FONT)
        widest = max((font.measure(e[0]) for e in entries if e), default=0)
        menu.tk_popup(btn.winfo_rootx(), btn.winfo_rooty() + btn.winfo_height(),
                      min_width=widest + 44 + 4)
        return "break"


SHOW_HELP = True  # Settings > Help: the ? (What's This?) buttons, on or off


class WhatsThis:
    """Windows 95 / 98's "What's This?" help for a dialog: a ? button next to X; clicking it
    turns the pointer into an arrow with a question mark, and the next click on any part of
    the dialog shows a short note about it (lookup(widget, x_root, y_root) -> text or None).
    The note closes with the next click or key."""

    def __init__(self, win, chrome, lookup):
        self.win, self.chrome, self.lookup = win, chrome, lookup
        self.tag = f"WhatsThis{id(self)}"  # put in front of every widget's own events
        self.active, self.note, self.saved = False, None, {}
        chrome.on_help = self.start if SHOW_HELP else None  # (turned off in Settings: no ?)
        win.bind_class(self.tag, "<ButtonPress-1>", self.pick)
        win.bind_class(self.tag, "<ButtonPress-3>", lambda e: self.stop() or "break")
        win.bind_class(self.tag, "<Escape>", lambda e: self.stop() or "break")  # (not the dialog)
        # a note closes with the next click anywhere in the dialog, or any key
        for event in ("<ButtonPress>", "<KeyPress>"):
            win.bind(event, lambda e: self.close_note(), add="+")
        chrome.draw()

    def start(self):
        if self.active:  # ? again: leave help mode
            self.stop()
            return
        self.close_note()
        self.active = True
        for w in all_widgets(self.win):
            try:
                self.saved[w] = (w.cget("cursor"), w.bindtags())
                w.config(cursor="question_arrow")
                w.bindtags((self.tag,) + w.bindtags())
            except tk.TclError:
                pass

    def stop(self):
        self.active = False
        for w, (cursor, tags) in self.saved.items():
            try:
                w.config(cursor=cursor)
                w.bindtags(tags)
            except tk.TclError:
                pass
        self.saved = {}

    def pick(self, e):
        if e.widget is self.chrome.bar:  # the title bar (? again, X...): out of help mode
            self.stop()
            return None  # ... and the click still reaches its button
        widget = e.widget  # (a click inside a button is the button's)
        while isinstance(widget, tk.Misc) and not isinstance(widget, ClassicButton):
            widget = widget.master
        widget = widget if isinstance(widget, ClassicButton) else e.widget
        text = self.lookup(widget, e.x_root, e.y_root)
        self.stop()
        if text:
            self.show(text, e.x_root, e.y_root)
        return "break"

    def show(self, text, x, y):
        note = self.note = tk.Toplevel(self.win)
        note.overrideredirect(True)
        note.attributes("-topmost", True)
        tk.Label(note, text=text, bg="#FFFFE1", fg="black", relief="solid", bd=1, font=FONT,
                 justify="left", wraplength=230, padx=6, pady=4).pack()
        note.update_idletasks()
        x = min(x + 4, note.winfo_screenwidth() - note.winfo_reqwidth() - 4)
        note.geometry(f"+{x}+{y + 16}")
        note.bind("<ButtonPress>", lambda e: self.close_note())
        # a click on any part of the window closes it - caught first, before the part itself
        # (some keep the click to themselves), and still doing what it does
        self.note_tag = self.tag + "note"
        self.win.bind_class(self.note_tag, "<ButtonPress>", lambda e: self.close_note())
        self.tagged = []
        for w in all_widgets(self.win):
            try:
                w.bindtags((self.note_tag,) + w.bindtags())
                self.tagged.append(w)
            except tk.TclError:
                pass
        self._buttons_down = self.buttons_down()  # (the click that opened it is still down)
        self.win.after(15, self.watch_note)

    @staticmethod
    def buttons_down():
        """Which mouse buttons are down right now (left, right, middle), asked of Windows."""
        try:
            import ctypes
            state = ctypes.windll.user32.GetAsyncKeyState
            return tuple(bool(state(vk) & 0x8000) for vk in (0x01, 0x02, 0x04))
        except Exception:
            return (False, False, False)

    def watch_note(self):
        """While a note shows: a click outside the window - on another window, on the
        desktop - or switching to another program closes it, like Windows'. (Clicks on the
        window itself: see show.)"""
        note = self.note
        if note is None:
            return
        down = self.buttons_down()
        clicked = any(now and not before for now, before in zip(down, self._buttons_down))
        self._buttons_down = down
        try:  # the program in front isn't this one (this window, its owner, or the note)?
            import ctypes
            u = ctypes.windll.user32
            ours = [self.win, note] + ([self.win.master.winfo_toplevel()] if self.win.master else [])
            front = u.GetAncestor(u.GetForegroundWindow(), 2)  # GA_ROOT
            away = bool(front) and front not in {u.GetParent(w.winfo_id()) for w in ours}
        except (tk.TclError, Exception):
            away = False
        if clicked or away:
            self.close_note()
        else:
            self.win.after(15, self.watch_note)

    def close_note(self):
        if self.note:
            self.note.destroy()
            self.note = None
        for w in getattr(self, "tagged", ()):
            try:
                w.bindtags(tuple(t for t in w.bindtags() if t != self.note_tag))
            except tk.TclError:
                pass
        self.tagged = []


BASIC_COLORS = [  # the 48 "Basic colors" of Windows' classic colour picker
    "#FF8080", "#FFFF80", "#80FF80", "#00FF80", "#80FFFF", "#0080FF", "#FF80C0", "#FF80FF",
    "#FF0000", "#FFFF00", "#80FF00", "#00FF40", "#00FFFF", "#0080C0", "#8080C0", "#FF00FF",
    "#804040", "#FF8040", "#00FF00", "#008080", "#004080", "#8080FF", "#800040", "#FF0080",
    "#800000", "#FF8000", "#008000", "#008040", "#0000FF", "#0000A0", "#800080", "#8000FF",
    "#400000", "#804000", "#004000", "#004040", "#000080", "#000040", "#400040", "#400080",
    "#000000", "#808000", "#808040", "#808080", "#408080", "#C0C0C0", "#400040", "#FFFFFF",
]


def dropper_icon():
    """A Windows 98-style eyedropper (16 x 16): a dark rubber bulb at the top right, a glass
    tube down to the tip at the bottom left, lit from the top left."""
    rows = ["...........KKK..",
            "..........KDDDK.",
            "......K..KDDDDK.",
            ".......KKDDDDK..",
            ".......KKKDDK...",
            "......KSWKKK....",
            ".....KSWSK.K....",
            "....KSWSK.......",
            "...KSWSK........",
            "..KSWSK.........",
            ".KSWSK..........",
            ".KWSK...........",
            "KKKK............",
            "KK.............."]
    colors = {"K": (0, 0, 0, 255), "D": (96, 96, 96, 255), "S": (176, 196, 222, 255),
              "W": (255, 255, 255, 255)}
    im = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            if ch in colors:
                im.putpixel((x + 1, y + 1), colors[ch])
    return im


def color_dialog(parent, title, initial="#000085", beside=False):
    """(beside: open off to the right of the main window, partly outside it, instead of
    centred over it.)
    Colour picker laid out exactly like Windows 95 / 98's "Edit Colors" box (positions
    measured from it), in the app's own style: basic and custom colours on the left, the
    rainbow field, brightness bar, preview and Hue / Sat / Lum / Red / Green / Blue boxes on
    the right. Returns "#RRGGBB", or None if cancelled. The colours are drawn as pictures,
    so dark mode leaves them as they are; custom colours are remembered."""
    owner = parent.winfo_toplevel()
    win = tk.Toplevel(owner)
    win.configure(bg=BG)
    win.resizable(False, False)
    win.transient(owner)
    result = {"color": None}
    chrome = ClassicWindow(win, title, win.destroy, resizable=False, taskbar=False)
    body = chrome.body
    box = tk.Frame(body, bg=BG, width=429, height=309)
    box.pack()
    box.pack_propagate(False)
    FX, FY, FW, FH = 212, 7, 175, 187  # rainbow field (inside its edge)
    BX, BW = 404, 10  # brightness bar
    keep = []  # Tk forgets pictures nothing holds on to
    SMALL = FONT  # (MS Sans Serif 8: the original's own font)

    def solid(color, w, h):
        img = ImageTk.PhotoImage(Image.new("RGB", (w, h), color))
        keep.append(img)
        return img

    def edge1(c, x0, y0, x1, y1):  # 1 px sunken edge round the area [x0, x1] x [y0, y1]
        c.create_line(x0 - 1, y1 + 1, x0 - 1, y0 - 1, x1 + 2, y0 - 1, fill=thin_shadow())
        c.create_line(x0 - 1, y1 + 1, x1 + 1, y1 + 1, x1 + 1, y0 - 2, fill=EDGE_LIGHT)

    def edge2(c, x0, y0, x1, y1):  # 2 px sunken edge (the swatches'): grey + black, face + white
        c.create_line(x0 - 2, y1 + 2, x0 - 2, y0 - 2, x1 + 3, y0 - 2, fill=EDGE_SHADOW)
        c.create_line(x0 - 1, y1 + 1, x0 - 1, y0 - 1, x1 + 2, y0 - 1, fill=EDGE_DARK)
        c.create_line(x0 - 1, y1 + 1, x1 + 1, y1 + 1, x1 + 1, y0 - 2, fill=BG)
        c.create_line(x0 - 2, y1 + 2, x1 + 2, y1 + 2, x1 + 2, y0 - 3, fill=EDGE_LIGHT)

    def hls(col):
        return colorsys.rgb_to_hls(*[int(col[i:i + 2], 16) / 255 for i in (1, 3, 5)])

    h, l, s_ = hls(initial)
    state = {"h": h, "l": l, "s": s_, "pick": None}  # pick: ("basic" / "custom", index)
    saved = load_settings().get("custom_colors") or []
    customs = (list(saved) + [None] * 16)[:16]
    state["slot"] = next((i for i, c in enumerate(customs) if c is None), 0)

    # one canvas under everything: swatches, rainbow, bar and preview are drawn on it
    cv = tk.Canvas(box, width=429, height=309, bg=BG, highlightthickness=0)
    cv.place(x=0, y=0)

    def label(text, x, y, under=-1, anchor="nw"):
        tk.Label(box, text=text, bg=BG, font=SMALL, underline=under, padx=0,
                 pady=0).place(x=x, y=y, anchor=anchor)

    # ---- basic and custom colours: 20 x 20 sunken boxes (16 x 16 inside), 24 apart each way
    label("Basic colors:", 4, 2, 0)  # (near the top, level with the colour field)
    label("Custom colors:", 4, 186, 0)
    cells = []  # (kind, index, x, y) of each swatch's colour area (16 x 16)
    for i, col in enumerate(BASIC_COLORS):
        x, y = 10 + (i % 8) * 24, 27 + (i // 8) * 24
        cells.append(("basic", i, x, y))
    for i in range(16):
        x, y = 10 + (i % 8) * 24, 205 + (i // 8) * 24
        cells.append(("custom", i, x, y))
    swatch_items = {}

    def draw_swatch(kind, i, x, y):
        col = BASIC_COLORS[i] if kind == "basic" else (customs[i] or "#FFFFFF")
        if (kind, i) in swatch_items:
            cv.delete(swatch_items[(kind, i)])
        swatch_items[(kind, i)] = cv.create_image(x, y, image=solid(col, 16, 16), anchor="nw")
    for kind, i, x, y in cells:
        draw_swatch(kind, i, x, y)
        edge2(cv, x, y, x + 15, y + 15)
    mark = [cv.create_rectangle(0, 0, 0, 0, outline="#000000", width=1),
            cv.create_rectangle(0, 0, 0, 0, outline="#000000", width=1, dash=(1, 1))]

    # ---- the rainbow field (hue across, saturation down) and the brightness bar
    rainbow = Image.new("RGB", (FW, FH))
    rainbow.putdata([tuple(round(v * 255) for v in colorsys.hls_to_rgb(
        x / (FW - 1), 0.5, 1 - y / (FH - 1))) for y in range(FH) for x in range(FW)])
    img = ImageTk.PhotoImage(rainbow)
    keep.append(img)
    cv.create_image(FX, FY, image=img, anchor="nw")
    edge1(cv, FX, FY, FX + FW - 1, FY + FH - 1)
    edge1(cv, BX, FY, BX + BW - 1, FY + FH - 1)
    bar_item = cv.create_image(BX, FY, anchor="nw")
    arrow = cv.create_polygon(0, 0, 0, 0, 0, 0, fill="#000000")
    cross = [cv.create_line(0, 0, 0, 0, fill="#000000", width=2) for _ in range(4)]

    # ---- preview, and the numbers
    PX, PY, PW, PH = 212, 204, 58, 44  # (the number box under it lines up with Lum)
    # the colour, with its number (#RRGGBB) under it: type or paste one in, Enter
    preview_item = cv.create_image(PX, PY, anchor="nw")
    edge1(cv, PX, PY, PX + PW - 1, PY + PH - 1)
    hexbox = tk.Entry(box, font=SMALL, relief="sunken", bd=2, bg="white", justify="center")
    hexbox.place(x=PX - 1, y=PY + PH + 3, width=PW + 2, height=20)
    boxes = {}
    measure = font_of(SMALL, cv).measure
    # each column's names start on one line, as far left as the longest one needs to
    # end just before its boxes
    for column, box_x in (((("hue", "Hue:", 1), ("sat", "Sat:", 0), ("lum", "Lum:", 0)), 305),
                          ((("red", "Red:", 0), ("green", "Green:", 0), ("blue", "Blue:", 2)), 386)):
        left = box_x - 5 - max(measure(text) for _, text, _ in column)
        for r, (name, text, under) in enumerate(column):
            label(text, left, 205 + r * 24, under, anchor="nw")
            boxes[name] = tk.Entry(box, font=SMALL, relief="sunken", bd=2, bg="white", width=3)
            boxes[name].place(x=box_x, y=203 + r * 24, width=28, height=20)

    def current():
        rgb = colorsys.hls_to_rgb(state["h"], state["l"], state["s"])
        return "#" + "".join(f"{round(v * 255):02X}" for v in rgb)

    def refresh():
        col = current()
        cx, cy = FX + state["h"] * (FW - 1), FY + (1 - state["s"]) * (FH - 1)
        for item, (x0, y0, x1, y1) in zip(cross, ((-8, 0, -3, 0), (3, 0, 8, 0),
                                                 (0, -8, 0, -3), (0, 3, 0, 8))):
            cv.coords(item, cx + x0, cy + y0, cx + x1, cy + y1)
        strip = Image.new("RGB", (1, FH))
        strip.putdata([tuple(round(v * 255) for v in colorsys.hls_to_rgb(
            state["h"], 1 - y / (FH - 1), state["s"])) for y in range(FH)])
        bar_img = ImageTk.PhotoImage(strip.resize((BW, FH)))
        keep.append(bar_img)
        cv.itemconfig(bar_item, image=bar_img)
        ay = FY + (1 - state["l"]) * (FH - 1)
        cv.coords(arrow, BX + BW + 3, ay, BX + BW + 9, ay - 6, BX + BW + 9, ay + 6)
        cv.itemconfig(preview_item, image=solid(col, PW, PH))
        values = {"hue": round(state["h"] * 240) % 240, "sat": round(state["s"] * 240),
                  "lum": round(state["l"] * 240)}
        values.update(zip(("red", "green", "blue"), (int(col[i:i + 2], 16) for i in (1, 3, 5))))
        for name, val in values.items():
            boxes[name].delete(0, "end")
            boxes[name].insert(0, str(val))
        hexbox.delete(0, "end")
        hexbox.insert(0, col)
        # the chosen swatch: a black frame and a dotted one round it, like Windows'
        pick = state["pick"]
        spot = next(((x, y) for k, i, x, y in cells if (k, i) == pick), None)
        if spot:
            x, y = spot
            cv.coords(mark[0], x - 3, y - 3, x + 18, y + 18)
            cv.coords(mark[1], x - 5, y - 5, x + 20, y + 20)
        else:
            for m in mark:
                cv.coords(m, 0, 0, 0, 0)

    def set_rgb(col, pick=None):
        state["h"], state["l"], state["s"] = hls(col)
        state["pick"] = pick
        refresh()

    def click(e):
        for kind, i, x, y in cells:
            if x - 2 <= e.x <= x + 17 and y - 2 <= e.y <= y + 17:
                if kind == "custom":
                    state["slot"] = i
                    if customs[i] is None:  # an empty slot: just choose it for "Add"
                        state["pick"] = ("custom", i)
                        refresh()
                        return
                    set_rgb(customs[i], ("custom", i))
                else:
                    set_rgb(BASIC_COLORS[i], ("basic", i))
                return
        if FX <= e.x < FX + FW and FY <= e.y < FY + FH:
            drag_field(e)
        elif BX <= e.x < BX + BW + 12 and FY - 3 <= e.y < FY + FH + 3:
            drag_bar(e)

    def drag_field(e):
        state["h"] = min(max((e.x - FX) / (FW - 1), 0), 1)
        state["s"] = min(max(1 - (e.y - FY) / (FH - 1), 0), 1)
        if state["l"] in (0.0, 1.0):  # black / white has no colour to show: middle brightness
            state["l"] = 0.5
        state["pick"] = None
        refresh()

    def drag_bar(e):
        state["l"] = min(max(1 - (e.y - FY) / (FH - 1), 0), 1)
        state["pick"] = None
        refresh()

    def motion(e):
        if state.get("drag") == "field":
            drag_field(e)
        elif state.get("drag") == "bar":
            drag_bar(e)

    def press(e):
        in_field = FX <= e.x < FX + FW and FY <= e.y < FY + FH
        in_bar = BX <= e.x < BX + BW + 12 and FY - 3 <= e.y < FY + FH + 3
        state["drag"] = "field" if in_field else "bar" if in_bar else None
        click(e)
    cv.bind("<ButtonPress-1>", press)
    cv.bind("<B1-Motion>", motion)

    def typed(e=None):  # Enter / leaving a box: take what was typed, if it makes sense
        try:
            if e is not None and e.widget in (boxes["hue"], boxes["sat"], boxes["lum"]):
                state["h"] = min(max(int(boxes["hue"].get()), 0), 239) / 240
                state["s"] = min(max(int(boxes["sat"].get()), 0), 240) / 240
                state["l"] = min(max(int(boxes["lum"].get()), 0), 240) / 240
                state["pick"] = None
                refresh()
                return
            rgb = [min(max(int(boxes[n].get()), 0), 255) for n in ("red", "green", "blue")]
            set_rgb("#" + "".join(f"{v:02X}" for v in rgb))
        except ValueError:
            refresh()  # nonsense typed: show the colour as it was
    for entry in boxes.values():
        entry.bind("<Return>", typed)
        entry.bind("<FocusOut>", typed)

    def typed_hex(e=None):  # "#FF8000", "ff8000", "F80" ... - else the colour as it was
        text = hexbox.get().strip().lstrip("#")
        if len(text) == 3:
            text = "".join(ch * 2 for ch in text)
        if re.fullmatch(r"[0-9A-Fa-f]{6}", text):
            set_rgb("#" + text.upper())
        else:
            refresh()
        return "break"  # (Enter here doesn't also press OK)
    hexbox.bind("<Return>", typed_hex)
    hexbox.bind("<FocusOut>", typed_hex)

    def add_custom():
        i = state["slot"]
        customs[i] = current()
        save_settings(custom_colors=customs)
        x, y = next((x, y) for k, j, x, y in cells if (k, j) == ("custom", i))
        draw_swatch("custom", i, x, y)
        cv.tag_raise(mark[0]), cv.tag_raise(mark[1])
        state["pick"] = ("custom", i)
        state["slot"] = (i + 1) % 16
        refresh()

    def ok(_=None):
        result["color"] = current()
        win.destroy()

    def button(text, cmd, x, y, w, under=-1, state_="normal"):
        b = xp_button(box, text, cmd)
        b.config(underline=under, state=state_, padx=0, pady=0, font=SMALL)
        b.place(x=x, y=y, width=w, height=23)
        return b
    def pick_from_screen():
        """The eyedropper: point anywhere on the screen (the PDF, another window...) - the
        colour under the mouse shows in the box as it moves; a click takes it, Esc or a
        right-click puts back the one there was. A see-through layer over every screen
        catches the click, so it can't go to whatever is under it."""
        import ctypes
        import ctypes.wintypes
        was = current()
        u = ctypes.windll.user32
        x, y = u.GetSystemMetrics(76), u.GetSystemMetrics(77)  # the whole desktop, every screen
        w, h = u.GetSystemMetrics(78), u.GetSystemMetrics(79)
        layer = tk.Toplevel(win)
        layer.overrideredirect(True)
        layer.attributes("-alpha", 0.01, "-topmost", True)
        layer.geometry(f"{w}x{h}+{x}+{y}")
        layer.config(cursor="crosshair")
        tag = tk.Toplevel(win)  # beside the pointer: the colour and its number
        tag.overrideredirect(True)
        tag.attributes("-topmost", True)
        chip = tk.Label(tag, width=2, relief="solid", bd=1)
        chip.pack(side="left")
        code = tk.Label(tag, bg="#FFFFE1", fg="black", font=SMALL, relief="solid", bd=1, padx=3)
        code.pack(side="left")

        # the screen as it is now: the colours are read from this picture of it (the
        # see-through layer would tint them a little), in real pixels, every screen
        from PIL import ImageGrab
        shot = ImageGrab.grab(all_screens=True)
        u.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
        u.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]

        def real(f):  # (asked as a DPI-aware program: real pixels on a scaled screen)
            old = u.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
            try:
                return f()
            finally:
                u.SetThreadDpiAwarenessContext(ctypes.c_void_p(old))
        origin = real(lambda: (u.GetSystemMetrics(76), u.GetSystemMetrics(77)))

        def under_mouse():
            """The colour of the screen's pixel under the mouse."""
            pt = ctypes.wintypes.POINT()
            real(lambda: u.GetCursorPos(ctypes.byref(pt)))
            px, py = pt.x - origin[0], pt.y - origin[1]
            if not (0 <= px < shot.width and 0 <= py < shot.height):
                return None
            return "#%02X%02X%02X" % shot.getpixel((px, py))[:3]

        def follow(e=None):
            col = under_mouse()
            if col:
                set_rgb(col)
                chip.config(bg=untranslated(col, "box"))
                code.config(text=col)
            px, py = win.winfo_pointerxy()
            tag.geometry(f"+{px + 16}+{py + 18}")

        def done(take):
            col = under_mouse() if take else was
            layer.destroy()
            tag.destroy()
            win.grab_set()  # (back to the colour box)
            win.focus_force()
            set_rgb(col or was)
        layer.bind("<Motion>", follow)
        layer.bind("<ButtonRelease-1>", lambda e: done(True))
        layer.bind("<Button-3>", lambda e: done(False))
        layer.bind("<Escape>", lambda e: done(False))
        layer.update_idletasks()
        layer.grab_set()
        layer.focus_force()
        follow()
    # the eyedropper: a small square button with its picture, before Add to Custom Colors
    keep.append(ImageTk.PhotoImage(dropper_icon(), master=win))
    picker = ClassicButton(box, image=keep[-1], command=pick_from_screen)
    picker.place(x=211, y=280, width=23, height=23)
    Tooltip(picker.button, "Pick a colour from the screen")
    if sys.platform != "win32":
        picker.config(state="disabled")  # (it asks Windows for the screen's pixels)
    button("OK", ok, 5, 281, 66)
    button("Cancel", win.destroy, 77, 281, 66)
    button("Add to Custom Colors", add_custom, 238, 280, 188, 0)
    win.bind("<Return>", ok)
    win.bind("<Escape>", lambda e: win.destroy())

    # ---- "What's This?" (the ? button): a note for each part of the box
    notes = {
        "basic": "Click a colour to choose it.",
        "custom": "Colours you've saved. Click one to choose it. To save the chosen colour, "
                  "click an empty box, then click Add to Custom Colors.",
        "field": "Click or drag in the colours to choose one: the shade changes from left to "
                 "right, and the colour gets greyer towards the bottom.",
        "bar": "Click or drag to make the colour lighter (up) or darker (down).",
        "preview": "Shows the colour you've chosen.",
        "hex": "The colour's number, as used on web pages (#RRGGBB). Type or paste one in and "
               "press Enter.",
        "hue": "The colour's shade, from 0 (red) round through the rainbow to 239. "
               "Type a number and press Enter.",
        "sat": "How strong the colour is, from 0 (grey) to 240 (full colour). "
               "Type a number and press Enter.",
        "lum": "How light the colour is, from 0 (black) to 240 (white). "
               "Type a number and press Enter.",
        "red": "How much red is in the colour, from 0 to 255. Type a number and press Enter.",
        "green": "How much green is in the colour, from 0 to 255. Type a number and press Enter.",
        "blue": "How much blue is in the colour, from 0 to 255. Type a number and press Enter.",
        "picker": "The eyedropper: takes a colour from anywhere on the screen - the PDF, a picture, "
                            "another window: point at it (the box shows the colour under the "
                            "mouse) and click. Esc or a right-click cancels.",
        "OK": "Uses the chosen colour and closes this box.",
        "Cancel": "Closes this box without changing anything.",
        "Add to Custom Colors": "Saves the chosen colour in the selected Custom colors box, "
                                "so you can pick it again later.",
    }
    by_label = {"Basic colors:": "basic", "Custom colors:": "custom",
                "Hue:": "hue", "Sat:": "sat", "Lum:": "lum", "Red:": "red", "Green:": "green",
                "Blue:": "blue"}

    def whats_this(widget, xr, yr):
        if widget is picker:
            return notes["picker"]
        if widget is hexbox:
            return notes["hex"]
        for name, entry in boxes.items():
            if widget is entry:
                return notes[name]
        if widget is cv:  # the drawn parts: which area was clicked?
            x, y = xr - cv.winfo_rootx(), yr - cv.winfo_rooty()
            for kind, i, sx, sy in cells:
                if sx - 3 <= x <= sx + 18 and sy - 3 <= y <= sy + 18:
                    return notes[kind]
            if FX - 1 <= x <= FX + FW and FY - 1 <= y <= FY + FH:
                return notes["field"]
            if BX - 1 <= x <= BX + BW + 12 and FY - 4 <= y <= FY + FH + 4:
                return notes["bar"]
            if PX - 1 <= x <= PX + PW and PY - 1 <= y <= PY + PH:
                return notes["preview"]
            return None
        try:
            text = widget.cget("text")
        except tk.TclError:
            return None
        return notes.get(by_label.get(text, text))
    WhatsThis(win, chrome, whats_this)
    state["pick"] = next((("basic", i) for i, c in enumerate(BASIC_COLORS)
                          if c == initial.upper()), None)
    refresh()

    win.update_idletasks()
    w, h = win.winfo_reqwidth(), win.winfo_reqheight()
    if beside:  # its left edge two thirds across the main window, near the palette setting
        x = owner.winfo_rootx() + owner.winfo_width() * 64 // 100
        y = owner.winfo_rooty() + owner.winfo_height() * 29 // 100
        x = min(x, win.winfo_screenwidth() - w - 4)  # ... but never off the screen
        y = min(y, win.winfo_screenheight() - h - 40)
    else:  # centred over the main window
        x = owner.winfo_rootx() + (owner.winfo_width() - w) // 2
        y = owner.winfo_rooty() + (owner.winfo_height() - h) // 3
    win.geometry(f"+{max(0, x)}+{max(0, y)}")
    win.focus_force()
    win.grab_set()
    win.wait_window()
    return result["color"]


def reveal_all(paths):
    """Open the folder(s) holding `paths` with those files highlighted."""
    by_folder = {}
    for p in paths:
        if os.path.exists(p):  # skip files moved/deleted since
            by_folder.setdefault(os.path.dirname(p), []).append(p)
    if not by_folder:
        dialog("Master PDF", "The saved files aren't there anymore (moved or deleted).",
               sound="error")
        return
    for folder, files in list(by_folder.items())[:5]:  # don't flood the screen with windows
        reveal(folder, files)


def reveal(folder, files):
    """Open `folder` in the file manager; on Windows the given files come up selected."""
    if sys.platform == "win32":
        try:
            import ctypes
            from ctypes import wintypes
            shell32 = ctypes.windll.shell32
            shell32.ILCreateFromPathW.restype = ctypes.c_void_p
            shell32.ILCreateFromPathW.argtypes = [wintypes.LPCWSTR]
            shell32.ILFree.argtypes = [ctypes.c_void_p]
            shell32.SHOpenFolderAndSelectItems.argtypes = [
                ctypes.c_void_p, wintypes.UINT, ctypes.POINTER(ctypes.c_void_p), wintypes.DWORD]
            ctypes.windll.ole32.CoInitialize(None)
            parent = shell32.ILCreateFromPathW(os.path.normpath(folder))
            items = [i for i in (shell32.ILCreateFromPathW(os.path.normpath(f)) for f in files) if i]
            try:
                if parent and items:
                    arr = (ctypes.c_void_p * len(items))(*items)
                    if shell32.SHOpenFolderAndSelectItems(parent, len(items), arr, 0) == 0:
                        return
            finally:
                for pidl in items + [parent]:
                    if pidl:
                        shell32.ILFree(pidl)
        except Exception:
            pass
        os.startfile(folder)  # fallback: just open the folder
    else:
        subprocess.Popen(["open" if sys.platform == "darwin" else "xdg-open", folder])


def fit_text(font, text, max_px):
    """Shorten text with '...' so it fits in max_px."""
    if font.measure(text) <= max_px:
        return text
    while len(text) > 1 and font.measure(text + "...") > max_px:
        text = text[:-1]
    return text + "..."


def fmt_size(n):
    n = float(n)
    for unit in ("bytes", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{int(n)} bytes" if unit == "bytes" else f"{n:.2f} {unit}"
        n /= 1024


class TrackBar(tk.Canvas):
    """Windows 95 / 98's slider ("trackbar"): a thin sunken groove, a raised thumb with a
    point at the bottom, and tick marks under it - with the value shown above the thumb.
    Works like tk.Scale where the app uses it: variable=, from_=, to=, length=, and
    config(state="normal" / "disabled"). It only stops at multiples of step (0, 10, 20 ...
    100), with a tick mark at each: drag the thumb (it jumps stop to stop), click the groove,
    or use the arrow keys / Page Up / Down (one stop), Home / End.
    Everything is drawn in the app's written colours, so the themes recolour it."""

    def __init__(self, parent, variable, from_=0, to=100, length=200, step=10, **_):
        self.var, self.lo, self.hi, self.res = variable, from_, to, step
        self.enabled, self.drag = True, None
        self.L = length
        super().__init__(parent, width=length, height=40, bg=BG, highlightthickness=0,
                         takefocus=1)
        self.X0, self.X1 = 6, length - 7  # where the thumb's point can go
        self.TY = 14  # the thumb's top (the value is written above it)
        self.bind("<ButtonPress-1>", self.press)
        self.bind("<B1-Motion>", self.motion)
        self.bind("<ButtonRelease-1>", lambda e: setattr(self, "drag", None))
        for key, n in (("<Left>", -1), ("<Right>", 1), ("<Down>", -1), ("<Up>", 1),
                       ("<Prior>", 1), ("<Next>", -1)):
            self.bind(key, lambda e, n=n: self.step(n * self.res))
        self.bind("<Home>", lambda e: self.set(self.lo))
        self.bind("<End>", lambda e: self.set(self.hi))
        variable.trace_add("write", lambda *_: self.draw())
        self.draw()

    # tk.Scale's calls the app makes
    def configure(self, cnf=None, **kw):
        kw = {**(cnf or {}), **kw}
        state = kw.pop("state", None)
        kw.pop("fg", None)  # (the value's colour follows state)
        if state is not None:
            self.enabled = state != "disabled"
            self.draw()
        if kw:
            super().configure(**kw)
    config = configure

    def get(self):
        return self.var.get()

    def set(self, value):
        if self.enabled:  # to the nearest stop
            new = min(max(round(value / self.res) * self.res, self.lo), self.hi)
            if new != self.get():
                self.var.set(new)

    def step(self, n):
        self.set(self.get() + n)
        return "break"

    def x_of(self, value):
        return self.X0 + (value - self.lo) * (self.X1 - self.X0) / (self.hi - self.lo)

    def value_at(self, x):
        return self.lo + (x - self.X0) * (self.hi - self.lo) / (self.X1 - self.X0)

    def press(self, e):
        if not self.enabled:
            return
        self.focus_set()
        cx = self.x_of(self.get())
        if abs(e.x - cx) <= 6 and self.TY - 2 <= e.y <= self.TY + 22:  # on the thumb: drag it
            self.drag = e.x - cx
        else:  # on the groove: one stop towards the click, like Windows
            self.step(self.res if e.x > cx else -self.res)

    def motion(self, e):  # dragging: the thumb jumps from stop to stop
        if self.drag is not None and self.enabled:
            self.set(self.value_at(e.x - self.drag))

    def draw(self):
        self.delete("all")
        ty = self.TY
        cx = round(self.x_of(self.get()))
        ink = "#000000" if self.enabled else "#999999"
        # the value, above the thumb - kept fully inside at the ends (100 was cut in half)
        text = str(self.get())
        half = tkfont.Font(font=FONT).measure(text) / 2
        tx = min(max(cx, half + 1), self.L - half - 1)
        if self.enabled:
            self.create_text(tx, 1, text=text, anchor="n", font=FONT, fill=ink)
        else:  # greyed out: engraved, like Windows 98's greyed-out text
            draw_engraved(self, round(tx), 1, text, anchor="n")
        # the groove: sunken, 4 px tall, through the thumb's middle
        x0, x1, gy = self.X0 - 2, self.X1 + 2, ty + 6
        self.create_line(x0, gy + 3, x0, gy, x1, gy, fill=EDGE_SHADOW)  # grey top / left
        self.create_line(x0 + 1, gy + 2, x0 + 1, gy + 1, x1 - 1, gy + 1, fill=EDGE_DARK)
        self.create_line(x0 + 1, gy + 2, x1, gy + 2, fill=BG)
        self.create_line(x0, gy + 3, x1 + 1, gy + 3, fill=EDGE_LIGHT)  # white bottom / right
        self.create_line(x1, gy, x1, gy + 4, fill=EDGE_LIGHT)
        # a tick mark at each stop (1 px boxes, so dark mode lightens them like text)
        for v in range(self.lo, self.hi + 1, self.res):
            tx = round(self.x_of(v))
            self.create_rectangle(tx, ty + 23, tx + 1, ty + 26, fill=ink, outline="")
        # the thumb: 11 px wide, a 16 px body and a 5 px point, raised like a button
        l, r, b = cx - 5, cx + 5, ty + 15
        self.create_polygon(l, ty, r, ty, r, b, cx, b + 5, l, b, fill=BG, outline="")
        self.create_line(l, b, l, ty, r, ty, fill=EDGE_LIGHT)  # white left / top ...
        self.create_line(l, b, cx, b + 5, fill=EDGE_LIGHT)  # ... and left slope
        self.create_line(r - 1, ty + 1, r - 1, b, cx, b + 4, fill=EDGE_SHADOW)  # grey inner
        self.create_line(r, ty, r, b, cx, b + 5, fill=EDGE_DARK)  # black right and slope
        if not self.enabled:  # greyed out: a dotted face, like a disabled Windows thumb
            for yy in range(ty + 2, b, 2):
                for xx in range(l + 2 + (yy // 2) % 2, r - 1, 2):
                    self.create_rectangle(xx, yy, xx + 1, yy + 1, fill=EDGE_LIGHT, outline="")


class SlimTrackBar(TrackBar):
    """The same Windows 98 slider, slim enough for a toolbar: no number above it (the
    toolbar shows it beside it), a smaller thumb, and tick marks only when there are few
    stops."""
    H = 22

    def __init__(self, parent, variable, from_=0, to=100, length=110, step=10):
        super().__init__(parent, variable, from_=from_, to=to, length=length, step=step)
        tk.Canvas.configure(self, height=self.H)
        self.TY = 3
        self.draw()

    def set_range(self, lo, hi, step):
        self.lo, self.hi, self.res = lo, hi, step
        self.draw()

    def draw(self):
        self.delete("all")
        ty = self.TY
        cx = round(self.x_of(self.get()))
        ink = "#000000" if self.enabled else "#999999"
        x0, x1, gy = self.X0 - 2, self.X1 + 2, ty + 5  # the groove
        self.create_line(x0, gy + 3, x0, gy, x1, gy, fill=EDGE_SHADOW)
        self.create_line(x0 + 1, gy + 2, x0 + 1, gy + 1, x1 - 1, gy + 1, fill=EDGE_DARK)
        self.create_line(x0 + 1, gy + 2, x1, gy + 2, fill=BG)
        self.create_line(x0, gy + 3, x1 + 1, gy + 3, fill=EDGE_LIGHT)
        self.create_line(x1, gy, x1, gy + 4, fill=EDGE_LIGHT)
        stops = (self.hi - self.lo) // self.res
        for v in ([self.lo + k * self.res for k in range(stops + 1)] if stops <= 20
                  else [self.lo, self.hi]):
            tx = round(self.x_of(v))
            self.create_rectangle(tx, ty + 16, tx + 1, ty + 18, fill=ink, outline="")
        l, r, b = cx - 4, cx + 4, ty + 10  # the thumb: 9 px wide, a point at the bottom
        self.create_polygon(l, ty, r, ty, r, b, cx, b + 4, l, b, fill=BG, outline="")
        self.create_line(l, b, l, ty, r, ty, fill=EDGE_LIGHT)
        self.create_line(l, b, cx, b + 4, fill=EDGE_LIGHT)
        self.create_line(r - 1, ty + 1, r - 1, b, cx, b + 3, fill=EDGE_SHADOW)
        self.create_line(r, ty, r, b, cx, b + 4, fill=EDGE_DARK)
        if not self.enabled:
            for yy in range(ty + 2, b, 2):
                for xx in range(l + 2 + (yy // 2) % 2, r - 1, 2):
                    self.create_rectangle(xx, yy, xx + 1, yy + 1, fill=EDGE_LIGHT, outline="")


# the raised 3D edge Tk gives buttons on this background (sampled from the Add button)
EDGE_LIGHT, EDGE_SHADOW, EDGE_DARK = "#FFFFFF", "#8E8C82", "#000000"


# ---- PDFs: read, drawn and written by PyMuPDF ----
# MuPDF must only be used by one thread at a time (two at once can crash Python), and the
# tabs each have threads of their own: every use of it goes through this lock.
PDF_LOCK = threading.RLock()
PDF_EXTS = (".pdf",)
IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".jpe", ".jfif", ".bmp", ".gif", ".tif", ".tiff",
              ".webp", ".ico", ".tga", ".ppm", ".pcx")  # pictures Pillow can read
# names Windows won't let you use for a file, whatever the extension
RESERVED = {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)),
            *(f"LPT{i}" for i in range(1, 10))}
# the page sizes a picture can be put on: (width, height) in points, portrait
PAGE_SIZES = {"Fit the picture": None, "A4": (595.28, 841.89), "Letter": (612.0, 792.0),
              "A5": (419.53, 595.28), "Legal": (612.0, 1008.0)}
SAVE_OPTS = dict(garbage=3, deflate=True)  # drop unused parts, compress what's left


class Cancelled(Exception):
    """Cancel was pressed while a job was running."""


def page_find(page, query):
    """(PDF_LOCK held) Every place query is on the page: [(rect, (before, the words, after))]
    - found letter by letter, the way they read: case and accents as typed don't matter,
    Arabic's joined letter shapes (as PDFs keep them) and ligatures ("fi") count as the
    letters they are. Each line's text comes with it, to show round the words."""
    def norm(s):
        return unicodedata.normalize("NFKC", s).casefold()
    key = re.sub(r"\s+", " ", norm(query)).strip()
    if not key:
        return []
    out = []
    for block in page.get_text("rawdict")["blocks"]:
        if block.get("type") != 0:
            continue
        for line in block["lines"]:
            chars = [ch for s in line["spans"] for ch in s["chars"]]
            flat, where = [], []  # (the line read out normalized, and which letter each came from)
            for n, ch in enumerate(chars):
                piece = norm(ch["c"])
                if piece.isspace() or not piece:
                    piece = " "
                    if flat and flat[-1] == " ":
                        continue
                for x in piece:
                    flat.append(x)
                    where.append(n)
            s = "".join(flat)
            shown = unicodedata.normalize("NFKC", "".join(ch["c"] for ch in chars))
            start = s.find(key)
            while start >= 0:
                a, b = where[start], where[start + len(key) - 1]
                rect = pymupdf.Rect(chars[a]["bbox"])
                for ch in chars[a:b + 1]:
                    rect |= ch["bbox"]
                before = s[:start]  # (normalized: as shown, letters as they read)
                before, words, after = (shown[:len(before)] if len(shown) == len(s) else before,
                                        s[start:start + len(key)], s[start + len(key):])
                if len(shown) == len(s):
                    words, after = shown[start:start + len(key)], shown[start + len(key):]
                if len(before) > 40:
                    before = "..." + before[-37:].lstrip()
                if len(after) > 70:
                    after = after[:67].rstrip() + "..."
                out.append((rect, (before, words, after)))
                start = s.find(key, start + max(1, len(key)))
    return out


def is_pdf(path):
    return path.lower().endswith(PDF_EXTS)


# Word documents (and the like): made into PDFs by Microsoft Word itself - or LibreOffice
WORD_EXTS = (".docx", ".doc", ".docm", ".dotx", ".dotm", ".dot", ".rtf", ".odt")
WORD_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
$word = New-Object -ComObject Word.Application
try {
    $word.Visible = $false
    $word.DisplayAlerts = 0
    # (read-only, no conversion questions; a wrong password given, so a locked document
    # fails at once instead of asking for one where no one can see it)
    $doc = $word.Documents.Open($env:MP_SRC, $false, $true, $false, '~no password~')
    $doc.ExportAsFixedFormat($env:MP_DST, 17)
    $doc.Close(0)
} finally {
    $word.Quit()
    [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($word)
}
"""


def is_word(path):
    return path.lower().endswith(WORD_EXTS)


def find_libreoffice():
    for base in (os.environ.get("ProgramFiles", r"C:\Program Files"),
                 os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        exe = os.path.join(base, "LibreOffice", "program", "soffice.exe")
        if os.path.isfile(exe):
            return exe
    return None


def has_word():
    """Whether Microsoft Word is installed (it does the converting)."""
    try:
        import winreg
        winreg.CloseKey(winreg.OpenKey(winreg.HKEY_CLASSES_ROOT, r"Word.Application\CLSID"))
        return True
    except OSError:
        return False


KILL_HIDDEN_WORD = (  # (the Word started for converting - never one someone has open)
    "Get-CimInstance Win32_Process -Filter \"Name='WINWORD.EXE'\" | "
    "Where-Object { $_.CommandLine -match 'Embedding|Automation' } | "
    "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }")


def run_cancellable(args, job, **kw):
    """subprocess.run, but stopped if job["cancel"] is set meanwhile (raises Cancelled)."""
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)
    started = time.time()
    while True:
        try:
            out, err = proc.communicate(timeout=0.2)
            return subprocess.CompletedProcess(args, proc.returncode, out, err)
        except subprocess.TimeoutExpired:
            if job and job.get("cancel"):
                proc.kill()
                proc.communicate()
                raise Cancelled()
            if time.time() - started > 300:
                proc.kill()
                proc.communicate()
                raise subprocess.TimeoutExpired(args, 300)


def word_to_pdf(src, dst, job=None):
    """A Word document (.docx, .doc, .rtf...) made into the PDF dst - by Microsoft Word, as
    its own Save as PDF does (or by LibreOffice, without Word). Raises RuntimeError with a
    plain message if it can't be - or Cancelled, if job["cancel"] is set meanwhile (the
    hidden Word started for it is closed)."""
    hidden = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    why = None
    if has_word():
        env = dict(os.environ, MP_SRC=os.path.abspath(src), MP_DST=os.path.abspath(dst))
        try:
            r = run_cancellable(["powershell", "-NoProfile", "-NonInteractive", "-Command",
                                 WORD_SCRIPT], job, env=env, text=True, creationflags=hidden)
            if r.returncode == 0 and os.path.isfile(dst):
                return
            err = (r.stderr or r.stdout or "").strip().splitlines()
            why = next((line for line in err if line.strip() and not line.startswith("At ")),
                       "Word couldn't open it")
            if "password" in why.lower():
                why = "it's locked with a password"
        except Cancelled:
            subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command",
                            KILL_HIDDEN_WORD], capture_output=True, timeout=30, creationflags=hidden)
            raise
        except subprocess.TimeoutExpired:
            why = "Word took too long"
    office = find_libreoffice()
    if office:
        out = os.path.dirname(os.path.abspath(dst))
        run_cancellable([office, "--headless", "--convert-to", "pdf", "--outdir", out,
                         os.path.abspath(src)], job, creationflags=hidden)
        made = os.path.join(out, os.path.splitext(os.path.basename(src))[0] + ".pdf")
        if os.path.isfile(made):
            if os.path.abspath(made) != os.path.abspath(dst):
                os.replace(made, dst)
            return
        why = why or "LibreOffice couldn't convert it"
    raise RuntimeError(why or "making a PDF from a Word document needs Microsoft Word "
                              "(or LibreOffice) on this computer")


def open_pdf(path):
    """Open a PDF to read (call with PDF_LOCK held). A clear message if it can't be: locked
    with a password, or not a PDF at all."""
    try:
        doc = pymupdf.open(path)
    except Exception as e:
        raise ValueError(f"can't be read as a PDF ({e})") from None
    if doc.needs_pass:
        doc.close()
        raise ValueError("is locked with a password")
    if not doc.is_pdf:
        doc.close()
        raise ValueError("isn't a PDF")
    return doc


def render_page(page, box=None, dpi=None):
    """A page drawn into a Pillow picture (RGB, white paper): at dpi, or fitted inside box x
    box pixels. (Call with PDF_LOCK held.)"""
    zoom = dpi / 72 if dpi else box / max(page.rect.width, page.rect.height, 1)
    pix = page.get_pixmap(matrix=pymupdf.Matrix(zoom, zoom), alpha=False)
    return Image.frombytes("RGB", (pix.width, pix.height), pix.samples)


def open_picture(path):
    """A picture file the right way up (turned per EXIF), as RGB, or RGBA if it has
    see-through parts."""
    with Image.open(path) as im:
        im.load()
        im = ImageOps.exif_transpose(im)
    alpha = "A" in im.getbands() or "transparency" in im.info
    return im.convert("RGBA" if alpha else "RGB")


def picture_dpi(im):
    """The resolution a picture says it was made for - 96 if it doesn't say, or says 72 (phone
    photos do, which would make their pages over a metre wide)."""
    try:
        dpi = float(im.info.get("dpi", (96, 96))[0])
    except (TypeError, ValueError, IndexError):
        dpi = 96
    return dpi if dpi > 72 else 96


def add_picture_page(doc, path, page_size=None, quality=92):
    """Put the picture at path on a new page at the end of doc (call with PDF_LOCK held).
    page_size None: the page is the picture's own size; else (w, h) in points - the picture
    is fitted in, half an inch from the edges, on a page turned sideways for a wide picture.
    Photos go in as JPEG (at quality), anything with see-through parts as PNG."""
    im = open_picture(path)
    if page_size is None:
        dpi = picture_dpi(im)
        page = doc.new_page(width=im.width * 72 / dpi, height=im.height * 72 / dpi)
        rect = page.rect
    else:
        pw, ph = page_size
        if im.width > im.height:
            pw, ph = ph, pw
        page = doc.new_page(width=pw, height=ph)
        m = 36  # half an inch
        scale = min((pw - 2 * m) / im.width, (ph - 2 * m) / im.height)
        w, h = im.width * scale, im.height * scale
        rect = pymupdf.Rect((pw - w) / 2, (ph - h) / 2, (pw + w) / 2, (ph + h) / 2)
    buf = io.BytesIO()
    if im.mode == "RGBA" or not path.lower().endswith((".jpg", ".jpeg", ".jpe", ".jfif")):
        im.save(buf, "PNG", optimize=False)  # (drawings, screenshots: kept sharp)
    else:
        im.save(buf, "JPEG", quality=quality)
    page.insert_image(rect, stream=buf.getvalue())
    return page


def parse_ranges(text):
    """ "1-3, 5, 8-" -> [(1, 3), (5, 5), (8, None)] (None: to the last page). Raises
    ValueError with a message that says what's wrong."""
    out = []
    for part in re.split(r"[,;]", text):
        part = part.strip().replace(" ", "")
        if not part:
            continue
        m = re.fullmatch(r"(\d*)-(\d*)", part)
        if m and (m[1] or m[2]):
            a, b = int(m[1] or 1), int(m[2]) if m[2] else None
        elif part.isdigit():
            a = b = int(part)
        else:
            raise ValueError(f'"{part}" isn\'t a page or a range of pages (like 1-3).')
        if a < 1 or (b is not None and b < a):
            raise ValueError(f'"{part}" isn\'t a range of pages that can be.')
        out.append((a, b))
    if not out:
        raise ValueError("Type the pages to keep in each file, like 1-3, 5, 8-")
    return out


def page_size_text(w, h):
    """A page's size as people know it: "A4", "Letter (sideways)", or "210 x 99 mm"."""
    for name, size in PAGE_SIZES.items():
        if size is None:
            continue
        sw, sh = size
        if abs(w - sw) < 2 and abs(h - sh) < 2:
            return name
        if abs(w - sh) < 2 and abs(h - sw) < 2:
            return f"{name} (sideways)"
    return f"{w * 25.4 / 72:.0f} x {h * 25.4 / 72:.0f} mm"


def short_size(n):
    """1234567 -> '1.2 MB' (rounder than fmt_size, for the Status column)."""
    if n < 1024 * 1024:
        return f"{max(1, round(n / 1024))} KB"
    return f"{n / 1024 / 1024:.1f} MB"


class FlatScrollbar(tk.Canvas):
    """Scrollbar drawn like Windows 98's - up and down, or (orient="horizontal") left and
    right: raised 3D arrow buttons with solid black triangles (pressed: flat, the triangle
    shifted), a raised thumb, and a checkered track of the face and the light edge colour (in
    the grey themes; the others, whose schemes have a scrollbar colour of their own, a plain
    track) - the part of the track being held down turns dark, like Windows'. Drawn in the
    theme's own colours. Drop-in for tk.Scrollbar: command=widget.yview (or .xview), and the
    widget's yscrollcommand (xscrollcommand) = sb.set."""
    CHECKERED = ("98", "xp", "mono")  # the themes with Windows 98's checkered track
    W, ARROW = 17, 17
    TRACK = "#EFEFEF"

    def __init__(self, parent, command, orient="vertical"):
        self.vertical = orient == "vertical"
        size = {"width": self.W} if self.vertical else {"height": self.W}
        super().__init__(parent, bg=self.TRACK, highlightthickness=0, bd=0, **size)
        self.command = command
        self.first, self.last = 0.0, 1.0
        self.held = None  # part being pressed: "up" / "down" (left / right), "thumb", "page_..."
        self._drag, self._repeat = None, None
        self.bind("<Configure>", lambda e: self.draw())
        self.bind("<ButtonPress-1>", self.press)
        self.bind("<B1-Motion>", self.drag)
        self.bind("<ButtonRelease-1>", self.release)
        self.bind("<MouseWheel>", lambda e: e.delta and self.command(
            "scroll", -int(e.delta / 120) or (-1 if e.delta > 0 else 1), "units"))

    # positions along the bar: y if it's upright, x if it lies down
    def along(self, e):
        return e.y if self.vertical else e.x

    def length(self):
        return self.winfo_height() if self.vertical else self.winfo_width()

    def box(self, a0, a1):
        """The screen rectangle of the stretch a0..a1 along the bar."""
        return (0, a0, self.W, a1) if self.vertical else (a0, 0, a1, self.W)

    def set(self, first, last):  # called by the scrolled widget
        self.first, self.last = float(first), float(last)
        self.draw()

    def thumb_box(self):
        """Start and end of the thumb along the bar (at least 20 px so it stays grabbable)."""
        L = self.length()
        t0, t1 = self.ARROW, max(self.ARROW + 1, L - self.ARROW)
        span = t1 - t0
        size = max(20, (self.last - self.first) * span)
        a0 = t0 + (span - size) * (self.first / max(1e-9, 1 - (self.last - self.first)))
        return a0, a0 + size

    def part_at(self, a):
        if a < self.ARROW:
            return "up"
        if a >= self.length() - self.ARROW:
            return "down"
        a0, a1 = self.thumb_box()
        return "thumb" if a0 <= a <= a1 else ("page_up" if a < a0 else "page_down")

    _checkers = {}  # (size, colours) -> a long checkered picture, made once

    def checker(self, a, b):
        """A checkered picture of colours a and b, as long as a screen (drawn from the start,
        the canvas shows as much as it needs)."""
        n = self.winfo_screenheight() if self.vertical else self.winfo_screenwidth()
        w, h = (self.W, n) if self.vertical else (n, self.W)
        key = (id(self.tk), w, h, a, b)
        if key not in self._checkers:
            ca, cb = (tuple(int(c[i:i + 2], 16) for i in (1, 3, 5)) for c in (a, b))
            img = Image.new("RGB", (w, h))
            img.putdata([ca if (x + y) % 2 == 0 else cb for y in range(h) for x in range(w)])
            self._checkers[key] = ImageTk.PhotoImage(img, master=self)
        return self._checkers[key]

    def raised_box(self, x0, y0, x1, y1):
        """A raised Windows 98 box over [x0, x1) x [y0, y1): face, light inside the top /
        left, shadow inside and dark outside the bottom / right."""
        self.create_rectangle(x0, y0, x1, y1, fill=BG, outline="")
        self.create_line(x0 + 1, y1 - 2, x0 + 1, y0 + 1, x1 - 2, y0 + 1, fill=EDGE_LIGHT)
        self.create_line(x0 + 1, y1 - 2, x1 - 2, y1 - 2, x1 - 2, y0, fill=EDGE_SHADOW)
        self.create_line(x0, y1 - 1, x1 - 1, y1 - 1, x1 - 1, y0 - 1, fill=EDGE_DARK)

    def draw(self):
        self.delete("all")
        L, A = self.length(), self.ARROW
        at = (lambda a: (0, a)) if self.vertical else (lambda a: (a, 0))  # noqa: E731
        face, light = theme_color(BG), theme_color(EDGE_LIGHT, "edge")
        if THEME == "mono":
            face, light = "#FFFFFF", "#000000"  # (black and white checks)
        if THEME in self.CHECKERED:
            self.create_image(*at(A), image=self.checker(face, light), anchor="nw")
        else:
            self.create_rectangle(*self.box(A, L - A), fill=self.TRACK, outline="")
        a0, a1 = (round(v) for v in self.thumb_box())
        if self.held in ("page_up", "page_down"):  # the part held down: dark
            s, e = (A, a0) if self.held == "page_up" else (a1, L - A)
            if e > s:
                if THEME in self.CHECKERED:  # dark checks from the start of the part, then
                    # the light ones again from its end (on the same checks as the rest)
                    dark = self.checker(theme_color(EDGE_DARK, "edge"),
                                        theme_color(EDGE_SHADOW, "edge"))
                    self.create_image(*at(s - (s - A) % 2), image=dark, anchor="nw")
                    self.create_image(*at(e + (e - A) % 2), image=self.checker(face, light),
                                      anchor="nw")
                else:
                    self.create_rectangle(*self.box(s, e), outline="",
                                          fill=theme_color(EDGE_SHADOW, "edge"))
        self.raised_box(*self.box(a0, a1))  # the thumb, then the arrow buttons over the ends
        for part, s in (("up", 0), ("down", L - A)):
            down = self.held == part
            x0, y0, x1, y1 = self.box(s, s + A)
            if down:  # pressed: flat, a 1 px shadow round it
                self.create_rectangle(x0, y0, x1, y1, fill=BG, outline="")
                self.create_line(x0, y0, x1 - 1, y0, x1 - 1, y1 - 1, x0, y1 - 1, x0, y0,
                                 fill=EDGE_SHADOW)
            else:
                self.raised_box(x0, y0, x1, y1)
            # a solid black triangle, 7 px wide and 4 deep, in the middle (1 px down / right
            # while pressed, like a pushed button)
            cx, cy = (x0 + x1) // 2 + down, (y0 + y1) // 2 + down
            for row in range(4):
                if self.vertical:
                    y = cy - 2 + row if part == "up" else cy + 1 - row
                    self.create_rectangle(cx - row, y, cx + row + 1, y + 1, fill="#000000",
                                          outline="")
                else:
                    x = cx - 2 + row if part == "up" else cx + 1 - row
                    self.create_rectangle(x, cy - row, x + 1, cy + row + 1, fill="#000000",
                                          outline="")

    def press(self, e):
        part = self.part_at(self.along(e))
        self.held = part
        if part == "thumb":
            self._drag = (self.along(e), self.first)
        else:
            self.step(part, first=True)
        self.draw()

    def step(self, part, first=False):
        """One arrow / page step; keeps repeating while the button is held, like Windows."""
        if self.held != part:
            return
        if part in ("up", "down"):
            self.command("scroll", -1 if part == "up" else 1, "units")
        else:
            a0, a1 = self.thumb_box()  # stop paging once the thumb reaches the mouse
            pa = (self.winfo_pointery() - self.winfo_rooty() if self.vertical
                  else self.winfo_pointerx() - self.winfo_rootx())
            if (part == "page_up" and pa >= a0) or (part == "page_down" and pa <= a1):
                return
            self.command("scroll", -1 if part == "page_up" else 1, "pages")
        self._repeat = self.after(400 if first else 50, self.step, part)

    def drag(self, e):
        if not self._drag:
            return
        start, first0 = self._drag
        span = max(1, self.length() - 2 * self.ARROW)
        size = max(20, (self.last - self.first) * span)
        room = max(1, span - size)  # how far the thumb can travel
        frac = first0 + (self.along(e) - start) / room * (1 - (self.last - self.first))
        self.command("moveto", min(max(frac, 0), 1))

    def release(self, e):
        if self._repeat:
            self.after_cancel(self._repeat)
        self._repeat, self._drag, self.held = None, None, None
        self.draw()


# ---- toolbar pictures: drawn pixel by pixel, 16 x 16, like Windows 98's toolbar icons ----
PIXEL_COLORS = {
    "K": (0, 0, 0), "W": (255, 255, 255), "S": (192, 192, 192), "D": (128, 128, 128),
    "Y": (255, 214, 0), "H": (255, 255, 160), "O": (160, 120, 0),  # folder / note yellows
    "B": (40, 40, 140), "N": (0, 0, 128), "L": (96, 160, 255),  # blues
    "R": (200, 0, 0), "P": (255, 150, 170), "G": (0, 128, 0), "T": (210, 150, 80),
}


def pixel_icon(rows, colors=None):
    """A 16 x 16 picture from rows of letters (see PIXEL_COLORS; "." is see-through);
    colors: other colours for some letters."""
    palette = {**PIXEL_COLORS, **(colors or {})}
    im = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    for y, row in enumerate(rows[:16]):
        for x, ch in enumerate(row[:16]):
            if ch in palette:
                im.putpixel((x, y), palette[ch] + (255,))
    return im


# which parts of a tool's picture show the colour it draws in: the highlighter's tip and
# stroke, the pencil's paint, the note's paper (its lines a darker shade), the T itself
TINTED = {"highlight": {"Y": 1.0}, "pen": {"Y": 1.0}, "text": {"K": 1.0}}


def tool_icon(tool, color):
    """A tool's picture, in the colour it's set to draw in."""
    if tool in ("pen", "highlight"):  # (the pencil and the marker: as their pointers show)
        return drawing_icon("pen" if tool == "pen" else "marker", color)
    rgb = hex_rgb_255(color)
    shade = lambda k: tuple(round(v * k) for v in rgb)  # noqa: E731
    return pixel_icon(ICON_ROWS[tool], {ch: shade(k) for ch, k in TINTED[tool].items()})


def arrow_icon(kind):
    """Black triangles for the page buttons: "up" / "down"."""
    im = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    for row in range(5):
        y = 5 + row if kind == "up" else 10 - row
        for x in range(7 - row, 9 + row):
            im.putpixel((x, y), (0, 0, 0, 255))
    return im


def lens_icon(sign=None):
    """A magnifying glass, with - or + in it for zooming."""
    big = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    d.line([(38, 38), (58, 58)], fill=(90, 60, 20, 255), width=12)  # the handle
    d.ellipse([4, 4, 44, 44], fill=(200, 230, 255, 255), outline=(0, 0, 0, 255), width=6)
    if sign:
        d.rectangle([14, 22, 34, 26], fill=(0, 0, 0, 255))
        if sign == "+":
            d.rectangle([22, 14, 26, 34], fill=(0, 0, 0, 255))
    return big.resize((16, 16), Image.LANCZOS)


ICON_ROWS = {
    "open": ["................",
             "................",
             "..KKKK..........",
             ".KHHHHK.........",
             "KHHHHHHKKKKKKK..",
             "KHYYYYYYYYYYYHK.",
             "KHYYYYYYYYYYYYK.",
             "KHYYYYYYYYYYYYK.",
             "KHYYYYYYYYYYYYK.",
             "KHYYYYYYYYYYYYK.",
             "KHYYYYYYYYYYYYK.",
             "KHYYYYYYYYYYYYK.",
             "KOOOOOOOOOOOOOK.",
             "KKKKKKKKKKKKKKK."],
    "save": ["................",
             ".KKKKKKKKKKKKKK.",
             ".KBBWWWWWWWWBBK.",
             ".KBBWWWWWWWWBBK.",
             ".KBBWDDDDDDWBBK.",
             ".KBBWWWWWWWWBBK.",
             ".KBBWDDDDDDWBBK.",
             ".KBBWWWWWWWWBBK.",
             ".KBBBBBBBBBBBBK.",
             ".KBBBSSSSSSSBBK.",
             ".KBBBSKKSSSSBBK.",
             ".KBBBSKKSSSSBBK.",
             ".KBBBSKKSSSSBBK.",
             ".KBBBSSSSSSSBBK.",
             ".KKKKKKKKKKKKKK."],
    "move": ["...K............",
             "...KK...........",
             "...KWK..........",
             "...KWWK.........",
             "...KWWWK........",
             "...KWWWWK.......",
             "...KWWWWWK......",
             "...KWWWWWWK.....",
             "...KWWWWWWWK....",
             "...KWWWWWKKKK...",
             "...KWWKWWK......",
             "...KWK.KWWK.....",
             "...KK..KWWK.....",
             "...K....KWWK....",
             "........KWWK....",
             ".........KK....."],
    "edit": ["KK.KK.KK.KK.KK..",
             "K............K..",
             "................",
             "K...NNN.NNN..K..",
             "K......N.....K..",
             ".......N........",
             "K......N.....K..",
             "K......N.....K..",
             ".......N........",
             "K......N.....K..",
             "K...NNN.NNN..K..",
             "................",
             "K............K..",
             "KK.KK.KK.KK.KK.."],
    "text": ["................",
             ".KKKKKKKKKKKKK..",
             ".KKKKKKKKKKKKK..",
             ".KK....KK....KK.",
             ".K.....KK.....K.",
             ".......KK.......",
             ".......KK.......",
             ".......KK.......",
             ".......KK.......",
             ".......KK.......",
             ".......KK.......",
             ".......KK.......",
             ".......KK.......",
             ".....KKKKKK.....",
             "................"],
    "highlight": ["..........KK....",
                  ".........KDDK...",
                  "........KDDDDK..",
                  ".......KDDDDK...",
                  "......KWDDDK....",
                  ".....KWWDDK.....",
                  "....KWWWDK......",
                  "...KYYWWK.......",
                  "..KYYYYK........",
                  "..KYYYK.........",
                  "...KKK..........",
                  "................",
                  "YYYYYYYYYYYYY...",
                  "YYYYYYYYYYYYY...",
                  "YYYYYYYYYYYYY..."],
    "pen": ["............KK..",
            "...........KPPK.",
            "..........KSPPK.",
            ".........KYSSK..",
            "........KYYYK...",
            ".......KYYYK....",
            "......KYYYK.....",
            ".....KYYYK......",
            "....KYYYK.......",
            "...KYYYK........",
            "..KTYYK.........",
            "..KTTK..........",
            "..KKK...........",
            "..K.............",
            "................",
            "................"],
}


# Undo / Redo: Windows 98's Back / Forward arrows - a block arrow (a triangle head on a thick
# shaft), lit from the top left: light along its top and left edges, dark along its bottom
# and right ones. Blue while it can be clicked; greyed out (engraved) when there's nothing
# to undo / redo, like Windows 98's
BLOCK_ARROW = {"W": (120, 120, 210), "M": PIXEL_COLORS["B"], "A": (16, 16, 80)}  # (the save
# button's blue, with a lighter and a darker shade of it)
BACK_ROWS = ["................",
             "................",
             "......W.........",
             ".....WM.........",
             "....WMM.........",
             "...WMMMWWWWWWW..",
             "..WMMMMMMMMMMA..",
             ".WMMMMMMMMMMMA..",
             "..AMMMMMMMMMMA..",
             "...AMMMAAAAAAA..",
             "....AMM.........",
             ".....AM.........",
             "......A........."]
FORWARD_ROWS = ["................",
                "................",
                ".........W......",
                ".........MW.....",
                ".........MMW....",
                "..WWWWWWWMMMW...",
                "..WMMMMMMMMMMW..",
                "..WMMMMMMMMMMMW.",
                "..WMMMMMMMMMMA..",
                "..AAAAAAAMMMA...",
                ".........MMA....",
                ".........MA.....",
                ".........A......"]


ICON_ROWS.update({
    "eraser": ["................",
               ".........KKK....",
               "........KPPPK...",
               ".......KPPPPPK..",
               "......KPPPPPPK..",
               ".....KWPPPPPK...",
               "....KWWKPPPK....",
               "...KWWWWKPK.....",
               "..KWWWWWWK......",
               "..KWWWWWK.......",
               "...KWWWK........",
               "....KKK.........",
               "................",
               "..DDDDDDDDDDD..."],
    "image": ["................",
              "................",
              ".KKKKKKKKKKKKKK.",
              ".KLLLLLLLLLLLLK.",
              ".KLLLLLLLLYYLLK.",
              ".KLLLLLLLLYYLLK.",
              ".KLLLLLLLLLLLLK.",
              ".KLLLLGLLLLLLLK.",
              ".KLLLGGGLLLGLLK.",
              ".KLLGGGGGLGGGLK.",
              ".KLGGGGGGGGGGGK.",
              ".KGGGGGGGGGGGGK.",
              ".KKKKKKKKKKKKKK."],
    "annots": ["................",
               "................",
               ".KKKKKKKKKKKKK..",
               ".KWWWWWWWWWWWK..",
               ".KWDDDDDDDDDWK..",
               ".KWWWWWWWWWWWK..",
               ".KWDDDDDDDWWWK..",
               ".KWWWWWWWWWWWK..",
               ".KWDDDDDDDDDWK..",
               ".KWWWWWWWWWWWK..",
               ".KKKKKWWKKKKKK..",
               ".....KWK........",
               "......KK........"],
})


def drawn_icon(draw, color="#000000", width=2):
    """A picture drawn with lines at 4 x the size, then made small (smooth, like the lens)."""
    big = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    draw(ImageDraw.Draw(big), hex_rgb_255(color) + (255,), width * 4)
    return big.resize((16, 16), Image.LANCZOS)


def shape_icon(kind, color):
    """The Shape tool's picture: its rectangle, ellipse, line or arrow, in its colour."""
    if kind == "rect":
        im = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
        ImageDraw.Draw(im).rectangle([1, 3, 14, 12], outline=hex_rgb_255(color) + (255,), width=2)
        return im

    def draw(d, c, w):
        if kind == "ellipse":
            d.ellipse([4, 10, 59, 53], outline=c, width=w)
        elif kind == "line":
            d.line([(8, 56), (56, 8)], fill=c, width=w)
        else:  # an arrow: a line with a head at its top right
            d.line([(8, 56), (48, 16)], fill=c, width=w)
            d.polygon([(58, 6), (30, 14), (50, 34)], fill=c)
    return drawn_icon(draw, color)


def sign_icon():
    """A signature's squiggle over the line it's signed on."""
    def draw(d, c, w):
        d.line([(4, 40), (12, 22), (18, 40), (26, 18), (32, 42), (40, 24), (48, 36), (60, 26)],
               fill=(0, 0, 128, 255), width=w, joint="curve")
        d.line([(2, 54), (62, 54)], fill=(0, 0, 0, 255), width=4)
    return drawn_icon(draw)


def link_icon():
    """Two links of a chain."""
    def draw(d, c, w):
        d.rounded_rectangle([4, 24, 38, 44], radius=10, outline=(0, 0, 0, 255), width=w)
        d.rounded_rectangle([26, 20, 60, 40], radius=10, outline=(0, 0, 128, 255), width=w)
    return drawn_icon(draw)


T_ROWS = ["................",
          "..KKKKKKKKKKKK..",
          "..KKKKKKKKKKKK..",
          "..K....KK....K..",
          ".......KK.......",
          ".......KK.......",
          ".......KK.......",
          ".......KK.......",
          ".......KK.......",
          ".......KK.......",
          ".......KK.......",
          "......KKKK......",
          "................",
          "................",
          "................",
          "................"]


def marked_rows(kind):
    """The text toolbar's marks: a T highlighted, struck out, squiggly-underlined - or a U
    underlined (rows for pixel_icon)."""
    rows = [list(r) for r in T_ROWS]
    if kind == "highlight":
        for y in range(0, 13):
            for x in range(1, 15):
                if rows[y][x] == ".":
                    rows[y][x] = "Y"
    elif kind == "strike":
        for y in (6, 7):
            for x in range(1, 15):
                rows[y][x] = "R"
    elif kind == "squiggly":
        for y, row in ((12, "..R...R...R...R."), (13, ".R.R.R.R.R.R.R.R"), (14, "R...R...R...R...")):
            rows[y] = list(row)
    elif kind == "underline":
        rows = [list(r) for r in ["................",
                                  "..NN.......NN...",
                                  "..NN.......NN...",
                                  "..NN.......NN...",
                                  "..NN.......NN...",
                                  "..NN.......NN...",
                                  "..NN.......NN...",
                                  "..NN.......NN...",
                                  "..NN.......NN...",
                                  "...NN.....NN....",
                                  "....NNNNNNN.....",
                                  "................",
                                  "................",
                                  "..NNNNNNNNNNNN..",
                                  "................",
                                  "................"]]
    return ["".join(r) for r in rows]


def copy_rows():
    """Copy: two sheets of paper, one over the other (rows for pixel_icon)."""
    g = [["."] * 16 for _ in range(16)]

    def sheet(x0, y0, x1, y1):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                g[y][x] = "K" if x in (x0, x1) or y in (y0, y1) else "W"
        for y in range(y0 + 2, y1 - 1, 2):  # (its lines of writing)
            for x in range(x0 + 2, x1 - 1):
                g[y][x] = "D"
    sheet(1, 1, 8, 11)
    sheet(6, 4, 14, 14)
    return ["".join(r) for r in g]


FORMAT_ROWS = {  # (C: the colour bar under the letter, drawn in the colour chosen)
    "bold": ["................",
             "................",
             "...KKKKKKKK.....",
             "....KKK..KKK....",
             "....KKK...KKK...",
             "....KKK...KKK...",
             "....KKK..KKK....",
             "....KKKKKKK.....",
             "....KKK..KKK....",
             "....KKK...KKK...",
             "....KKK...KKK...",
             "....KKK..KKK....",
             "...KKKKKKKK.....",
             "................"],
    "italic": ["................",
               "................",
               ".......KKKKK....",
               "........KKK.....",
               "........KK......",
               ".......KKK......",
               ".......KK.......",
               "......KKK.......",
               "......KK........",
               ".....KKK........",
               ".....KK.........",
               "....KKK.........",
               "...KKKKK........",
               "................"],
    "underline": ["................",
                  "................",
                  "...KKK...KKK....",
                  "....K.....K.....",
                  "....K.....K.....",
                  "....K.....K.....",
                  "....K.....K.....",
                  "....K.....K.....",
                  "....K.....K.....",
                  ".....K...K......",
                  "......KKK.......",
                  "................",
                  "...KKKKKKKKK....",
                  "................"],
    "strike": ["................",
               "................",
               "......KKKK......",
               ".....K....K.....",
               "....K......K....",
               "....K...........",
               ".....KK.........",
               "..KKKKKKKKKKKK..",
               "..........K.....",
               "...........K....",
               "....K......K....",
               ".....K....K.....",
               "......KKKK......",
               "................"],
    "color": ["................",
              ".......K........",
              "......KKK.......",
              "......KKK.......",
              ".....KK.KK......",
              ".....KK.KK......",
              "....KK...KK.....",
              "....KKKKKKK.....",
              "...KK.....KK....",
              "...KK.....KK....",
              "..KKK.....KKK...",
              "................",
              "................",
              ".CCCCCCCCCCCCCC.",
              ".CCCCCCCCCCCCCC.",
              ".CCCCCCCCCCCCCC."],
    "marker": ["..........KK....",
               ".........KWWK...",
               "........KWWWWK..",
               ".......KWWWWK...",
               "......KWWWWK....",
               ".....KWWWWK.....",
               "....KDDWWK......",
               "...KDDDDK.......",
               "...KYYDK........",
               "..KYYKK.........",
               "..KKK...........",
               "................",
               "................",
               ".CCCCCCCCCCCCCC.",
               ".CCCCCCCCCCCCCC.",
               ".CCCCCCCCCCCCCC."],
    "case": ["................",
             "................",
             "....K...........",
             "...KKK..........",
             "...KKK..........",
             "..KK.KK.........",
             "..KK.KK...KKKK..",
             ".KK...KK.K...KK.",
             ".KKKKKKK.....KK.",
             ".KK...KK..KKKKK.",
             "KK.....KKK...KK.",
             "KK.....KKK..KKK.",
             "KK.....KK.KKK.KK",
             "................"],
    "grow": ["...........K....",
             "..........KKK...",
             ".........KK.KK..",
             "....K...........",
             "...KKK..........",
             "...KKK..........",
             "..KK.KK.........",
             "..KK.KK.........",
             ".KK...KK........",
             ".KKKKKKK........",
             "KK.....KK.......",
             "KK.....KK.......",
             "KK.....KK.......",
             "................"],
    "shrink": [".........KK.KK..",
               "..........KKK...",
               "...........K....",
               "................",
               "....K...........",
               "...KKK..........",
               "...KKK..........",
               "..KK.KK.........",
               "..KK.KK.........",
               ".KKKKKKK........",
               ".KK...KK........",
               "KK.....KK.......",
               "KK.....KK.......",
               "................"],
}


def align_rows(kind):
    """Lines of text lined up left, centred, right or justified (rows for pixel_icon)."""
    rows = []
    for n in range(16):
        if n < 2 or n > 13 or n % 2:
            rows.append("." * 16)
            continue
        w = 12 if (n // 2) % 2 or kind == "justify" else 8
        x = {"left": 2, "center": 2 + (12 - w) // 2, "right": 14 - w}.get(kind, 2)
        rows.append("." * x + "K" * w + "." * (16 - x - w))
    return rows


def direction_rows(kind):
    """A pilcrow with a little arrow: before it pointing right (left-to-right), or after it
    pointing left (right-to-left) - like Word's buttons."""
    mark = [".KKKKKK",
            "KKKKK.K",
            "KKKKK.K",
            "KKKKK.K",
            ".KKKK.K",
            "...KK.K",
            "...KK.K",
            "...KK.K",
            "...KK.K",
            "...KK.K",
            "...KK.K"]
    arrow = ["N..", "NN.", "NNN", "NN.", "N.."] if kind == "ltr" else ["..N", ".NN", "NNN", ".NN", "..N"]
    rows = [["."] * 16 for _ in range(16)]
    mx, ax = (8, 2) if kind == "ltr" else (2, 12)
    for y, row in enumerate(mark):
        for x, ch in enumerate(row):
            if ch != ".":
                rows[y + 2][mx + x] = ch
    for y, row in enumerate(arrow):
        for x, ch in enumerate(row):
            if ch != ".":
                rows[y + 5][ax + x] = ch
    return ["".join(r) for r in rows]


def format_icon(kind, color=None):
    """A text formatting button's picture (the colour ones with their colour bar)."""
    return pixel_icon(FORMAT_ROWS[kind], {"C": hex_rgb_255(color)} if color else {"C": (0, 0, 0)})


ERASER_TOOL_ROWS = [  # (the Eraser tool's button: a little bigger, a pixel lower than the
    "................",  # eraser the Remove buttons show)
    ".........KKK....",
    "........KPPPK...",
    ".......KPPPPPK..",
    "......KPPPPPPPK.",
    ".....KPPPPPPPK..",
    "....KWKPPPPPK...",
    "...KWWWKPPPK....",
    "..KWWWWWKPK.....",
    ".KWWWWWWWK......",
    ".KWWWWWWK.......",
    "..KWWWWK........",
    "...KWWK.........",
    "....KK..........",
    "................",
    ".DDDDDDDDDDDDD.."]


def make_icons():
    """Every toolbar picture, by name (Pillow pictures; the buttons make them Tk's)."""
    icons = {name: pixel_icon(rows) for name, rows in ICON_ROWS.items()}
    edit = Image.new("RGBA", (16, 16), (0, 0, 0, 0))  # the Edit tool's box: 1 px right and down
    edit.paste(icons["edit"].crop((0, 0, 15, 15)), (1, 1))
    icons["edit"] = edit
    icons["eraser_tool"] = pixel_icon(ERASER_TOOL_ROWS)  # (the Eraser tool's button)
    icons["image_tool"] = pixel_icon(["." * 16] + ICON_ROWS["image"])  # (the Image tool's button:
    # its picture a pixel lower)
    icons.update(up=arrow_icon("up"), down=arrow_icon("down"),
                 undo=pixel_icon(BACK_ROWS, BLOCK_ARROW), redo=pixel_icon(FORWARD_ROWS, BLOCK_ARROW),
                 zoom_out=lens_icon("-"), zoom_in=lens_icon("+"), sign=sign_icon(),
                 link=link_icon(), shape=shape_icon("rect", "#FF0000"),
                 find=lens_icon(), copy=pixel_icon(copy_rows()),
                 **{"mark_" + k: pixel_icon(marked_rows(k))
                    for k in ("highlight", "strike", "underline", "squiggly")},
                 **{"fmt_" + k: format_icon(k) for k in FORMAT_ROWS},
                 **{"align_" + k: pixel_icon(align_rows(k))
                    for k in ("left", "center", "right", "justify")},
                 **{"dir_" + k: pixel_icon(direction_rows(k)) for k in ("ltr", "rtl")})
    return icons


def swatch_icon(color):
    """The colour button's picture: the colour in a black-edged square."""
    im = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    ImageDraw.Draw(im).rectangle([1, 1, 14, 14], fill=color, outline=(0, 0, 0))
    return im


def engraved_icon(im):
    """A picture greyed out the way Windows 98 greys out toolbar pictures: its shape engraved
    into the face - in the edges' shadow colour, over a copy in their light colour 1 px down
    and right."""
    light, dark = engraved_colors()
    mask = im.getchannel("A").point(lambda a: 255 if a > 100 else 0)
    out = Image.new("RGBA", im.size, (0, 0, 0, 0))
    out.paste(Image.new("RGBA", im.size, light), (1, 1), mask)
    out.paste(Image.new("RGBA", im.size, dark), (0, 0), mask)
    return out


class Tooltip:
    """Windows' little yellow note with a button's name, shown when the mouse rests on it."""
    DELAY = 600

    def __init__(self, widget, text):
        self.widget, self.text = widget, text  # text: a string, or a function giving one
        self.tip, self.job = None, None
        widget.bind("<Enter>", lambda e: self.schedule(), add="+")
        widget.bind("<Leave>", lambda e: self.hide(), add="+")
        widget.bind("<ButtonPress>", lambda e: self.hide(), add="+")

    def schedule(self):
        self.hide()
        self.job = self.widget.after(self.DELAY, self.show)

    def show(self):
        self.job = None
        text = self.text() if callable(self.text) else self.text
        if not text:
            return
        w = self.widget
        tip = self.tip = tk.Toplevel(w)
        tip.overrideredirect(True)
        tip.attributes("-topmost", True)
        tk.Label(tip, text=text, bg="#FFFFE1", fg="black", relief="solid", bd=1, font=FONT,
                 padx=4, pady=1).pack()
        tip.update_idletasks()
        x = min(w.winfo_pointerx(), w.winfo_screenwidth() - tip.winfo_reqwidth() - 4)
        tip.geometry(f"+{x}+{w.winfo_rooty() + w.winfo_height() + 4}")

    def hide(self):
        if self.job:
            self.widget.after_cancel(self.job)
            self.job = None
        if self.tip:
            self.tip.destroy()
            self.tip = None


class ToolButton(tk.Canvas):
    """A Windows 98 toolbar button: flat, just its picture; under the mouse it rises (a 1 px
    raised edge), held down it sinks in. A tool that's in use (latched) stays sunk in on a
    checkered face, like Windows 98's toggled toolbar buttons. Greyed out, the picture is
    engraved. The mouse resting on it shows its name (tip)."""
    W, H = 23, 22
    ARROW_W = 11  # (a button with a list: the narrow part at its right with a little arrow)

    def __init__(self, parent, icon, tip, command, note=None, menu=None, label=None):
        """menu: for a button with a list - called with the button when its arrow is
        clicked (it shows the list). label: words shown beside the picture."""
        self.split = self.ARROW_W if menu else 0
        self.label = label
        self.label_w = font_of(FONT, parent).measure(label) + 6 if label else 0
        super().__init__(parent, width=self.W + self.split + self.label_w, height=self.H,
                         bg=BG, highlightthickness=0, bd=0)
        self.icon, self.tip, self.command, self.menu = icon, tip, command, menu
        self.note = note or tip  # What's This? text
        self.hot = self.down = self.latched = False
        self.enabled = True
        self._photo = None
        self.bind("<Enter>", lambda e: self.set_hot(True))
        self.bind("<Leave>", lambda e: self.set_hot(False))
        self.bind("<ButtonPress-1>", self.on_press)
        self.bind("<B1-Motion>", lambda e: self.set_hot(
            0 <= e.x < self.winfo_width() and 0 <= e.y < self.winfo_height()))
        self.bind("<ButtonRelease-1>", self.on_release)
        Tooltip(self, lambda: self.tip)
        self.draw()

    def set_icon(self, icon):
        self.icon = icon
        self.draw()

    def set_enabled(self, on):
        if on != self.enabled:
            self.enabled = on
            self.draw()

    def set_latched(self, on):
        if on != self.latched:
            self.latched = on
            self.draw()

    def set_hot(self, hot):
        if hot != self.hot:
            self.hot = hot
            self.draw()

    def on_press(self, e):
        if self.enabled:
            self.down = True
            self.draw()

    def on_release(self, e):
        go = self.down and self.hot and self.enabled
        self.down = False
        self.draw()
        if go and self.split and e.x >= int(self.cget("width")) - self.split:
            self.menu(self)  # (the arrow: the list)
        elif go:
            self.command()

    _checks = {}

    def draw(self):
        self.delete("all")
        W, H = int(self.cget("width")), int(self.cget("height"))
        pressed = self.down and self.hot
        sunk = self.latched or pressed
        if self.latched and not pressed:  # checkered face, like Windows 98's toggled buttons
            key = (W, H, theme_color(BG), theme_color(EDGE_LIGHT, "edge"))
            if key not in self._checks:
                a, b = (tuple(int(c[i:i + 2], 16) for i in (1, 3, 5)) for c in key[2:])
                img = Image.new("RGB", (W, H))
                img.putdata([a if (x + y) % 2 else b for y in range(H) for x in range(W)])
                self._checks[key] = ImageTk.PhotoImage(img, master=self)
            self.create_image(0, 0, image=self._checks[key], anchor="nw")
        if sunk:
            self.create_line(0, H - 1, 0, 0, W - 1, 0, fill=thin_shadow())
            self.create_line(1, H - 1, W - 1, H - 1, W - 1, 0, fill=EDGE_LIGHT)
        elif self.hot and self.enabled:
            self.create_line(0, H - 1, 0, 0, W - 1, 0, fill=EDGE_LIGHT)
            self.create_line(1, H - 1, W - 1, H - 1, W - 1, 0, fill=thin_shadow())
        icon = self.icon
        if THEME == "dark" and getattr(self, "whiten", None) is not None:
            icon = whitened(icon, self.whiten)  # (its black parts white: seen on the dark)
        icon = icon if self.enabled else engraved_icon(icon)
        self._photo = ImageTk.PhotoImage(icon, master=self)
        o = 1 if sunk else 0  # the picture shifts when pressed, like a real button
        x = self.W // 2 if self.label else (W - self.split) // 2
        self.create_image(x + o, H // 2 + o, image=self._photo)
        if self.label:
            self.create_text(self.W - 2 + o, H // 2 + o, text=self.label, anchor="w", font=FONT,
                             fill="#000000" if self.enabled else engraved_colors()[1])
        if self.split:  # the arrow part: a small black triangle, a line between when raised
            ax = W - self.split
            if self.hot and self.enabled and not sunk:
                self.create_line(ax, 2, ax, H - 2, fill=thin_shadow())
                self.create_line(ax + 1, 2, ax + 1, H - 2, fill=EDGE_LIGHT)
            ink = "#000000" if self.enabled else engraved_colors()[1]
            cx, cy = ax + self.split // 2 + o, H // 2 + o
            for row in range(3):
                self.create_rectangle(cx - 2 + row, cy - 1 + row, cx + 3 - row, cy + row,
                                      fill=ink, outline="")


WHITE_IN_DARK = {"text": 16, "sign": 16, "link": 16, "grow": 16, "shrink": 16, "fmt_bold": 16,
                 "fmt_italic": 16, "fmt_underline": 16, "fmt_strike": 16, "case": 16,
                 "text_color": 12,  # (its bar under the A: the colour chosen - left as it is)
                 "align_left": 16, "align_center": 16, "align_right": 16, "align_justify": 16,
                 "dir_ltr": 16, "dir_rtl": 16, "prev": 16, "next": 16}  # (the buttons whose pictures are black:
# in the dark theme those parts are white, down to that many rows from the top)
_WHITENED = {}


def whitened(icon, rows=16):
    """The picture with its black and dark grey parts made white (down to rows from
    the top) - its colours left as they are: how it shows in the dark theme."""
    key = (id(icon), icon.size, rows, icon.tobytes()[:64])
    if key not in _WHITENED:
        im = icon.convert("RGBA")
        px = im.load()
        for y in range(min(rows, im.height)):
            for x in range(im.width):
                r, g, b, a = px[x, y]
                if a and max(r, g, b) < 110 and max(r, g, b) - min(r, g, b) < 40:  # (black /
                    # dark grey - a colour, even a dark blue, is left as it is)
                    v = 255 - min(r, g, b) // 3  # (black: white; dark grey: a light grey)
                    px[x, y] = (v, v, v, a)
        _WHITENED[key] = im
        if len(_WHITENED) > 400:
            _WHITENED.pop(next(iter(_WHITENED)))
    return _WHITENED[key]


def tool_separator(parent):
    """The etched line between groups of toolbar buttons: shadow beside light."""
    f = tk.Frame(parent, bg=BG, width=2)
    f.pack(side="left", fill="y", padx=4, pady=2)
    tk.Frame(f, bg=EDGE_SHADOW, width=1).pack(side="left", fill="y")
    tk.Frame(f, bg=EDGE_LIGHT, width=1).pack(side="left", fill="y")
    return f


def etched_line(parent, **pack):
    """An etched line across: shadow over light (like the line under the menu bar)."""
    f = tk.Frame(parent, bg=BG)
    tk.Frame(f, bg=EDGE_SHADOW, height=1).pack(fill="x")
    tk.Frame(f, bg=EDGE_LIGHT, height=1).pack(fill="x")
    f.pack(fill="x", **pack)
    return f


def sunken_panel(parent):
    """A status bar panel: a 1 px sunken edge (shadow top / left, light bottom / right) round
    a face-coloured inside, which is returned with the outside: (outside, inside)."""
    outer = tk.Frame(parent, bg=EDGE_LIGHT)
    mid = tk.Frame(outer, bg=EDGE_SHADOW)
    mid.pack(fill="both", expand=True, padx=(0, 1), pady=(0, 1))
    inner = tk.Frame(mid, bg=BG)
    inner.pack(fill="both", expand=True, padx=(1, 0), pady=(1, 0))
    return outer, inner


def center_dialog(win, owner):
    """Put a dialog in the middle of its window (a little above the middle), on the screen."""
    win.update_idletasks()
    w, h = win.winfo_reqwidth(), win.winfo_reqheight()
    x = owner.winfo_rootx() + (owner.winfo_width() - w) // 2
    y = owner.winfo_rooty() + (owner.winfo_height() - h) // 3
    x = min(max(0, x), win.winfo_screenwidth() - w)
    y = min(max(0, y), win.winfo_screenheight() - h - 40)
    win.geometry(f"+{x}+{y}")


def new_dialog(owner, title):
    """A Windows 98 dialog box over owner: (window, its ClassicWindow, its body)."""
    owner = owner.winfo_toplevel()
    win = tk.Toplevel(owner)
    win.configure(bg=BG)
    win.resizable(False, False)
    win.transient(owner)
    chrome = ClassicWindow(win, title, win.destroy, resizable=False, taskbar=False)
    return win, chrome, chrome.body


def show_dialog(win, owner, focus=None):
    """Centre the dialog, hand it the keyboard and wait for it to close."""
    center_dialog(win, owner.winfo_toplevel())
    win.focus_force()  # windows without Windows' title bar don't take the keyboard by themselves
    if focus is not None:
        focus.focus_set()
    win.grab_set()
    win.wait_window()


def text_dialog(owner, title, prompt, text="", multiline=True, secret=False):
    """Ask for some text, in the app's own style: a sticky note's text (several lines), or a
    password (secret: shown as stars). Returns the text, or None if cancelled."""
    win, chrome, body = new_dialog(owner, title)
    result = [None]
    tk.Label(body, text=prompt, bg=BG, font=FONT, justify="left").pack(
        anchor="w", padx=12, pady=(12, 4))
    if multiline:
        box = tk.Text(body, width=44, height=7, font=FONT, wrap="word", bg="white",
                      relief="sunken", bd=2, padx=4, pady=2, highlightthickness=0)
        box.insert("1.0", text)
        get = lambda: box.get("1.0", "end-1c")  # noqa: E731
    else:
        var = tk.StringVar(value=text)
        box = tk.Entry(body, textvariable=var, font=FONT, width=38, relief="sunken", bd=2,
                       bg="white", show="*" if secret else "")
        get = var.get
    box.pack(padx=12)

    def ok(_=None):
        result[0] = get()
        win.destroy()
        return "break"
    row = tk.Frame(body, bg=BG)
    row.pack(pady=(10, 12))
    xp_button(row, "OK", ok).pack(side="left", padx=4)
    xp_button(row, "Cancel", win.destroy).pack(side="left", padx=4)
    win.bind("<Escape>", lambda e: win.destroy())
    if multiline:  # (Enter starts a new line: Ctrl + Enter is OK)
        box.bind("<Control-Return>", ok)
    else:
        win.bind("<Return>", ok)
    show_dialog(win, owner, focus=box)
    return result[0]


def hex_rgb(color):
    """ "#FF8000" -> (1.0, 0.5, 0.0), the way PDFs write colours."""
    return tuple(int(color[i:i + 2], 16) / 255 for i in (1, 3, 5))


def hex_rgb_255(color):
    """ "#FF8000" -> (255, 128, 0)."""
    return tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))


def rgb_hex(rgb):
    return "#" + "".join(f"{round(max(0, min(1, v)) * 255):02X}" for v in rgb[:3])


ZOOM_STEPS = list(range(25, 401, 25))  # 25% to 400%, a step of 25% each
ZOOMS = ["Fit page", "Fit width"] + [f"{z}%" for z in ZOOM_STEPS]
SCREEN_DPI = 96  # 100% shows a page at its real size on a normal screen
MOVABLE = ("FreeText", "Text", "Ink", "Square", "Circle")
MARKUPS = ("Highlight", "StrikeOut", "Underline", "Squiggly")  # marks on the text itself


def quad_rects(annot):
    """A text mark's pieces (one per line it covers), as rectangles in the page's own space."""
    v = annot.vertices or []
    out = []
    for k in range(0, len(v) - 3, 4):
        xs, ys = [pt[0] for pt in v[k:k + 4]], [pt[1] for pt in v[k:k + 4]]
        out.append(pymupdf.Rect(min(xs), min(ys), max(xs), max(ys)))
    return out or [pymupdf.Rect(annot.rect)]


def same_line(a, b):
    """Do two rectangles sit on the same line (overlapping by half the height or more)?"""
    return min(a.y1, b.y1) - max(a.y0, b.y0) >= 0.5 * min(a.height, b.height)


def cut_out(rect, holes):
    """What's left of rect with the parts under holes (on its line) taken out."""
    pieces = [pymupdf.Rect(rect)]
    for h in holes:
        if not same_line(rect, h):
            continue
        rest = []
        for r in pieces:
            if h.x1 <= r.x0 or h.x0 >= r.x1:
                rest.append(r)
                continue
            if h.x0 - r.x0 > 0.5:
                rest.append(pymupdf.Rect(r.x0, r.y0, h.x0, r.y1))
            if r.x1 - h.x1 > 0.5:
                rest.append(pymupdf.Rect(h.x1, r.y0, r.x1, r.y1))
        pieces = rest
    return pieces  # annotations that can be dragged about


# ---- sharp pages on a scaled screen ----
# The app isn't "DPI aware": on a screen scaled above 100% (125%, 150%...), Windows draws the
# whole window at 100% and stretches it. That keeps the Windows 98 look (the crisp bitmap
# font, pixel-sized edges) - but a page's picture, stretched, comes out blurry. So pages are
# drawn at the screen's real resolution and then made smaller in a way that Windows' stretch
# turns back into (nearly) that sharp picture, instead of a soft one.
def screen_stretch(widget):
    """How much Windows stretches the app on the screen it's on (1.25 at 125%); 1 if not."""
    if sys.platform != "win32":
        return 1.0
    try:
        import ctypes
        u = ctypes.windll.user32
        u.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
        u.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        u.MonitorFromWindow.restype = ctypes.c_void_p
        u.MonitorFromWindow.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        old = u.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))  # (asked as a DPI-aware app,
        try:  # Windows tells the real numbers)
            monitor = u.MonitorFromWindow(widget.winfo_id(), 2)  # MONITOR_DEFAULTTONEAREST
            dx, dy = ctypes.c_uint(), ctypes.c_uint()
            ctypes.windll.shcore.GetDpiForMonitor(ctypes.c_void_p(monitor), 0, ctypes.byref(dx),
                                                  ctypes.byref(dy))  # MDT_EFFECTIVE_DPI
        finally:
            u.SetThreadDpiAwarenessContext(ctypes.c_void_p(old))
        return max(1.0, dx.value / 96)
    except Exception:
        return 1.0


def page_picture(page, scale, stretch=1.0):
    """Page drawn at scale (screen pixels per point) as a Pillow picture, ready for a screen
    that Windows stretches by stretch: drawn that much bigger, then made smaller in steps that
    each look at how the stretched result differs from the sharp picture and correct for it
    (three rounds of "back-projection", each pushed a little past the difference: Windows'
    stretch is softer than a plain one) - so after the stretch, the text is crisp."""
    if stretch <= 1.01:
        pix = page.get_pixmap(matrix=pymupdf.Matrix(scale, scale), alpha=False)
        return Image.frombytes("RGB", (pix.width, pix.height), pix.samples)
    pix = page.get_pixmap(matrix=pymupdf.Matrix(scale * stretch, scale * stretch), alpha=False)
    sharp = Image.frombytes("RGB", (pix.width, pix.height), pix.samples)
    return sharpened(sharp, (max(1, round(pix.width / stretch)), max(1, round(pix.height / stretch))))


TILE = 512  # (pages are drawn in squares this many screen pixels across: only what's in view)
TILE_PAD = 8  # (each drawn a little bigger and cut down: no seams between them)


def tile_picture(page, scale, stretch, box, page_size):
    """Part of a page - box: (x0, y0, x1, y1) in screen pixels from the page's top left as
    it's shown, page_size its (width, height) in them - drawn and sharpened like
    page_picture. A whole page at 400% is some 20 million pixels and took seconds; a tile
    takes a few hundredths."""
    w, h = page_size
    x0, y0, x1, y1 = box
    p0, q0 = max(0, x0 - TILE_PAD), max(0, y0 - TILE_PAD)
    p1, q1 = min(w, x1 + TILE_PAD), min(h, y1 + TILE_PAD)
    clip = pymupdf.Rect(p0 / scale, q0 / scale, p1 / scale, q1 / scale)
    size = (max(1, p1 - p0), max(1, q1 - q0))
    k = scale * (stretch if stretch > 1.01 else 1)
    pix = page.get_pixmap(matrix=pymupdf.Matrix(k, k), clip=clip, alpha=False)
    img = Image.frombytes("RGB", (pix.width, pix.height), pix.samples)
    if stretch > 1.01:
        big = (round(size[0] * stretch), round(size[1] * stretch))
        if img.size != big:  # (MuPDF rounds the clip's edges: exactly the size it stands for)
            img = img.resize(big, Image.BILINEAR)
        img = sharpened(img, size)
    elif img.size != size:
        img = img.resize(size, Image.BILINEAR)
    return img.crop((x0 - p0, y0 - q0, x1 - p0, y1 - q0))


def sharpened(sharp, size):
    """A picture drawn bigger made size, for Windows' stretch to turn back into the sharp one
    (see page_picture)."""
    import numpy as np
    want = np.asarray(sharp, dtype=np.int16)
    small = sharp.resize(size, Image.BOX)
    for _ in range(3):
        seen = np.asarray(small.resize(sharp.size, Image.BILINEAR), dtype=np.int16)  # (as Windows shows it)
        off = Image.fromarray(np.clip(want - seen + 128, 0, 255).astype(np.uint8)).resize(size, Image.BOX)
        fixed = np.asarray(small, dtype=np.int16) + 1.5 * (np.asarray(off, dtype=np.int16) - 128)
        small = Image.fromarray(np.clip(fixed, 0, 255).astype(np.uint8))  # (tuned on a 125% screen)
    return small


# ---- editing what's in the PDF itself: its text lines and pictures ----
# Text is changed the way PDF editors do it: the line's letters are taken out of the page
# (a "redaction" of just that line, which leaves pictures and drawings alone), then the new
# text is written in its place - in the same font when it's installed on this computer (the
# copy inside a PDF usually holds only the letters it used), else the closest one.
# Pictures keep their place in the page's drawing instructions: moving or resizing one just
# changes the position it's drawn at, so nothing else on the page is touched.
FONT_FOLDERS = [os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts"),
                os.path.join(os.environ.get("LOCALAPPDATA", ""), "Microsoft", "Windows", "Fonts")]
BASE_FONTS = {"he": ("helv", "heit", "hebo", "hebi"), "ti": ("tiro", "tiit", "tibo", "tibi"),
              "co": ("cour", "coit", "cobo", "cobi")}  # PDF's own fonts: plain, italic, bold, both
RTL_LETTERS = re.compile("[\u0590-\u08ff\ufb1d-\ufdff\ufe70-\ufeff]")  # Hebrew, Arabic...


def font_key(name):
    """A font's name made comparable: "ABCDEF+TimesNewRomanPS-BoldMT" and "Times New Roman
    Bold" both become "timesnewromanbold"."""
    n = re.sub(r"[^a-z0-9]", "", name.split("+")[-1].lower())
    n = n.replace("oblique", "italic").replace("regular", "").replace("normal", "")
    n = re.sub(r"mt$", "", n)
    n = re.sub(r"ps$", "", n)
    return re.sub(r"ps(?=bold|italic|$)", "", n)


def sfnt_names(path):
    """The names of the (first) font in a .ttf / .otf / .ttc file, read straight from its
    name table: [full name, family + style, ...], its family, and its style ("Bold
    Italic"...)."""
    with open(path, "rb") as f:
        head = f.read(12)
        base = 0
        if head[:4] == b"ttcf":  # a collection: its first font
            base = struct.unpack(">I", f.read(4))[0]
            f.seek(base)
            head = f.read(12)
        count = struct.unpack(">H", head[4:6])[0]
        dirs = f.read(16 * count)
        for k in range(count):
            tag, _, off, length = struct.unpack(">4sIII", dirs[16 * k:16 * k + 16])
            if tag == b"name":
                f.seek(off)
                table = f.read(length)
                break
        else:
            return [], None, ""
    _, n, strings = struct.unpack(">HHH", table[:6])
    found = {}
    for r in range(n):
        pid, eid, lid, nid, ln, of = struct.unpack(">HHHHHH", table[6 + 12 * r:18 + 12 * r])
        if nid not in (1, 2, 4, 6) or pid not in (1, 3):
            continue
        raw = table[strings + of:strings + of + ln]
        text = raw.decode("utf-16-be" if pid == 3 else "latin-1", "ignore").strip()
        english = pid == 1 or lid == 0x409
        if text and (english or nid not in found):
            found[nid] = text
    names = [found.get(4, ""), f"{found.get(1, '')} {found.get(2, '')}", found.get(6, "")]
    return [x for x in names if x.strip()], found.get(1), found.get(2, "")


# font key -> (file, family); family key -> its fonts: [(file, family, bold, italic,
# plain)]; file -> (bold, italic) it really is; and the pymupdf.Fonts loaded
_FONTS = {"index": None, "families": {}, "styles": {}, "fonts": {}, "cut": {}}


def style_of(subfamily):
    """(bold, italic) from a font's style name ("Bold Italic", "Black", "Oblique"...)."""
    s = subfamily.lower()
    return (any(w in s for w in ("bold", "black", "heavy")),
            any(w in s for w in ("italic", "oblique")))


def scan_fonts():
    """Every font installed on this computer, by font_key (run on a worker thread at start:
    it only reads the files' name tables, so it doesn't need MuPDF)."""
    index, families, styles, cut = {}, {}, {}, {}
    for folder in FONT_FOLDERS:
        try:
            files = sorted(os.listdir(folder))
        except OSError:
            continue
        for file in files:
            if not file.lower().endswith((".ttf", ".otf", ".ttc")):
                continue
            path = os.path.join(folder, file)
            try:
                names, family, subfamily = sfnt_names(path)
            except Exception:
                continue
            if not names:
                continue
            for name in names:
                index.setdefault(font_key(name), (path, family or name))
                # (PDF readers cut long names at 31 letters: found by that too)
                cut.setdefault(name.replace(" ", "")[:31].lower(), (path, family or name))
            bold, italic = styles[path] = style_of(subfamily)
            plain = font_key(subfamily) in ("", "book")  # (its family's everyday style)
            families.setdefault(font_key(family or names[0]), []).append(
                (path, family or names[0], bold, italic, plain))
    _FONTS["families"], _FONTS["styles"], _FONTS["cut"] = families, styles, cut
    _FONTS["index"] = index


def family_font(key, bold, italic):
    """(file, family) of the font of a family (by its key) closest to bold / italic - the
    family's own bold or italic when it has one, else its plain one."""
    members = (_FONTS["families"] or {}).get(key)
    if not members:
        return None
    best = min(members, key=lambda m: (2 * (m[2] != bold) + (m[3] != italic), not m[4]))
    return best[0], best[1]


def family_and_style(key):
    """A font key as (family key, bold, italic) - "centurybolditalic": Century, bold and
    italic - or None if it doesn't start with a family's name. (PDF readers cut font names
    at 31 letters, so "...-BoldItal" counts as bold italic too.)"""
    families = _FONTS["families"] or {}
    if key in families:
        return key, False, False
    best = None
    for fk in families:
        rest = key[len(fk):] if key.startswith(fk) else None
        if rest and (rest == "bi" or any(s.startswith(rest) for s in ("bold", "italic",
                                                                      "bolditalic"))) \
                and (best is None or len(fk) > len(best[0])):
            best = (fk, rest.startswith("b"),
                    rest.startswith("i") or rest == "bi" or len(rest) > 4)
    return best


def chosen_style(family, bold, italic):
    """Of a font's bold / italic, what was chosen: a family that's only ever italic (or
    bold) isn't italic by choice."""
    members = (_FONTS["families"] or {}).get(font_key(family)) or []
    if not members:
        return bold, italic
    return bold and any(not m[2] for m in members), italic and any(not m[3] for m in members)


def real_style(path):
    """(bold, italic) a font file really is (False, False if it's not known)."""
    return _FONTS["styles"].get(path, (False, False))


def find_font(span_font, flags):
    """(file, Tk family, bold, italic) of the installed font closest to a span's font (None
    as the file: none found - then one of PDF's own built-in fonts is used)."""
    index = _FONTS["index"] or {}
    key = font_key(span_font)
    raw = span_font.split("+")[-1].replace(" ", "").lower()
    if key in index or (len(raw) >= 31 and raw[:31] in _FONTS["cut"]):
        # the very font: as bold / italic as it really is
        path, family = index[key] if key in index else _FONTS["cut"][raw[:31]]
        return (path, family) + real_style(path)
    found = family_and_style(key)
    if found:  # a family, and a style after it ("Century-Italic": Century, in italic - what
        k, bold, italic = found  # Master PDF calls fonts it thickens or slants itself)
        path, family = family_font(k, bold, italic)
        rb, ri = real_style(path)
        return path, family, bold or rb, italic or ri
    bold = bool(flags & 16) or "bold" in key or "black" in key or "heavy" in key
    italic = bool(flags & 2) or "italic" in key
    style = ("bold" if bold else "") + ("italic" if italic else "")
    plain = re.sub(r"(semibold|bold|black|heavy|italic|light|medium)+$", "", key)
    for k in (plain + style, plain):
        if k in index:
            return index[k] + (bold, italic)
    sans = any(w in key for w in ("sans", "aptos", "calibri", "arial", "helvetica", "segoe",
                                   "verdana", "tahoma", "roboto", "inter", "lato", "gothic",
                                   "grotesk", "frutiger", "univers", "myriad"))
    serif = not sans and (bool(flags & 4) or any(w in key for w in (
        "times", "serif", "georgia", "garamond", "cambria", "roman", "book")))
    mono = bool(flags & 8) or bool(re.search(r"courier|mono(?!type)|consol", key))
    family = "couriernew" if mono else "timesnewroman" if serif else "arial"
    for k in (family + style, family):
        if k in index:
            return index[k] + (bold, italic)
    return None, ("Courier New" if mono else "Times New Roman" if serif else "Arial"), bold, italic


def loaded_font(path):
    """(PDF_LOCK held) a pymupdf.Font for a font file, made once."""
    if path not in _FONTS["fonts"]:
        _FONTS["fonts"][path] = pymupdf.Font(fontfile=path)
    return _FONTS["fonts"][path]


def tagged_owners(doc, i):
    """(PDF_LOCK held) {letter (char_key): piece} for the text Master PDF wrote on page i - read
    from its labels (see TEXT_TAG), so its pieces are the same after the PDF is saved and
    opened again."""
    page = doc[i]
    found = {}
    for xref in page.get_contents():
        for m in TAGGED.finditer(doc.xref_stream(xref)):
            found.setdefault(int(m.group(1)), []).append(m.group(0))
    if not found:
        return {}
    tmp = pymupdf.open()
    tmp.insert_pdf(doc, from_page=i, to_page=i)
    tp = tmp[0]
    first = tp.get_contents()[0]
    for x in tp.get_contents()[1:]:
        tmp.update_stream(x, b" ")
    out = {}
    for tag, blocks in found.items():  # (each piece drawn on its own, and read)
        tmp.update_stream(first, b"\n".join(blocks))
        for b in tp.get_text("rawdict")["blocks"]:
            for line in b.get("lines", []):
                for s in line["spans"]:
                    for ch in s["chars"]:
                        out[char_key(ch)] = tag
    tmp.close()
    return out


def char_key(ch):
    """A letter, by what it is and where: (letter, x, y) - to know it again later."""
    o = ch.get("origin") or ch["bbox"][:2]
    return ch["c"], round(o[0] * 4), round(o[1] * 4)


def page_objects(page, owner=None):
    """(PDF_LOCK held) what's on the page that can be edited: its text lines and pictures,
    as dicts with "kind" ("text" / "image") and "rect" (in the page's own space). owner:
    owner(letter) -> the piece of text (a number) that letter is known to belong to, or
    None - the letters of a piece always make that piece, never mixed with others."""
    out = []
    mid = page.cropbox.width / 2  # (the page's middle, across)
    blocks = page.get_text("rawdict")["blocks"]
    # (where the page's lines start across: a piece starting where others do starts a column)
    starts = [line["bbox"][0] for b in blocks if b["type"] == 0 for line in b["lines"]]
    for block in blocks:
        if block["type"] != 0:
            continue
        first = len(out)
        for line in block["lines"]:
            if abs(line["dir"][1]) > 0.01 or line["dir"][0] < 0:
                continue  # (only text going straight across can be retyped)
            for s in line["spans"]:
                s["text"] = "".join(ch["c"] for ch in s["chars"])
            mine = {}  # (the line's letters by the piece each belongs to: a line made of two
            for s in line["spans"]:  # pieces, one over the other, is two lines)
                for ch in s["chars"]:
                    mine.setdefault(owner(ch) if owner else None, {}).setdefault(
                        id(s), (s, []))[1].append(ch)
            for who, spans_of in mine.items():
                spans_now = [piece_span(s, chs) for s, chs in spans_of.values()] \
                    if len(mine) > 1 else line["spans"]
                for part in split_at_gaps(spans_now):  # (a tab-wide gap: two pieces of text)
                    spans = [s for s in part if s["text"].strip()]
                    if not spans:
                        continue
                    text = "".join(s["text"] for s in part)
                    text = re.sub(r" {2,}", " ", text.replace("\xa0", " ").replace("\ufffd", " ")).strip()
                    rect = pymupdf.Rect(spans[0]["bbox"])
                    for s in spans[1:]:
                        rect |= s["bbox"]
                    out.append({"kind": "text", "rect": rect, "spans": spans, "text": text,
                                "all": part, "page_mid": mid, "owner": who})  # (all: with
                    # the spaces between, letter by letter)
        # (a justified line written word by word reads back as pieces on one baseline:
        # put together again, a space between)
        joined = []
        for o in sorted(out[first:], key=lambda o: (round(o["spans"][0]["origin"][1], 1),
                                                    o["rect"].x0)):
            prev = joined[-1] if joined else None
            if prev is not None:
                same = abs(prev["spans"][0]["origin"][1] - o["spans"][0]["origin"][1]) < 0.6
                size = max(s["size"] for s in prev["spans"] + o["spans"])
                gap = o["rect"].x0 - prev["rect"].x1
                column = sum(1 for x in starts if abs(x - o["rect"].x0) < 1) >= 3
                if same and -1 < gap < 1.6 * size and not column and \
                        prev.get("owner") == o.get("owner"):
                    last = prev["all"][-1]
                    space = dict(last, text=" ", chars=[{"c": " ", "origin": (prev["rect"].x1, 0),
                                                         "bbox": (prev["rect"].x1, prev["rect"].y0,
                                                                  o["rect"].x0, prev["rect"].y1)}])
                    prev["spans"] = prev["spans"] + o["spans"]
                    prev["all"] = prev["all"] + [space] + o["all"]
                    prev["text"] = re.sub(r" {2,}", " ", prev["text"] + " " + o["text"])
                    prev["rect"] = prev["rect"] | o["rect"]
                    continue
            joined.append(o)
        out[first:] = joined
        peers = [o["rect"] for o in out[first:]]  # (the paragraph's lines, top to bottom)
        for o in out[first:]:
            o["peers"] = peers
    # a line that's a paragraph on its own (PDFs often keep each line apart - and a line
    # retyped here is put apart): the lines just over and under it, across from it, count
    for o in out:
        if len(o["peers"]) > 1:
            continue
        r, near = o["rect"], []
        for q in out:
            s = q["rect"]
            gap = max(s.y0 - r.y1, r.y0 - s.y1)
            if (q is not o and -0.5 * min(r.height, s.height) < gap < 0.8 * max(r.height, s.height)
                    and min(r.x1, s.x1) - max(r.x0, s.x0) > 0):
                near.append(s)
        if near:
            o["peers"] = sorted([r] + near, key=lambda x: x.y0)
    # the column it's in: as wide as the lines near it (within 3 lines) that it's across from
    # - what an alignment chosen lines it up in, when its paragraph is just itself
    for o in out:
        r = o["rect"]
        x0, x1 = r.x0, r.x1
        for q in out:
            s = q["rect"]
            if (abs((s.y0 + s.y1) - (r.y0 + r.y1)) / 2 < 3 * max(r.height, s.height)
                    and min(r.x1, s.x1) - max(r.x0, s.x0) > 0):
                x0, x1 = min(x0, s.x0), max(x1, s.x1)
        o["column"] = (x0, x1)
    texts = [o for o in out]
    out = []
    cells = page_edges(page)
    known = {}  # (lines of a piece already known: that piece, as it is - not grouped again)
    for o in texts:
        if o.get("owner") is not None:
            known.setdefault(o["owner"], []).append(o)
    frames = []
    for who, lines in known.items():
        lines.sort(key=lambda o: (o["rect"].y0, o["rect"].x0))
        rect = pymupdf.Rect(lines[0]["rect"])
        for o in lines[1:]:
            rect |= o["rect"]
        frames.append({"lines": lines, "rect": rect, "owner": who,
                       "paras": split_paragraphs(lines, rect)})
    loose = [o for o in texts if o.get("owner") is None]
    frames += cell_frames(group_frames(loose, page_rules(page)), cells, page.rect)
    for f in frames:
        for obj in frame_objects(f["lines"], mid, f["paras"], cells, page.rect):
            if f.get("owner") is not None:
                obj["oid"] = f["owner"]
            out.append(obj)
    names = {}
    for item in page.get_images(full=True):
        names.setdefault(item[0], item[7])
    try:
        page._image_info = None  # (the page keeps the pictures' places from the first time it's asked)
    except Exception:
        pass
    infos = page.get_image_info(xrefs=True)
    uses = {}  # a picture used several times (a signature in every row...) is one picture
    for info in infos:  # drawn at several places: which time this is, of how many
        uses[info.get("xref")] = uses.get(info.get("xref"), 0) + 1
    seen = {}
    for k, info in enumerate(infos):
        xref = info.get("xref")
        nth = seen[xref] = seen.get(xref, -1) + 1
        rect = pymupdf.Rect(info["bbox"])
        if xref and xref in names and not rect.is_empty:
            out.append({"kind": "image", "rect": rect, "xref": xref, "nth": nth,
                        "uses": uses[xref], "order": k, "pictures": len(infos),
                        "transform": pymupdf.Matrix(info["transform"])})
    return out


def split_at_gaps(spans):
    """A line's spans cut where there's a gap far wider than a space (over 1.8 times the
    text's size) - a tab, or a table's columns written as one line ("Company Address    :
    Junction Express"): each part its own piece of text. (Justified text's gaps are never
    that wide.) Returns lists of spans, each keeping only its letters in that part."""
    parts, cur, end = [], [], None
    for s in spans:
        piece = []
        for ch in s.get("chars", []):
            if ch["c"].strip():
                if end is not None and ch["bbox"][0] - end > 1.8 * s["size"]:
                    if piece:
                        cur.append(piece_span(s, piece))
                        piece = []
                    if cur:
                        parts.append(cur)
                    cur = []
                end = ch["bbox"][2]
            piece.append(ch)
        if piece:
            cur.append(piece_span(s, piece))
    if cur:
        parts.append(cur)
    return parts or [spans]


def piece_span(span, chars):
    """A span with just these letters."""
    xs = [c["bbox"][0] for c in chars] + [c["bbox"][2] for c in chars]
    return dict(span, chars=chars, text="".join(c["c"] for c in chars),
                bbox=(min(xs), span["bbox"][1], max(xs), span["bbox"][3]),
                origin=tuple(chars[0]["origin"]) if chars[0].get("origin") else span["origin"])


def page_rules(page):
    """(PDF_LOCK held) The page's drawn lines running across (table borders, rules) and the
    tops / bottoms of its boxes: text on either side of one isn't one frame."""
    out = []
    for d in page.get_drawings():
        for it in d["items"]:
            if it[0] == "l":
                a, b = it[1], it[2]
                if abs(a.y - b.y) < 1.5 and abs(a.x - b.x) > 4:
                    out.append(pymupdf.Rect(min(a.x, b.x), min(a.y, b.y) - 0.5, max(a.x, b.x),
                                            max(a.y, b.y) + 0.5))
            elif it[0] in ("re", "qu"):
                r = pymupdf.Rect(it[1]) if it[0] == "re" else it[1].rect
                if r.height < 2.5 and r.width > 4:  # (a thin bar: a rule)
                    out.append(r)
                elif "s" in (d.get("type") or ""):  # (an outlined box: its top and bottom -
                    out += [pymupdf.Rect(r.x0, r.y0 - 0.5, r.x1, r.y0 + 0.5),  # shading behind
                            pymupdf.Rect(r.x0, r.y1 - 0.5, r.x1, r.y1 + 0.5)]  # text isn't one)
    return out


def page_edges(page):
    """(PDF_LOCK held) The page's drawn lines, as ("h", y, x0, x1) across and ("v", x, y0, y1)
    down - lines, thin bars, and boxes' sides (a table's borders, however it's drawn)."""
    out = []

    def seg(a, b):
        if abs(a[1] - b[1]) < 1.5 and abs(a[0] - b[0]) > 2:
            out.append(("h", (a[1] + b[1]) / 2, min(a[0], b[0]), max(a[0], b[0])))
        elif abs(a[0] - b[0]) < 1.5 and abs(a[1] - b[1]) > 2:
            out.append(("v", (a[0] + b[0]) / 2, min(a[1], b[1]), max(a[1], b[1])))
    for d in page.get_drawings():
        for it in d["items"]:
            if it[0] == "l":
                seg(it[1], it[2])
            elif it[0] in ("re", "qu"):
                r = pymupdf.Rect(it[1]) if it[0] == "re" else it[1].rect
                if r.height < 2.5 and r.width > 2:  # (a thin bar: a line)
                    out.append(("h", (r.y0 + r.y1) / 2, r.x0, r.x1))
                elif r.width < 2.5 and r.height > 2:
                    out.append(("v", (r.x0 + r.x1) / 2, r.y0, r.y1))
                elif "s" in (d.get("type") or ""):  # (an outlined box: its four sides)
                    for a, b in ((r.tl, r.tr), (r.bl, r.br), (r.tl, r.bl), (r.tr, r.br)):
                        seg(a, b)
    return out


def find_cell(rect, edges, page_rect=None):
    """The table cell rect is in - the nearest drawn lines left, right, over and under it,
    each running along the whole of that side - or None (not in a table)."""
    cx, cy = (rect.x0 + rect.x1) / 2, (rect.y0 + rect.y1) / 2
    left = [x for k, x, a, b in edges if k == "v" and x <= rect.x0 + 1 and a <= rect.y0 + 1
            and b >= rect.y1 - 1]
    right = [x for k, x, a, b in edges if k == "v" and x >= rect.x1 - 1 and a <= rect.y0 + 1
             and b >= rect.y1 - 1]
    if not left or not right:
        return None
    x0, x1 = max(left), min(right)
    top = [y for k, y, a, b in edges if k == "h" and y <= rect.y0 + 1 and a <= x0 + 2 and b >= x1 - 2]
    bottom = [y for k, y, a, b in edges if k == "h" and y >= rect.y1 - 1 and a <= x0 + 2
              and b >= x1 - 2]
    if not top or not bottom or not x0 < cx < x1:
        return None
    cell = pymupdf.Rect(x0, max(top), x1, min(bottom))
    if page_rect is not None and (cell.width > 0.95 * page_rect.width
                                  or cell.height > 0.5 * page_rect.height):
        return None  # (a page's border, not a table's cell)
    return cell if cell.y0 < cy < cell.y1 else None


def cell_frames(frames, edges, page_rect=None):
    """Frames in the same table cell made one (a cell is one text box, as in Word); each
    frame in a cell knows it ("cell")."""
    out, seen = [], {}
    for f in frames:
        cell = find_cell(f["rect"], edges, page_rect)
        if cell is None:
            out.append(f)
            continue
        key = tuple(round(v) for v in cell)
        if key in seen:
            g = seen[key]
            g["lines"] = sorted(g["lines"] + f["lines"], key=lambda o: (o["rect"].y0, o["rect"].x0))
            g["rect"] = g["rect"] | f["rect"]
            g["paras"] = None
        else:
            f["cell"] = cell
            seen[key] = f
            out.append(f)
    for f in out:
        if f.get("paras") is None:
            f["paras"] = split_paragraphs(f["lines"], f["rect"])
    return out


def cell_span(obj, how):
    """(left, right) a frame in a table cell wraps between: the cell, less the room its text
    keeps from the cell's sides - or None (not in a cell)."""
    cell, r = obj.get("cell"), obj["rect"]
    if cell is None:
        return None
    size = max(line_size(o) for o in obj["lines"])
    pad = max(1.0, min(r.x0 - cell.x0, cell.x1 - r.x1, 0.6 * size))
    if how == "right":
        return (cell.x0 + max(pad, cell.x1 - r.x1), r.x1)
    if how == "center":
        return (cell.x0 + pad, cell.x1 - pad)
    return (r.x0, cell.x1 - max(pad, r.x0 - cell.x0))


def line_size(o):
    return max(s["size"] for s in o["spans"])


def first_word_width(o):
    """How wide a line's first word is (from its letters)."""
    chars = [ch for s in o.get("all") or o["spans"] for ch in s.get("chars", [])]
    chars = [ch for ch in chars]
    while chars and not chars[0]["c"].strip():
        chars.pop(0)
    word = []
    for ch in chars:
        if not ch["c"].strip():
            break
        word.append(ch)
    return (word[-1]["bbox"][2] - word[0]["bbox"][0]) if word else 0


def group_frames(lines, rules=()):
    """Lines into frames - text boxes, like iLovePDF's: lines one under the other, lined up
    (left, centre or right), the same size, spaced as the frame's lines are (a bigger gap -
    a heading's, a paragraph's spacing - starts another), and not split by a drawn line. Each
    frame then into paragraphs: a first-line indent, or a line that ended though the next
    one's first word would have fitted on it (a list, an address...). Judged by the text's
    own sizes and spacing, so it works for any PDF - tightly set reports, airy designs."""
    frames = []
    for o in sorted(lines, key=lambda o: (o["rect"].y0, o["rect"].x0)):
        r, h = o["rect"], o["rect"].height
        best = None
        for f in frames:
            last = f["lines"][-1]["rect"]
            gap = r.y0 - last.y1
            fr = f["rect"]
            over = min(r.x1, fr.x1) - max(r.x0, fr.x0)
            size = max(line_size(o), line_size(f["lines"][-1]))
            lined_up = any(abs(r.x0 - q.x0) < 2.2 * size or abs(r.x1 - q.x1) < 2.2 * size
                           or abs((r.x0 + r.x1 - q.x0 - q.x1) / 2) < 2.2 * size for q in (fr, last))
            ruled = any(last.y1 - 1 <= q.y0 <= r.y0 + 1 and min(q.x1, r.x1) - max(q.x0, r.x0) > 2
                        for q in rules)
            same_size = abs(line_size(o) - line_size(f["lines"][-1])) <= 0.15 * size
            usual = f["gaps"][len(f["gaps"]) // 2] if f["gaps"] else None
            close = (gap <= max(usual, 0) + max(1.5, 0.25 * size) if usual is not None
                     else gap <= 0.65 * size)  # (spaced like the frame's lines - or, the first
            # two, like lines of text: up to an airy design's spacing, not a form's rows)
            if (-0.3 * h < gap and close and same_size and over >= 0.4 * min(r.width, fr.width)
                    and lined_up and not ruled):
                if best is None or gap < best[0]:
                    best = (gap, f)
        if best:
            f = best[1]
            f["gaps"] = sorted(f["gaps"] + [best[0]])
            f["lines"].append(o)
            f["rect"] = f["rect"] | r
        else:
            frames.append({"lines": [o], "rect": pymupdf.Rect(r), "gaps": []})
    for f in frames:
        f["paras"] = split_paragraphs(f["lines"], f["rect"])
    return frames


def frame_objects(lines, mid, paras=None, edges=(), page_rect=None):
    """Lines that make a frame, as the Edit tool's objects: one frame - or, a line on its own
    or right-to-left text, line by line."""
    if any(RTL_LETTERS.search(o["text"]) for o in lines):  # (right-to-left: line by line)
        for o in lines:
            o["lines"], o["paras"] = [o], [[o]]
        return list(lines)
    rect = pymupdf.Rect(lines[0]["rect"])
    for o in lines[1:]:
        rect |= o["rect"]
    paras = paras or split_paragraphs(lines, rect)
    obj = {"kind": "text", "frame": True, "lines": lines, "paras": paras, "rect": rect,
           "spans": [s for o in lines for s in o["spans"]],
           "text": "\n".join(" ".join(o["text"] for o in para) for para in paras),
           "peers": [o["rect"] for o in lines], "page_mid": mid,
           "column": (rect.x0, rect.x1), "edges": edges, "page_rect": page_rect}
    cell = find_cell(rect, edges, page_rect) if edges else None
    if cell is not None:  # (in a table's cell: wraps in it, lined up in it)
        obj["cell"] = cell
    return [obj]


def split_paragraphs(ls, rect):
    """A frame's lines (top to bottom) into paragraphs - see group_frames."""
    x0, x1 = rect.x0, rect.x1
    gaps = [b["rect"].y0 - a["rect"].y1 for a, b in zip(ls, ls[1:])]
    normal = sorted(gaps)[len(gaps) // 2] if gaps else 0
    mid = (x0 + x1) / 2
    # (centred / right-aligned lines: one starting further in is how they look)
    ragged = len(ls) > 1 and any(o["rect"].x0 > x0 + line_size(o) for o in ls) and (
        all(abs((o["rect"].x0 + o["rect"].x1) / 2 - mid) < 0.6 * line_size(o) for o in ls)
        or all(abs(o["rect"].x1 - x1) < 0.6 * line_size(o) for o in ls))
    paras = [[ls[0]]]
    for a, b, g in zip(ls, ls[1:], gaps):
        sa, sb = line_size(a), line_size(b)
        fits = a["rect"].width + 0.3 * sb + first_word_width(b) <= (x1 - x0) + 0.5
        new = (g > normal + 0.35 * max(sa, sb)  # (a bigger gap)
               or abs(sa - sb) > 0.15 * max(sa, sb)  # (another size)
               or fits  # (ended, though the next word would have fitted)
               or span_bold(b["spans"][0]) and not span_bold(a["spans"][-1])  # (a bold label
               # starting it - "University: ..." - after a line that didn't end bold)
               or not ragged and b["rect"].x0 > x0 + 1.2 * sb and a["rect"].x0 <= x0 + 1.2 * sa)
        if new:
            paras.append([b])
        else:
            paras[-1].append(b)
    return paras


def span_bold(span):
    return bool(span.get("flags", 0) & 16) or "bold" in span.get("font", "").lower()


def main_span(obj):
    """The span most of a line's text is in (its font, size and colour stand for the line)."""
    return max(obj["spans"], key=lambda s: len(s["text"].strip()))


def span_color(span):
    c = span["color"]
    return "#%02X%02X%02X" % (c >> 16 & 255, c >> 8 & 255, c & 255)


# Text Master PDF writes is labelled in the page's drawing with the piece of text (a frame,
# a line) it belongs to - so that piece's letters can be taken out exactly, never the
# letters of another piece it was moved over or that was moved over it.
TEXT_TAG = {"tag": None, "mode": {}, "boxes": []}  # (the piece being written now, if any)
TAGGED = re.compile(rb"/MasterPDF\s*<<\s*/T\s+(\d+)\s*>>\s*BDC.*?EMC", re.S)


@contextlib.contextmanager
def text_of(tag):
    """While in it, text written belongs to the piece tag (None: to nothing in particular);
    yields the boxes it's written in."""
    old = dict(TEXT_TAG)
    TEXT_TAG.update(tag=tag, mode={}, boxes=[])
    try:
        yield TEXT_TAG["boxes"]
    finally:
        TEXT_TAG.update(old)


def label_drawn(page, before, tag):
    """(PDF_LOCK held) What was drawn on the page since before ({content stream: its length}
    then) labelled as piece tag's."""
    doc = page.parent
    for x in page.get_contents():
        data = doc.xref_stream(x)
        n = before.get(x, 0)
        if len(data) > n:
            doc.update_stream(x, data[:n] + b"\n/MasterPDF <</T %d>> BDC\n" % tag + data[n:]
                              + b"\nEMC\n")


def frame_snapshot(doc, i, obj, others):
    """(PDF_LOCK held) A frame's text exactly as page i draws it - its own fonts, letters and
    places - as a one-page PDF with nothing else on it, and the box round it: to draw it back
    exactly as it was (see restore_snapshot). others: the page's other pieces of text."""
    tmp = pymupdf.open()
    tmp.insert_pdf(doc, from_page=i, to_page=i, annots=False)
    tp = tmp[0]
    tp.add_redact_annot(tp.rect, fill=False, cross_out=False)  # (pictures and drawings out)
    tp.apply_redactions(images=pymupdf.PDF_REDACT_IMAGE_REMOVE,
                        graphics=pymupdf.PDF_REDACT_LINE_ART_REMOVE_IF_TOUCHED,
                        text=pymupdf.PDF_REDACT_TEXT_NONE)
    mine = obj.get("oid")
    tags = {int(m.group(1)) for x in tp.get_contents() for m in TAGGED.finditer(tmp.xref_stream(x))}
    for tag in tags:
        if tag != mine:  # (other pieces Master PDF wrote: out, exactly)
            take_tagged(tp, tag)
    with text_of(None):
        for o in others:  # (the PDF's own other text: out, by place - never by the place of
            if o.get("oid") in tags:  # a piece Master PDF wrote, that may be over this one)
                continue
            for line in o.get("lines") or [o]:
                remove_text(tp, line["rect"])
    clip = pymupdf.Rect(obj["rect"]) + (-2, -3, 2, 3)
    return tmp.tobytes(garbage=3, deflate=True), clip


def restore_snapshot(page, data, clip):
    """(PDF_LOCK held) A frame drawn back exactly as it was (see frame_snapshot) - labelled as
    the piece being written now (TEXT_TAG)."""
    src = pymupdf.open("pdf", data)
    doc = page.parent
    page.wrap_contents()
    before = {x: len(doc.xref_stream(x)) for x in page.get_contents()}
    page.show_pdf_page(clip, src, 0, clip=clip)
    if TEXT_TAG["tag"] is not None:
        label_drawn(page, before, TEXT_TAG["tag"])
        TEXT_TAG["boxes"].append(pymupdf.Rect(clip))


def draw_snapshot(page, data, clip, dx, dy):
    """(PDF_LOCK held) A frame drawn exactly as it was (see frame_snapshot), moved by
    (dx, dy) - labelled as the piece being written now (TEXT_TAG): a copy of it, pasted."""
    src = pymupdf.open("pdf", data)
    doc = page.parent
    page.wrap_contents()
    before = {x: len(doc.xref_stream(x)) for x in page.get_contents()}
    dest = clip + (dx, dy, dx, dy)
    page.show_pdf_page(dest, src, 0, clip=clip)
    if TEXT_TAG["tag"] is not None:
        label_drawn(page, before, TEXT_TAG["tag"])
        TEXT_TAG["boxes"].append(pymupdf.Rect(dest))


def take_tagged(page, tag=None):
    """(PDF_LOCK held) The labelled text of piece tag (None: of every piece) taken out of the
    page's drawing - returned, as it was drawn."""
    doc = page.parent
    out = []
    for xref in page.get_contents():
        data = doc.xref_stream(xref)
        if b"/MasterPDF" not in data:
            continue

        def cut(m):
            if tag is None or int(m.group(1)) == tag:
                out.append(b"\n" + m.group(0) + b"\n")
                return b" "
            return m.group(0)
        new = TAGGED.sub(cut, data)
        if new != data:
            doc.update_stream(xref, new)
    return out


def page_resources(page):
    """(PDF_LOCK held) The page's fonts and graphics states: {(kind, name): reference}."""
    doc = page.parent
    out = {}
    for kind in ("Font", "ExtGState", "XObject"):
        what, value = doc.xref_get_key(page.xref, "Resources/" + kind)
        if what == "xref":
            value = doc.xref_object(int(value.split()[0]))
        elif what != "dict":
            continue
        for name, ref in re.findall(r"/([^\s/<>\[\]()]+)\s+(\d+ 0 R)", value):
            out[(kind, name)] = ref
    return out


def remove_text(page, rect):
    """Take the letters inside rect out of the page - only letters: pictures and drawings
    stay. Just the middle of the line is used, so the lines above and below aren't touched.
    While a piece of text Master PDF wrote is being changed (see text_of), it's that piece's
    own letters that go - all of them, exactly; and taking out the PDF's own letters never
    touches what Master PDF wrote over them."""
    tag = TEXT_TAG["tag"]
    if tag is not None:  # (the first time on this page tells which: its own labelled text
        mode = TEXT_TAG["mode"]  # there - taken out at once; or the PDF's own - by place)
        if page.xref not in mode:
            mode[page.xref] = bool(take_tagged(page, tag))
        if mode[page.xref]:
            return
    r = pymupdf.Rect(rect)
    r.y0, r.y1 = r.y0 + r.height * 0.3, r.y1 - r.height * 0.3
    kept = take_tagged(page)  # (Master PDF's text: out of the way meanwhile)
    fonts = page_resources(page) if kept else {}
    page.add_redact_annot(r, fill=False, cross_out=False)
    page.apply_redactions(images=pymupdf.PDF_REDACT_IMAGE_NONE,
                          graphics=pymupdf.PDF_REDACT_LINE_ART_NONE,
                          text=pymupdf.PDF_REDACT_TEXT_REMOVE)
    if kept:
        doc = page.parent
        have = page_resources(page)
        for (kind, name), ref in fonts.items():  # (its fonts: still there for it)
            if (kind, name) not in have:
                try:
                    doc.xref_set_key(page.xref, f"Resources/{kind}/{name}", ref)
                except Exception:
                    pass
        page.wrap_contents()
        last = page.get_contents()[-1]
        doc.update_stream(last, doc.xref_stream(last) + b"".join(kept))


def write_text(page, origin, text, span, size=None, color=None, right=None, family=None,
               rotate=0, bold=None, italic=None, direction=None, measure=False):
    """Write text on the page with its baseline starting at origin, in the font of span (or
    the closest one that has all its letters), its size and colour unless given. (right:
    where the line ended - right-to-left text ends there. family: another font, in the
    span's weight and slant. rotate: the page's turn, so the text is upright as shown.
    bold, italic: True / False to make it bold / italic or not; None keeps the span's.
    direction: "rtl" / "ltr" - written right-to-left or not; None: as its first letter is.
    measure: nothing written - only how wide it would be.) Returns (how wide, the font's
    ascender and descender per point)."""
    size = size or span["size"]
    rgb = hex_rgb(color or span_color(span))
    wanted = family
    path, family, was_bold, was_italic = find_font(span["font"], span["flags"])
    if wanted:  # (another font: bold / italic go with it if they were chosen - not the old
        # font's own: Lucida Handwriting is only ever italic, a line changed from it isn't)
        own_bold, own_italic = chosen_style(family, was_bold, was_italic)
        bold = own_bold if bold is None else bold
        italic = own_italic if italic is None else italic
    bold = was_bold if bold is None else bold
    italic = was_italic if italic is None else italic
    if wanted or (bold, italic) != (was_bold, was_italic):  # another font, or weight / slant
        path, family = find_family(wanted or family, bold, italic)
    needs = [c for c in text if not c.isspace()]
    if path and not all(loaded_font(path).has_glyph(ord(c)) for c in needs):
        path = None  # (this font hasn't got some of the letters)
    if path is None:  # Arial has most of the world's letters
        arial = find_family("Arial", bold, italic)[0]
        if arial and all(loaded_font(arial).has_glyph(ord(c)) for c in needs):
            path, family = arial, "Arial"
    rb, ri = real_style(path) if path else (bold, italic)
    fake_bold, fake_italic = bold and not rb, italic and not ri  # (the family has none)
    if path:
        f = loaded_font(path)
        metrics = (f.text_length(text, size), f.ascender, f.descender)
    else:
        metrics = (pymupdf.get_text_length(text, fontsize=size), 0.9, -0.2)
    html_line = path and (RTL_LETTERS.search(text) or direction == "rtl")
    if measure and not html_line:
        return metrics
    if html_line:  # right-to-left letters (Arabic...) must be joined
        # up and run the other way: laid out as a line of HTML, which does that
        folder, file = os.path.split(path)
        first = next((c for c in text if c.isalpha()), "")
        rtl = direction == "rtl" if direction else bool(RTL_LETTERS.match(first))  # (the
        # line starts right-to-left: it ends at
        width = size * len(text) * 0.8 + 20  # the right, where the old line ended)
        x0 = (right or origin.x + width) - width if rtl else origin.x
        css = (f"@font-face {{font-family: f; src: url({file});}} "
               f"* {{font-family: f; font-size: {size}px; color: {color or span_color(span)}; "
               f"margin: 0; padding: 0; white-space: nowrap; text-align: left;"
               f"{' font-weight: bold;' if fake_bold else ''}"
               f"{' font-style: italic;' if fake_italic else ''}}}")
        html = f'<div dir="{"rtl" if rtl else "ltr"}">{html_escape(text)}</div>'
        # (for right-to-left text "left" means its start: the right edge.) Where the line's
        # baseline falls in the box is measured on a scratch page, so it sits on the old one
        scratch = pymupdf.open()
        try:
            sp = scratch.new_page(width=width + 40, height=size * 4)
            sp.insert_htmlbox(pymupdf.Rect(0, 0, width, size * 3), html, css=css,
                              archive=pymupdf.Archive(folder))
            spans = [s for b in sp.get_text("dict")["blocks"] for line in b.get("lines", [])
                     for s in line["spans"]]
            drop = spans[0]["origin"][1] if spans else size
            if spans:  # (how wide it really is, its letters joined: not as one by one)
                metrics = (max(s["bbox"][2] for s in spans) - min(s["bbox"][0] for s in spans),
                           metrics[1], metrics[2])
        finally:
            scratch.close()
        if measure:
            return metrics
        top = origin.y - drop
        page.insert_htmlbox(pymupdf.Rect(x0, top, x0 + width, top + size * 3), html, css=css,
                            archive=pymupdf.Archive(folder))
        return metrics
    extra = {}
    if path:
        style = ("Bold" if fake_bold else "") + ("Italic" if fake_italic else "")
        name = ("MPF" + re.sub(r"[^A-Za-z0-9]", "", os.path.splitext(os.path.basename(path))[0])
                + ("X" + style if style else ""))
        if style:  # (the file and a few bytes more: PDF keeps it apart from the plain one)
            pad = {"Bold": 1, "Italic": 2, "BoldItalic": 3}[style]
            if (path, pad) not in _FONTS["fonts"]:
                with open(path, "rb") as f:
                    _FONTS["fonts"][path, pad] = f.read() + b"\0" * pad
            xref = page.insert_font(fontname=name, fontbuffer=_FONTS["fonts"][path, pad])
        else:
            xref = page.insert_font(fontname=name, fontfile=path)
        if style:  # made bold / slanted here: named for the whole style, so it's known for
            doc = page.parent  # what it is later (PDF readers cut names at 31 letters)
            whole = ("Bold" if bold else "") + ("Italic" if italic else "")
            base = re.sub(r"[^A-Za-z0-9]", "", font_key(family).title())
            base = "/" + base + "-" + (whole if len(base) + len(whole) < 31
                                       else whole.replace("old", "").replace("talic", ""))
            for x in [xref] + [int(v) for v in re.findall(r"(\d+) 0 R",
                                                          doc.xref_get_key(xref, "DescendantFonts")[1])]:
                doc.xref_set_key(x, "BaseFont", base)
        if fake_bold:  # thickened: the letters' outlines drawn round them too
            extra.update(render_mode=2, fill=rgb, border_width=0.035)
        if fake_italic:  # slanted, like the italic it hasn't got (upright as shown)
            r = pymupdf.Matrix(page.rotation_matrix)
            r.e = r.f = 0
            extra["morph"] = (origin, r * pymupdf.Matrix(1, 0, 0.2, 1, 0, 0) * ~r)
    else:  # no font files at all: PDF's own Helvetica / Times / Courier
        kind = "co" if family.startswith("Courier") else "ti" if family.startswith("Times") else "he"
        name = BASE_FONTS[kind][bold * 2 + italic]
    tag = TEXT_TAG["tag"]
    doc = page.parent
    if tag is not None:
        before = {x: len(doc.xref_stream(x)) for x in page.get_contents()}
    page.insert_text(origin, text, fontname=name, fontsize=size, color=rgb, rotate=rotate,
                     **extra)
    if tag is not None:  # (labelled with the piece it belongs to)
        label_drawn(page, before, tag)
        w = metrics[0] if metrics else size * len(text)
        TEXT_TAG["boxes"].append(pymupdf.Rect(origin.x - w, origin.y - size * 1.2, origin.x + w,
                                              origin.y + size * 0.5))
    if path:
        keep_case(page.parent, xref, path, text)
    return metrics  # (how wide, and the font's ascender / descender per point)


CASE_MARK = "% Master PDF: letters as typed"


def keep_case(doc, xref, path, text):
    """(PDF_LOCK held) A capitals-only font (Algerian, Castellar...) draws "F" and "f" with
    the same shape, and the PDF then reads that shape back as "f" - so a line changed to it
    and back would lose its capitals. The font's letter map is told the letters as typed
    (a capital where both were typed); the Master PDF part is replaced each time."""
    font = loaded_font(path)
    want = {}
    for c in text:
        if c.isalpha() and len(c.upper()) == len(c.lower()) == 1 and c.lower() != c.upper():
            g = font.has_glyph(ord(c.lower()))
            if g and g == font.has_glyph(ord(c.upper())):
                want[g] = c.upper() if want.get(g, c) != c else c
    if not want:
        return
    kind, value = doc.xref_get_key(xref, "ToUnicode")
    if kind != "xref":
        return
    tu = int(value.split()[0])
    cmap = doc.xref_stream(tu).decode("latin-1")
    old = {}
    m = re.search(re.escape(CASE_MARK) + r"\n\d+ beginbfchar\n(.*?)endbfchar\n", cmap, re.S)
    if m:  # (what earlier lines told it stays, unless this line says otherwise)
        old = {int(a, 16): chr(int(b, 16)) for a, b in re.findall(r"<([0-9a-f]+)> <([0-9a-f]+)>", m[1])}
        cmap = cmap.replace(m[0], "")
    old.update(want)
    block = "".join(f"<{g:04x}> <{ord(c):04x}>\n" for g, c in sorted(old.items()))
    cmap = cmap.replace("endcmap", f"{CASE_MARK}\n{len(old)} beginbfchar\n{block}endbfchar\nendcmap", 1)
    doc.update_stream(tu, cmap.encode("latin-1"))


def find_family(name, bold=False, italic=False):
    """(file, family) of the installed font of family name nearest bold / italic (its own
    bold or italic when it has one) - or, for a name that isn't a family, the closest font."""
    key = font_key(name)
    found = family_font(key, bold, italic)
    if found:
        return found
    path, family, _b, _i = find_font(name + ("Bold" if bold else "") + ("Italic" if italic else ""),
                                     (16 if bold else 0) | (2 if italic else 0))
    return path, family


def font_families():
    """Every font family installed, A to Z (once the fonts have been looked up)."""
    return sorted({m[0][1] for m in (_FONTS["families"] or {}).values()}, key=str.lower)


def write_new_text(page, top, text, family, size, color, bold=False, italic=False,
                   align="left", direction=None):
    """New text on the page (Add text): its first line's top at top (in the page's own
    space), a line every 1.2 sizes down, upright on the page as it's shown - lined up at
    the left, centre or right of the widest line, or justified (each line but the last as
    wide as the widest). Returns each line's box (in the page's own space)."""
    path, family = find_family(family, bold, italic)
    font = loaded_font(path) if path else None
    ascent, descent = (font.ascender, font.descender) if font else (0.9, -0.2)

    span = {"font": family, "flags": 0, "size": size, "color": int(color[1:], 16)}

    def width(s):  # (as it really comes out: right-to-left letters joined up)
        if direction == "rtl" or RTL_LETTERS.search(s):
            return write_text(page, pymupdf.Point(0, 0), s, span, bold=bold, italic=italic,
                              direction=direction, measure=True)[0]
        return font.text_length(s, size) if font else pymupdf.get_text_length(s, fontsize=size)
    corner = top * page.rotation_matrix  # (laid out on the page as it's shown)
    lines = text.split("\n")
    widths = [width(line) for line in lines]
    full = max(widths, default=0)
    last = max((n for n, line in enumerate(lines) if line.strip()), default=0)
    boxes = []
    for n, line in enumerate(lines):
        if not line.strip():
            continue
        y = corner.y + ascent * size + n * size * 1.2
        words = line.split()
        stretch = (align == "justify" and n < last and len(words) > 1 and direction != "rtl"
                   and justify_gap(full, widths[n], line.count(" "), width(" "), size))
        if stretch:  # the gaps share the room left (not so wide it'd stop reading as a line)
            gap = (full - sum(width(w) for w in words)) / (len(words) - 1)
            x, x0, w = corner.x, corner.x, full
            for word in words:
                write_text(page, pymupdf.Point(x, y) * page.derotation_matrix, word, span,
                           rotate=page.rotation, bold=bold, italic=italic)
                x += width(word) + gap
        else:
            w = widths[n]
            how = ("right" if direction == "rtl" else "left") if align == "justify" else align
            x0 = corner.x + {"center": (full - w) / 2, "right": full - w}.get(how, 0)
            write_text(page, pymupdf.Point(x0, y) * page.derotation_matrix, line, span,
                       rotate=page.rotation, bold=bold, italic=italic, direction=direction,
                       right=x0 + w)
        boxes.append(pymupdf.Rect(x0, y - ascent * size, x0 + w, y - descent * size)
                     * page.derotation_matrix)
    return boxes


def html_escape(text):
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def line_align(obj):
    """How a line of the PDF is lined up: "left", "center", "right" or "justify" - told by
    the other lines of its paragraph (a line on its own: centred if it's in the middle of
    the page)."""
    if obj.get("frame"):  # (a frame: by its paragraphs - lines running to both edges, but
        x0, x1 = obj["rect"].x0, obj["rect"].x1  # their last: justified)
        mid = (x0 + x1) / 2
        lines = obj["lines"]
        cell = obj.get("cell")
        if cell is not None:  # (in a table's cell: lined up in the cell)
            cm = (cell.x0 + cell.x1) / 2
            size = max(line_size(o) for o in lines)
            if all(abs((o["rect"].x0 + o["rect"].x1) / 2 - cm) < 0.6 * line_size(o)
                   for o in lines) and x0 - cell.x0 > 0.5 * size:
                return "center"
            if cell.x1 - x1 < size < x0 - cell.x0 and all(abs(o["rect"].x1 - x1) < 2.5 for o in lines):
                return "right"
            inner = cell.x1 - min(x0 - cell.x0, size)  # (where justified lines would end)
            if not lines[:-1] or not all(abs(o["rect"].x1 - inner) < 2.5 for o in lines[:-1]):
                return "left"  # (its lines stop short of the cell's side: not justified)
        if len(lines) > 1 and any(o["rect"].x0 > x0 + line_size(o) for o in lines) and all(
                abs((o["rect"].x0 + o["rect"].x1) / 2 - mid) < 0.6 * line_size(o) for o in lines):
            return "center"
        long = [para for para in obj["paras"] if len(para) > 1]
        full = [para for para in long if all(abs(o["rect"].x1 - x1) < 2.5 for o in para[:-1])
                and abs(para[-1]["rect"].x0 - x0) < 2.5 + 0.5 * line_size(para[-1])]
        if long and len(full) * 2 >= len(long):
            return "justify"
    r, peers = obj["rect"], obj.get("peers") or [obj["rect"]]
    x0, x1 = min(p.x0 for p in peers), max(p.x1 for p in peers)
    near = 2.5

    def full(p):
        return abs(p.x0 - x0) < near and abs(p.x1 - x1) < near
    if len(peers) > 1:
        if len(peers) >= 3 and all(full(p) for p in peers[:-1]) and abs(peers[-1].x0 - x0) < near:
            return "justify"
        short = [p for p in peers if not full(p)] or peers
        votes = {"left": sum(abs(p.x0 - x0) < near for p in short),
                 "center": sum(abs((p.x0 + p.x1 - x0 - x1) / 2) < near for p in short),
                 "right": sum(abs(p.x1 - x1) < near for p in short)}
        best = max(votes, key=lambda k: (votes[k], k == "left"))
        return best if votes[best] else "left"
    if abs((r.x0 + r.x1) / 2 - obj.get("page_mid", -99)) < 3:
        return "center"
    return "left"


def align_ref(obj, align=None):
    """(how to line up, (left, right) to line up between) for retyping a line: as it is
    lined up now, keeping its own edges (an indent stays) - or, chosen, in its paragraph."""
    r = obj["rect"]
    if align is None:
        return line_align(obj), (r.x0, r.x1)
    peers = obj.get("peers") or [r]
    x0, x1 = min(p.x0 for p in peers), max(p.x1 for p in peers)
    if "column" in obj and len(peers) == 1:  # (a paragraph of its own: the lines round it)
        x0, x1 = min(x0, obj["column"][0]), max(x1, obj["column"][1])
    return align, (x0, x1)


def justify_gap(room, width, spaces, space, size):
    """How much wider (or narrower: down to half) each space gets so a line width wide
    fills room - 0 if it can't (no spaces, or gaps so wide it'd stop reading as a line)."""
    if not spaces:
        return 0
    gap = (room - width) / spaces
    if gap > 0.9 * size:
        return 0
    return max(gap, -0.5 * space)


def line_start(ref, align, width):
    """Where a line width wide starts, lined up between ref's left and right."""
    x0, x1 = ref
    return {"right": x1 - width, "center": (x0 + x1 - width) / 2}.get(align, x0)


def retype_line(page, obj, text, size=None, color=None, family=None, bold=None,
                italic=None, align="left", ref=None, direction=None):
    """Put new text (or a new size / colour / font / weight / slant) in place of a line,
    lined up between ref (its own edges unless given) - its left end, middle or right end
    staying there. direction: "rtl" / "ltr" to write it right-to-left or not."""
    origin = pymupdf.Point(obj["spans"][0]["origin"])
    ref = ref or (obj["rect"].x0, obj["rect"].x1)
    span = main_span(obj)
    kw = dict(family=family, bold=bold, italic=italic, direction=direction)
    width = write_text(page, origin, text, span, size, color, measure=True, **kw)[0]
    gap = 0
    rtl = direction == "rtl" or bool(RTL_LETTERS.search(text))
    if align == "justify" and not rtl:
        space = write_text(page, origin, " ", span, size, color, measure=True, **kw)[0]
        gap = justify_gap(ref[1] - ref[0], width, text.count(" "), space, size or span["size"])
    x = line_start(ref, ("right" if rtl else "left") if align == "justify" else align, width)
    remove_text(page, obj["rect"])
    if not text.strip():
        return
    if gap:  # (word by word - each with its space, so the text still reads with spaces -
        # each space a little wider, or narrower)
        for piece in re.findall(r"\S+ *| +", text):
            x += write_text(page, pymupdf.Point(x, origin.y), piece, span, size, color,
                            **kw)[0] + gap * piece.count(" ")
    else:
        write_text(page, pymupdf.Point(x, origin.y), text, span, size, color, right=x + width,
                   **kw)


def scale_line(page, obj, k, right=False):
    """Make a text line k times as big, on the same baseline, its left end staying where
    it is (right: its right end). Its pieces keep their own fonts and colours."""
    ax = obj["rect"].x1 if right else obj["rect"].x0
    remove_text(page, obj["rect"])
    for span in obj["spans"]:
        o = pymupdf.Point(span["origin"])
        write_text(page, pymupdf.Point(ax + (o.x - ax) * k, o.y), span["text"], span,
                   size=span["size"] * k)


# a letter's look in the Edit tool's box: (family, size, bold, italic, colour, underline,
# strikethrough, highlight colour or None)
STYLE_FIELDS = ("family", "size", "bold", "italic", "color", "underline", "strike", "highlight")


def seg_distance(p, a, b):
    """How far point p is from the segment a-b."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    length = dx * dx + dy * dy
    k = 0.0 if not length else max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / length))
    x, y = a[0] + k * dx - p[0], a[1] + k * dy - p[1]
    return (x * x + y * y) ** 0.5


RUBBABLE = ("Ink", "Line", "Square", "Circle", "PolyLine", "Polygon")  # (what the eraser takes)


def shape_outline(annot, kind, box, w):
    """A shape's outline as points (along the middle of its line)."""
    if kind == "Square":
        q = box + (w / 2, w / 2, -w / 2, -w / 2)
        return [tuple(q.tl), tuple(q.tr), tuple(q.br), tuple(q.bl), tuple(q.tl)]
    if kind == "Circle":
        q = box + (w / 2, w / 2, -w / 2, -w / 2)
        return [(q.x0 + q.width / 2 * (1 + math.cos(k * math.pi / 36)),
                 q.y0 + q.height / 2 * (1 + math.sin(k * math.pi / 36))) for k in range(73)]
    pts = [tuple(v) for v in annot.vertices or []]
    if kind == "Polygon" and pts:
        pts.append(pts[0])
    return pts


def rub_out(page, path, r):
    """(PDF_LOCK held) The eraser rubbed along path (points, in the page's own space), r
    across - like Photoshop's: what's under it goes, the rest stays. Pen and marker strokes,
    and shapes drawn as an outline, lose just the parts under it, cut at its edge (a stroke
    rubbed through the middle becomes two); a filled shape or an arrow it touches goes. True
    if anything changed."""
    segs = list(zip(path, path[1:])) or [(path[0], path[0])]
    xs, ys = [p[0] for p in path], [p[1] for p in path]
    reach = pymupdf.Rect(min(xs) - r, min(ys) - r, max(xs) + r, max(ys) + r)

    def under(p, w):
        return any(seg_distance(p, a, b) <= r + w / 2 for a, b in segs)
    changed = False
    for annot in list(page.annots()):
        kind = annot.type[1]
        if kind not in RUBBABLE:
            continue
        box = pymupdf.Rect(annot.rect)
        if not box.intersects(reach):
            continue
        w = (annot.border or {}).get("width") or 1
        near = reach + (-w, -w, w, w)

        def gone(q):
            return near.contains(q) and under(q, w)

        def edge(keep, cut):  # (where the eraser's edge crosses keep-cut)
            for _ in range(10):
                m = ((keep[0] + cut[0]) / 2, (keep[1] + cut[1]) / 2)
                if gone(m):
                    cut = m
                else:
                    keep = m
            return keep
        ends = annot.line_ends if kind in ("Line", "PolyLine") else None
        if kind == "Ink":
            strokes = [[tuple(v) for v in s] for s in annot.vertices or []]
        elif not annot.colors.get("fill") and not any(ends or ()):
            strokes = [shape_outline(annot, kind, box, w)]
        else:  # (filled, or an arrow: touched anywhere, it goes)
            outline = shape_outline(annot, kind, box, w)
            line = list(zip(outline, outline[1:])) or [(outline[0], outline[0])] if outline else []
            if any(seg_distance(p, a, b) <= r + w / 2 for a, b in line for p in path) or \
                    any(under(a, w) for a, _ in line):
                page.delete_annot(annot)
                changed = True
            continue
        step = max(0.25, min(1.0, r / 4))
        pieces, cut = [], False
        for stroke in strokes:
            pts = []  # (the stroke with points every so often - only its own kept, and the cuts)
            for a, b in zip(stroke, stroke[1:]):
                n = max(1, int(((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2) ** 0.5 / step))
                pts.append((a, True))
                pts += [((a[0] + (b[0] - a[0]) * k / n, a[1] + (b[1] - a[1]) * k / n), False)
                        for k in range(1, n)]
            if stroke:
                pts.append((stroke[-1], True))
            run, prev, was = [], None, False
            for q, own in pts:
                now = gone(q)
                if now:
                    cut = True
                    if prev is not None and not was:
                        run.append(edge(prev, q))
                    if len(run) > 1:
                        pieces.append(run)
                    run = []
                else:
                    if prev is not None and was:
                        run.append(edge(q, prev))
                    if own or not run:
                        run.append(q)
                prev, was = q, now
            if prev is not None and not was and run and run[-1] != prev:
                run.append(prev)
            if len(run) > 1:
                pieces.append(run)
        if not cut:
            continue
        stroke_color, opacity = annot.colors.get("stroke"), annot.opacity
        blend, info = annot.blendmode, annot.info
        page.delete_annot(annot)
        changed = True
        if pieces:  # (what's left: the same colour, width and see-through)
            new = page.add_ink_annot(pieces)
            if stroke_color:
                new.set_colors(stroke=stroke_color)
            new.set_border(width=w)
            if 0 <= opacity < 1:
                new.set_opacity(opacity)
            if blend:
                new.set_blendmode(blend)
            new.set_info(content=info.get("content", ""), title=info.get("title", ""),
                         subject=info.get("subject", ""))
            new.update()
    return changed


def retype_runs(page, obj, runs, align="left", ref=None):
    """Put pieces of text, each in its own style, in place of a line - one after the
    other along its baseline, lined up between ref (its own edges unless given): left,
    centred, right, or justified (the spaces widened to fill it). Returns each piece's
    box and style (for its marks)."""
    origin = pymupdf.Point(obj["spans"][0]["origin"])
    y, span = origin.y, main_span(obj)
    ref = ref or (obj["rect"].x0, obj["rect"].x1)

    def write(x, text, st, measure=False):
        family, size, bold, italic, color = st[:5]
        return write_text(page, pymupdf.Point(x, y), text, span, size, color, family=family,
                          bold=bold, italic=italic, measure=measure)
    metrics = [write(0, text, st, True) for text, st in runs]
    total = sum(m[0] for m in metrics)
    spaces = sum(text.count(" ") for text, _ in runs)
    first = runs[0][1] if runs else None
    space = write(0, " ", first, True)[0] if first else 0
    gap = justify_gap(ref[1] - ref[0], total, spaces, space, first[1]) \
        if align == "justify" and first else 0
    x = line_start(ref, "left" if align == "justify" else align, total)
    remove_text(page, obj["rect"])
    out = []
    for (text, st), (w, asc, desc) in zip(runs, metrics):
        start, size = x, st[1]
        if gap:  # (word by word, each space a little wider)
            for piece in re.findall(r"\S+ *| +", text):  # (each word with its space)
                x += write(x, piece, st)[0] + gap * piece.count(" ")
        else:
            x += write(x, text, st)[0]
        out.append((pymupdf.Rect(start, y - asc * size, x, y - desc * size), st))
    return out


def frame_geometry(frame):
    """A frame's spacing, as it is: (its text's size, the distance between baselines in a
    paragraph, the baseline-to-baseline distance between each paragraph and the next, each
    paragraph's first-line indent)."""
    sizes = sorted(line_size(o) for o in frame["lines"])
    base = sizes[len(sizes) // 2]
    pitches = [b["spans"][0]["origin"][1] - a["spans"][0]["origin"][1]
               for para in frame["paras"] for a, b in zip(para, para[1:])]
    leading = sorted(pitches)[len(pitches) // 2] if pitches else base * 1.2
    gaps = [b[0]["spans"][0]["origin"][1] - a[-1]["spans"][0]["origin"][1]
            for a, b in zip(frame["paras"], frame["paras"][1:])]
    x0 = frame["rect"].x0
    indents = [max(0.0, para[0]["rect"].x0 - x0) for para in frame["paras"]]
    return base, leading, gaps, indents


def layout_frame(page, frame, paras, x0, x1, align="left", wrap=True, start_y=None,
                 avoid=None, geometry=None):
    """(PDF_LOCK held) A frame's paragraphs (each a list of (text, look) pieces) written
    into x0 .. x1, word-wrapped, from its first line's baseline down - its own line spacing,
    gaps between paragraphs and first-line indents kept; lined up left, centred, right or
    justified (each line but a paragraph's last as wide as the frame). Returns each piece's
    box and look (for its underline / strike-out / highlight). start_y: its first baseline
    (else where it is); geometry: its spacing (else its own - see frame_geometry); avoid:
    avoid(top, bottom, x0, x1) -> the (left, right) a line there can go in - beside a picture
    text flows round - or None (none: the line goes lower, under it)."""
    base, leading, gaps, indents = geometry or frame_geometry(frame)
    span = main_span(frame)
    widths = {}

    def measure(text, st):
        key = (text, st[:4])
        if key not in widths:
            widths[key] = write_text(page, pymupdf.Point(0, 0), text, span, st[1], st[4],
                                     family=st[0], bold=st[2], italic=st[3], measure=True)
        return widths[key]
    y = frame["lines"][0]["spans"][0]["origin"][1] if start_y is None else start_y
    room_all = max(10.0, x1 - x0)
    placed = []
    for n, runs in enumerate(paras):
        if n:  # (the gap the paragraph had before it - or the frame's usual one)
            y += gaps[n - 1] if n - 1 < len(gaps) else (sorted(gaps)[len(gaps) // 2] if gaps
                                                        else leading * 1.5)
        words, cur = [], []  # (a word: its pieces - a word can change look half way)
        for text, st in runs:
            for piece in re.findall(r"\S+|\s+", text):
                if piece.isspace():
                    if cur:
                        cur[-1] = (cur[-1][0] + " ", cur[-1][1])
                        words.append(cur)
                        cur = []
                else:
                    cur.append((piece, st))
        if cur:
            words.append(cur)
        if not words:  # (an empty paragraph: just its gap)
            continue
        indent = indents[n] if n < len(indents) and align in ("left", "justify") else 0
        pending, k, biggest = list(words), 0, base
        while pending:  # (a line at a time: where it goes tells how wide it can be)
            if k:
                y += leading
            first = indent if k == 0 else 0
            lx0, lx1 = x0, x1
            if avoid is not None:  # (beside the pictures text flows round - or under them)
                for _ in range(2000):
                    free = avoid(y - 0.8 * base, y + 0.25 * base, x0, x1)
                    if free and free[1] - free[0] - first >= min(room_all - first, 4 * base):
                        lx0, lx1 = free
                        break
                    y += leading / 4
            room = max(10.0, lx1 - lx0) - first
            line, width = [], 0.0
            while pending:
                w = pending[0]
                ww = sum(measure(t.rstrip(), st)[0] for t, st in w)
                if wrap and line and width + ww > room + 0.01:  # (wrap: not a line on its own,
                    break  # left as wide as its text until its box is resized)
                line.append(pending.pop(0))
                width += ww + (measure(" ", w[-1][1])[0] if w[-1][0].endswith(" ") else 0)
            biggest = max(st[1] for w in line for _, st in w)
            if k and biggest > base:  # (bigger letters: the line further down)
                y += leading * (biggest / base - 1)
            total = sum(measure(t, st)[0] for w in line for t, st in w)
            trailing = line[-1][-1]
            if trailing[0].endswith(" "):
                total -= measure(" ", trailing[1])[0]
            spaces = sum(1 for w in line[:-1] if w[-1][0].endswith(" "))
            how = align
            gap = 0.0
            if align == "justify":
                if pending:
                    gap = justify_gap(room, total, spaces, measure(" ", line[0][0][1])[0], biggest)
                how = "left"
            x = lx0 + first + line_start((0, room), how, total)
            for wn, w in enumerate(line):
                for pn, (text, st) in enumerate(w):
                    last = wn == len(line) - 1 and pn == len(w) - 1
                    t2 = text.rstrip() if last else text  # (no space written at a line's end)
                    wd, asc, desc = write_text(page, pymupdf.Point(x, y), t2, span, st[1], st[4],
                                               family=st[0], bold=st[2], italic=st[3])
                    placed.append((pymupdf.Rect(x, y - asc * st[1], x + measure(t2.rstrip(), st)[0],
                                                y - desc * st[1]), st))
                    x += wd
                if w[-1][0].endswith(" ") and wn < len(line) - 1:
                    x += gap
            k += 1
    return placed


def put_marks(page, placed, see=1.0):
    """(PDF_LOCK held) Underline / strike-out / highlight over the pieces written that have
    them (placed: [(box, look)])."""
    for kind, field in (("Underline", 5), ("StrikeOut", 6), ("Highlight", 7)):
        groups = {}
        for box, st in placed:
            if st[field]:
                groups.setdefault(st[7] if kind == "Highlight" else st[4], []).append(box)
        for color, boxes in groups.items():
            add = {"Underline": page.add_underline_annot, "StrikeOut": page.add_strikeout_annot,
                   "Highlight": page.add_highlight_annot}[kind]
            annot = add(boxes)
            annot.set_colors(stroke=hex_rgb(color))
            if kind == "Highlight" and see < 1:
                annot.set_opacity(see)
            annot.update()


def written_rows(placed):
    """The lines written (placed: [(box, look)]): one box each."""
    rows = {}
    for box, _st in placed:
        key = round((box.y0 + box.y1) / 2)
        rows[key] = rows[key] | box if key in rows else pymupdf.Rect(box)
    return [r + (-0.5, 0, 0.5, 0) for r in rows.values()]


def runs_to_paras(runs):
    """[(text, look)] with "\n" between paragraphs -> a list of paragraphs, each its pieces."""
    paras = [[]]
    for text, st in runs:
        for k, part in enumerate(text.split("\n")):
            if k:
                paras.append([])
            if part:
                paras[-1].append((part, st))
    return paras


# a picture's layout, as Word's: in line with the text, text wrapped round it (square,
# tight, through, top and bottom), or behind / in front of the text
LAYOUTS = (("inline", "In Line with Text"), ("square", "Square"), ("tight", "Tight"),
           ("through", "Through"), ("top_bottom", "Top and Bottom"),
           ("behind", "Behind Text"), ("front", "In Front of Text"))
FLOWING = ("inline", "square", "tight", "through", "top_bottom")  # (text goes round these)
WRAP_SIDE, WRAP_TOP = 9.0, 4.0  # (how far text keeps from a picture: beside it, over / under)


def image_outline(doc, xref):
    """Where a picture with see-through parts is solid, row by row from its top: (left,
    right) as parts of its width - None for a solid picture (or one that can't be read)."""
    try:
        smask = doc.extract_image(xref).get("smask") or 0
        if not smask:
            return None
        pix = pymupdf.Pixmap(doc, smask)
        im = Image.frombytes("L" if pix.n == 1 else "RGB", (pix.width, pix.height),
                             pix.samples).convert("L")
    except Exception:
        return None
    rows = max(1, min(120, im.height))
    small = im.resize((max(1, min(200, im.width)), rows), Image.BOX)
    w = small.width
    out = []
    for y in range(rows):
        solid = [x for x in range(w) if small.getpixel((x, y)) > 40]
        out.append((solid[0] / w, (solid[-1] + 1) / w) if solid else None)
    return out


def wrap_band(obstacles):
    """avoid(top, bottom, x0, x1) for layout_frame: where a line from top to bottom can go
    between x0 and x1 with these pictures [(rect, layout, outline)] in the way - the widest
    room left beside them, or None (a picture text goes over and under: no room)."""
    def avoid(top, bottom, x0, x1):
        blocked = []
        for r, mode, outline in obstacles:
            if bottom <= r.y0 - WRAP_TOP or top >= r.y1 + WRAP_TOP or r.x1 <= x0 or r.x0 >= x1:
                continue
            if mode in ("inline", "top_bottom"):
                return None
            a, b = r.x0, r.x1
            if outline and mode in ("tight", "through"):  # (round where the picture is solid)
                n = len(outline)
                k0 = max(0, int((top - WRAP_TOP - r.y0) / max(1e-6, r.height) * n))
                k1 = min(n, int((bottom + WRAP_TOP - r.y0) / max(1e-6, r.height) * n) + 1)
                parts = [q for q in outline[k0:k1] if q]
                if not parts:
                    continue
                a = r.x0 + min(q[0] for q in parts) * r.width
                b = r.x0 + max(q[1] for q in parts) * r.width
            blocked.append((a - WRAP_SIDE, b + WRAP_SIDE))
        free, at = [], x0
        for a, b in sorted(blocked):
            if a > at:
                free.append((at, min(a, x1)))
            at = max(at, b)
        if at < x1:
            free.append((at, x1))
        free = [(a, b) for a, b in free if b > a]
        return max(free, key=lambda q: q[1] - q[0]) if free else None
    return avoid


def cut_marks(page, xrefs, holes):
    """(PDF_LOCK held) These marks taken off just the holes; what's beside them stays."""
    for xref in xrefs:
        annot = page.load_annot(xref)
        if annot is None:
            continue
        kind, stroke, opacity = annot.type[1], annot.colors.get("stroke"), annot.opacity
        info = annot.info
        pieces = [r for q in quad_rects(annot) for r in cut_out(q, holes)]
        page.delete_annot(annot)
        if not pieces:
            continue
        add = {"Highlight": page.add_highlight_annot, "StrikeOut": page.add_strikeout_annot,
               "Underline": page.add_underline_annot, "Squiggly": page.add_squiggly_annot}[kind]
        new = add(pieces)  # (what's left of it: the same kind, colour and see-through)
        if stroke:
            new.set_colors(stroke=stroke)
        if 0 <= opacity < 1:
            new.set_opacity(opacity)
        new.set_info(content=info.get("content", ""), title=info.get("title", ""),
                     subject=info.get("subject", ""))
        new.update()


def move_line(page, obj, d):
    """Move a text line by d (its pieces keep their own fonts, sizes and colours)."""
    remove_text(page, obj["rect"])
    for span in obj["spans"]:
        write_text(page, pymupdf.Point(span["origin"]) + d, span["text"], span)


def redraw_image(page, obj, change):
    """Change the instruction that draws a picture on the page: change(b"/Im1 Do") gives
    what goes in its place. The page's drawing instructions are tidied into one list first.
    The pictures' places were found in the order the page draws them, so the instruction
    is found by counting: obj["order"] of all obj["pictures"] pictures drawn on the page -
    which also works when the same picture is drawn at several places, or when the PDF
    holds identical copies of it (Word does that: they're all found as one)."""
    page.clean_contents()
    doc = page.parent

    def draws(names):  # [(content stream, where in it)] of each "/Name Do", in order
        if not names:
            return []
        pattern = rb"/(?:" + b"|".join(re.escape(n.encode()) for n in names) + rb")\s+Do\b"
        return [(xref, m.span()) for xref in page.get_contents()
                for m in re.finditer(pattern, doc.xref_stream(xref))]
    images = page.get_images(full=True)
    every = draws({item[7] for item in images})  # every picture the page draws itself
    if len(every) == obj.get("pictures", -1):
        xref, (a, b) = every[obj["order"]]
    else:  # (some are drawn inside a group: count just this picture's own instructions)
        mine = draws({item[7] for item in images if item[0] == obj["xref"]})
        if len(mine) != obj.get("uses", 1):
            raise ValueError("this picture is part of a group on the page, so it can't be "
                             "changed on its own")
        xref, (a, b) = mine[obj.get("nth", 0)]
    data = doc.xref_stream(xref)
    old = data[a:b]
    doc.update_stream(xref, data[:a] + change(old) + data[b:])
    return old


def place_image(page, obj, rect, behind=False):
    """Draw a picture at rect (in the page's own space) instead of where it is. It's taken
    out of where the page drew it and drawn again on top of the page: where it was, it may
    be clipped to a frame (Word clips a picture to its table cell), which would hide it
    once it's moved or made bigger."""
    old, new = obj["rect"], pymupdf.Rect(rect)
    move = (pymupdf.Matrix(1, 0, 0, 1, -old.x0, -old.y0)
            * pymupdf.Matrix(new.width / old.width, 0, 0, new.height / old.height, 0, 0)
            * pymupdf.Matrix(1, 0, 0, 1, new.x0, new.y0))
    # its place, from its own square to the page: MuPDF's (transform, moved) is upside down
    # to the picture's own square (flip) and in MuPDF's page space (back to PDF's with ~T)
    flip, T = pymupdf.Matrix(1, 0, 0, -1, 0, 1), page.transformation_matrix
    ctm = flip * (obj["transform"] * move) * ~T
    do = redraw_image(page, obj, lambda do: b"")
    doc = page.parent
    draw = b"\nq %.6f %.6f %.6f %.6f %.6f %.6f cm " % tuple(ctm) + do + b" Q\n"
    if behind:  # (behind the text: drawn first, the page's own drawing over it)
        xref = doc.get_new_xref()
        doc.update_object(xref, "<<>>")
        doc.update_stream(xref, draw)
        doc.xref_set_key(page.xref, "Contents", "[%s]" % " ".join(
            f"{x} 0 R" for x in [xref] + list(page.get_contents())))
        return
    page.wrap_contents()  # (the page's own drawing in q ... Q: what comes after starts afresh)
    last = page.get_contents()[-1]
    doc.update_stream(last, doc.xref_stream(last) + draw)


def delete_image(page, obj):
    redraw_image(page, obj, lambda do: b"")


def smooth_stroke(points, wobble=1.0, rounds=3):
    """A stroke drawn with the mouse made smooth, like drawing programs do: the mouse
    reports every pixel it crosses, so a stroke as it comes is all little steps and wobbles.
    Points closer than a pixel and a half are dropped, the wobble straightened out (any point
    within wobble pixels of the line through its neighbours goes - Douglas-Peucker), then
    the corners left rounded off (Chaikin's cutting, rounds times; the ends stay put)."""
    pts = []
    for x, y in points:
        if not pts or abs(x - pts[-1][0]) + abs(y - pts[-1][1]) >= 1.5:
            pts.append((float(x), float(y)))
    if len(points) and (not pts or pts[-1] != tuple(map(float, points[-1]))):
        pts.append(tuple(map(float, points[-1])))
    if len(pts) < 3:
        return pts

    def simplify(seg):
        (x0, y0), (x1, y1) = seg[0], seg[-1]
        dx, dy = x1 - x0, y1 - y0
        length = (dx * dx + dy * dy) ** 0.5 or 1e-9
        far, k = 0.0, 0
        for n in range(1, len(seg) - 1):
            d = abs(dy * (seg[n][0] - x0) - dx * (seg[n][1] - y0)) / length
            if d > far:
                far, k = d, n
        if far <= wobble:
            return [seg[0], seg[-1]]
        return simplify(seg[:k + 1])[:-1] + simplify(seg[k:])
    pts = simplify(pts)
    for _ in range(rounds):
        if len(pts) < 3:
            break
        out = [pts[0]]
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            out += [(0.75 * x0 + 0.25 * x1, 0.75 * y0 + 0.25 * y1),
                    (0.25 * x0 + 0.75 * x1, 0.25 * y0 + 0.75 * y1)]
        out.append(pts[-1])
        pts = out
    return pts


def drawing_rows(kind):
    """The pencil ("pen") or the highlighter ("marker"), pixel by pixel, 16 x 16 - lying
    from its tip at the bottom left up to the top right, like MS Paint's pencil (rows for
    pixel_icon: C / D the colour it draws in and a darker shade of it). The toolbar button
    is this; the pointer is this at twice the size - the same picture."""
    rows = []
    for y in range(16):
        row = ""
        for x in range(16):
            d = x + y - 15  # (across the pencil: 0 down its middle)
            a = x - y + 15  # (along it: 0 at the tip, a pixel and a half ... per 3)
            ch = "."
            if kind == "pen":
                half = min(a // 2, 2)  # (the point widening into the body)
                if 0 <= a <= 26 and abs(d) <= half:
                    if abs(d) == half or a >= 25:
                        ch = "K"  # (the outline)
                    elif a <= 2:
                        ch = "K"  # (the lead)
                    elif a <= 5:
                        ch = "W"  # (bare wood)
                    elif a <= 20:
                        ch = "C" if d < 1 else "D"  # (painted, in its colour; shaded below)
                    elif a <= 22:
                        ch = "S"  # (the metal band)
                    else:
                        ch = "P"  # (the rubber)
                if a == 0 and d == 0:
                    ch = "K"
            else:
                half = 1 if a <= 3 else 2  # (a chisel tip, then the barrel)
                if 0 <= a <= 26 and abs(d) <= half:
                    if abs(d) == half or a >= 25 or a == 10:
                        ch = "K"  # (the outline, the cap's seam)
                    elif a <= 9:
                        ch = "C" if d < 1 else "D"  # (the felt tip and its end, in the ink)
                    else:
                        ch = "S" if d < 0 else "G"  # (the grey barrel)
            row += ch
        rows.append(row)
    return rows


def drawing_icon(kind, color=None):
    """The toolbar button: the pointer's own pixels, cut to just the drawing - the pencil is
    19 x 19, it fits as it is; the highlighter (21 x 21) is drawn the same way a little
    smaller, to fit the button. Not shrunk from a bigger picture: crisp, like the pointer."""
    im = (pointer_drawing("pen", line=0.7, slim=0.6, short=0.8) if kind == "pen"  # (the
          # pencil slimmer, its outline thinner - both shorter than the pointers)
          else pointer_drawing(kind, color=color, shrink=19 / 21, short=0.92))[0]  # (18 x 18;
    # the marker's tip in the colour it draws in)
    return im.crop(im.getbbox())


def pointer_drawing(kind, size=32, color=None, shrink=1.0, line=1.2, slim=1.0, short=1.0):
    """A Windows 98-style pointer for drawing (MS Paint's way): a pencil ("pen") or a
    highlighter ("marker") lying from its tip at the bottom left up to the top right, its
    hot spot right at the tip - 32 x 32, as a picture with see-through round it. size 16:
    the same drawing for the toolbar button. color: the pencil's paint / the marker's ink
    in the colour the tool draws in."""
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    tx, ty = 1, size - 2  # (the tip)
    k = 24 / (size * 0.75) / shrink  # (the drawing is made for 32: a smaller one is the same,
    wide = 1 if size >= 32 else 1.35  # shrunk - a little fatter, so it still reads; shrink: the
    # 32 x 32 drawing itself made that much smaller, its shapes kept)
    half = 1 / 2 ** 0.5
    ink = hex_rgb_255(color) if color else None
    dark = tuple(round(v * 0.75) for v in ink) if ink else None
    for y in range(size):
        for x in range(size):
            a = ((x - tx) - (y - ty)) * half * k / short  # (along the pencil, from the tip;
            # short: how long, 1 as the pointer is)
            b = ((x - tx) + (y - ty)) * half * k / wide  # (across it)
            if kind == "pen":
                if a < 0 or a > 24:
                    continue
                width = min(a * 0.55, 2.6) * slim  # (slim: how fat, 1 as the pointer is)
                if abs(b) > width + 0.7:
                    continue
                edge = abs(b) > width + 0.7 - line or a > 24 - line * 0.67  # (line: how thick its outline)
                if a < 2.2:
                    color = (0, 0, 0)  # (the lead)
                elif edge:
                    color = (0, 0, 0)
                elif a < 6:
                    color = (233, 196, 140)  # (bare wood)
                elif a < 19:  # (painted: yellow, or the colour it draws in)
                    color = (ink or (255, 214, 0)) if b < 0.6 else (dark or (200, 150, 0))
                elif a < 20.5:
                    color = (192, 192, 192)  # (the metal band)
                else:
                    color = (255, 150, 170)  # (the rubber)
            else:
                if a < 0 or a > 24:
                    continue
                width = min(1.2 + a * 0.5, 3.2)  # (the felt tip widening into the barrel)
                if abs(b) > width + 0.7:
                    continue
                edge = abs(b) > width - 0.5 or a > 23.2 or 9 < a < 10  # (the cap's seam)
                if edge:
                    color = (0, 0, 0)
                elif a < 4.5:
                    color = ink or (255, 240, 0)  # (the felt tip, in the ink's colour)
                elif a < 9:
                    color = dark or (225, 200, 0)  # (the coloured end)
                else:
                    color = (160, 160, 160) if b < 0.6 else (110, 110, 110)  # (the barrel)
            im.putpixel((x, y), color + (255,))
    return im, (tx, ty)


def drawing_cursor(kind, color=None):
    """The pointer for drawing: the pencil / highlighter as it's always been. (picture,
    (x, y) of the hot spot)"""
    return pointer_drawing(kind, color=color if kind == "marker" else None)  # (the marker's
    # tip in the colour it draws in; the pencil as it is)


def cursor_file(kind, color=None):
    """drawing_cursor written as a Windows cursor file (.cur) for Tk (cursor="@file"): its
    path, or None if it can't be. (In the temp folder, by its short name: Tk can't take a
    path with spaces in it.)"""
    try:
        im, (hx, hy) = drawing_cursor(kind, color)
        w, h = im.size
        px = im.load()
        xor = b"".join(bytes((px[x, y][2], px[x, y][1], px[x, y][0], px[x, y][3]))
                       for y in range(h - 1, -1, -1) for x in range(w))
        mask = b""
        for y in range(h - 1, -1, -1):  # (1: see-through)
            bits = 0
            for x in range(w):
                bits = bits << 1 | (px[x, y][3] == 0)
            mask += bits.to_bytes(w // 8, "big")
        header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(xor) + len(mask), 0, 0, 0, 0)
        data = header + xor + mask
        tint = (color or "").lstrip("#").lower()
        path = os.path.join(tempfile.gettempdir(), f"mpdf_{kind}{tint}.cur")
        with open(path, "wb") as f:
            f.write(struct.pack("<HHH", 0, 2, 1))
            f.write(struct.pack("<BBBBHHII", w, h, 0, 0, hx, hy, len(data), 22))
            f.write(data)
        if sys.platform == "win32":
            import ctypes
            buf = ctypes.create_unicode_buffer(260)
            if ctypes.windll.kernel32.GetShortPathNameW(path, buf, 260):
                path = buf.value
        return None if " " in path else path.replace("\\", "/")
    except Exception:
        return None


class TextBar(tk.Toplevel):
    """The little floating toolbar (like Acrobat's) over selected text - copy it, mark it,
    edit it - or over a highlight (or other mark) picked up: copy, colour, remove.
    items: (picture, name, command, words beside the picture or None), None between groups;
    page and rects: what it floats over."""

    def __init__(self, view, kind, page, rects, items):
        super().__init__(view)
        self.withdraw()  # (shown once it's in place)
        self.overrideredirect(True)
        self.transient(view.winfo_toplevel())
        self.kind, self.page, self.rects = kind, page, rects
        frame = tk.Frame(self, bg=BG, relief="raised", bd=2)
        frame.pack()
        bar = tk.Frame(frame, bg=BG)
        bar.pack(padx=1, pady=1)
        for item in items:
            if item is None:
                tool_separator(bar).pack_configure(padx=2)
                continue
            icon, tip, command, label = item
            ToolButton(bar, icon, tip, command, label=label).pack(side="left")
        self.update_idletasks()
        try:  # (the main window has no Windows title bar, so Tk doesn't tell Windows who owns
            import ctypes  # the bar: told here, so it stays in front of the main window)
            u = ctypes.windll.user32
            owner = u.GetParent(view.winfo_toplevel().winfo_id())
            u.SetWindowLongPtrW(u.GetParent(self.winfo_id()), -8, owner)  # GWLP_HWNDPARENT
        except Exception:
            pass


def layout_icon(mode, small=False):
    """A layout's picture, as Word's: the picture (a grey dome - a thick half ring on short
    legs) and the text (thin blue lines) round it, over it or under it - 32 x 26 (small:
    16 x 16, for the button beside a picture)."""
    W, H = (16, 16) if small else (32, 26)
    k = 4  # (the dome drawn bigger and made smaller: smooth, like Word's)
    blue, grey, pale = (46, 116, 181, 255), (80, 80, 80, 255), (150, 168, 186, 255)
    if THEME == "dark":  # (on the dark face: a light grey dome, so it stands out)
        grey = (200, 200, 200, 255)
    if small:
        lt, rows = 1, [2, 6, 10, 14]
        R, th, legs, ax, ay = 3.5, 2, 2, 4.5, 5.5  # (the dome: radius, thickness, legs, place)
    else:
        lt, rows = 2, [3, 9, 15, 21]
        R, th, legs, ax, ay = 6.5, 3.5, 3, 9.5, 8.5
    lines = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    lp = lines.load()

    def line(x0, x1, y):
        for yy in range(y, min(H, y + lt)):
            for x in range(max(0, round(x0)), min(W, round(x1))):
                lp[x, yy] = blue

    def dome(color):
        big = Image.new("RGBA", (W * k, H * k), (0, 0, 0, 0))
        d = ImageDraw.Draw(big)
        x0, y0, r, t = ax * k, ay * k, R * k, th * k
        d.pieslice((x0, y0, x0 + 2 * r, y0 + 2 * r), 180, 360, fill=color)
        d.rectangle((x0, y0 + r, x0 + 2 * r, y0 + r + legs * k), fill=color)
        d.pieslice((x0 + t, y0 + t, x0 + 2 * r - t, y0 + 2 * r - t), 180, 360, fill=(0, 0, 0, 0))
        d.rectangle((x0 + t, y0 + r, x0 + 2 * r - t, y0 + r + legs * k), fill=(0, 0, 0, 0))
        return big.resize((W, H), Image.LANCZOS)
    left, right = ax, ax + 2 * R  # (the dome's sides)
    top, bottom = rows[0], rows[-1]
    under = None  # (drawn under the lines: Behind Text)
    over = None
    if mode == "inline":
        line(1, W - 1, top)
        ax, ay = (1, 6) if small else (2, 10)
        right = ax + 2 * R
        line(right + (1.5 if small else 2), W - 1, round(ay + R + legs) - lt)
        over = dome(grey)
    elif mode in ("square", "tight", "through"):
        line(1, W - 1, top)
        line(1, W - 1, bottom)
        gap = {"square": 2 if small else 3, "tight": 1 if small else 1.5,
               "through": 1 if small else 1.5}[mode]
        for n, y in enumerate(rows[1:-1]):
            hug = (1 if small else 2) if mode != "square" and n == 0 else 0  # (round its top)
            line(1, left - gap + hug, y)
            line(right + gap - hug, W - 1, y)
        if mode == "through":  # (text inside it too)
            line(left + th + 0.5, right - th - 0.5, rows[2])
        over = dome(grey)
    elif mode == "top_bottom":
        line(1, W - 1, top)
        line(1, W - 1, bottom)
        over = dome(grey)
    elif mode == "behind":
        for y in rows:
            line(1, W - 1, y)
        under = dome(pale)
    else:  # (in front)
        for y in rows:
            line(1, W - 1, y)
        over = dome(grey)
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    for layer in (under, lines, over):
        if layer is not None:
            im.alpha_composite(layer)
    return im


class LayoutButton(ToolButton):
    W, H = 40, 34  # (a big button: the layouts' pictures are bigger than a tool's)


class LayoutPanel(tk.Toplevel):
    """Layout Options (as Word's), beside a picture picked up: in line with the text, the
    ways text can wrap round it, behind or in front of the text - the one it has sunk in."""

    def __init__(self, view, sel, x, y):
        super().__init__(view)
        self.withdraw()  # (shown once its owner is told: else Windows keeps it behind)
        self.overrideredirect(True)
        self.transient(view.winfo_toplevel())
        self.view, self.sel = view, sel
        app = view.app
        now = app.picture_layout(*sel)
        outer = tk.Frame(self, bg=BG, relief="raised", bd=2)
        outer.pack()
        top = tk.Frame(outer, bg=BG)
        top.pack(fill="x", padx=6, pady=(4, 2))
        tk.Label(top, text="Layout Options", bg=BG, font=(FONT[0], FONT[1], "bold")).pack(side="left")
        cross = Image.new("RGBA", (8, 7), (0, 0, 0, 0))  # (Windows 98's close cross: 8 x 7)
        ink = (255, 255, 255, 255) if THEME == "dark" else (0, 0, 0, 255)
        for k in range(7):
            for px in (k, k + 1, 7 - k - 1, 7 - k):  # (not x: that's where the panel goes)
                if 0 <= px < 8:
                    cross.putpixel((px, k), ink)
        self._cross = ImageTk.PhotoImage(cross, master=self)
        tk.Button(top, image=self._cross, width=12, height=10, bd=2, bg=BG, activebackground=BG,
                  highlightthickness=0, command=self.close).pack(side="right")  # (square)
        groups = (("In Line with Text", ("inline",)),
                  ("With Text Wrapping", ("square", "tight", "through", "top_bottom", "behind",
                                          "front")))
        self.buttons = {}
        for title, modes in groups:
            tk.Label(outer, text=title, bg=BG, font=FONT).pack(anchor="w", padx=8, pady=(4, 1))
            grid = tk.Frame(outer, bg=BG)
            grid.pack(anchor="w", padx=10, pady=(0, 4))
            for n, mode in enumerate(modes):
                b = LayoutButton(grid, layout_icon(mode), dict(LAYOUTS)[mode],
                                 lambda m=mode: self.choose(m))
                b.set_latched(mode == now)
                b.grid(row=n // 3, column=n % 3, padx=1, pady=1)
                self.buttons[mode] = b
        self.update_idletasks()
        if x + self.winfo_reqwidth() > self.winfo_screenwidth():  # (no room on the right: left
            x = max(0, x - self.winfo_reqwidth() - 40)  # of the button)
        y = max(0, min(y, self.winfo_screenheight() - self.winfo_reqheight() - 40))
        self.geometry(f"+{x}+{y}")
        self.bind("<Escape>", lambda e: self.close())
        self.update_idletasks()
        try:  # (kept in front of the main window - see TextBar)
            import ctypes
            u = ctypes.windll.user32
            owner = u.GetParent(view.winfo_toplevel().winfo_id())
            u.SetWindowLongPtrW(u.GetParent(self.winfo_id()), -8, owner)
        except Exception:
            pass
        self.deiconify()
        self.lift()

    def choose(self, mode):
        i, obj = self.sel
        self.close()
        self.view.app.set_picture_layout(i, obj, mode)

    def close(self):
        if self.view.layout_panel is self:
            self.view.layout_panel = None
        self.destroy()


class DocView(tk.Frame):
    """The document, in the middle of the window like Acrobat's: the pages one under the
    other on a grey workspace, scrolled with the scrollbars or the mouse wheel (Ctrl + wheel
    zooms). Pages are drawn when they come into view. The mouse does what the chosen tool
    does - with none it selects text; Move drags the pages about and picks annotations up;
    the others add text, highlights, drawings and notes."""
    GAP = 14  # space round each page

    def __init__(self, parent, app):
        super().__init__(parent, bg=BG)
        self.app = app
        self.box = tk.Frame(self, bg=BG, relief="sunken", bd=2)
        c = self.canvas = tk.Canvas(self.box, bg="#7B7B7B", highlightthickness=0, bd=0,
                                    xscrollincrement=1, yscrollincrement=1)
        c.pack(fill="both", expand=True)
        self.vsb = FlatScrollbar(self, command=lambda *a: self.bar_scroll(c.yview, *a))
        self.hsb = FlatScrollbar(self, command=lambda *a: self.bar_scroll(c.xview, *a),
                                 orient="horizontal")
        self._wheel = []  # (Windows' wheel messages, read straight from it - see hook_wheel)
        self._smooth = 0.0  # (when a touchpad last scrolled: its small steps)
        self._rest = [0.0, 0.0]  # (scrolling's part pixels, carried over)
        self._pinch_end = 0.0  # (a pinch going on: until when - pages shown as a quick preview)
        self._previews = {}  # page -> its quick picture (for pinching), and the scale it's at
        self._preview_photos = []
        self._glide = None  # (a zoom being glided to: (percent, at))
        self._stroke = None  # (the pen's line while it's drawn)
        self.CURSORS = dict(DocView.CURSORS)
        self.after_idle(self.hook_wheel)
        c.config(yscrollcommand=self.on_yscroll, xscrollcommand=self.on_xscroll)
        self.box.grid(row=0, column=0, sticky="nsew")
        self.vsb.grid(row=0, column=1, sticky="ns")
        self.hsb.grid(row=1, column=0, sticky="ew")  # (only while the pages are wider than
        self.corner = tk.Frame(self, bg=BG, width=FlatScrollbar.W, height=FlatScrollbar.W)
        self.corner.grid(row=1, column=1)  # the window - see on_xscroll)
        self.rowconfigure(0, weight=1)
        self.columnconfigure(0, weight=1)
        self.zoom = "Fit width"  # a ZOOMS name, or a number (percent)
        self.stretch = 1.0  # how much Windows stretches the window (see page_picture)
        self.scale = 1.0  # screen pixels per PDF point
        self.rects = []  # each page's (x, y, w, h) on the canvas
        self.tiles = {}  # (page, column, row) -> (scale, PhotoImage, canvas item)
        self.selected = None  # the annotation picked up: (page, xref)
        self.edit_sel = None  # Edit tool: the text line or picture picked up: (page, object)
        self.hover = None  # Edit tool: what's under the mouse: (page, object)
        self._ghost = None  # (where a picture being resized will go, on the canvas)
        self.press = None  # what a press started: (kind, ...)
        self.editor = None  # the text box while typing a text annotation
        self.text_sel = None  # the text selected: {"page", "boxes" (one per line), "text"}
        self.text_bar = None  # (the floating toolbar over it)
        self._layout_job = None
        self._last_size = None
        self.start_panel = None  # the "No PDF is open" box, over the view
        c.bind("<Configure>", self.on_resize)
        c.bind("<ButtonPress-1>", self.on_press)
        c.bind("<B1-Motion>", self.on_drag)
        c.bind("<ButtonRelease-1>", self.on_release)
        c.bind("<Double-Button-1>", self.on_double)
        c.bind("<Triple-Button-1>", self.on_triple)
        c.bind("<Button-3>", self.on_right_click)
        c.bind("<Motion>", self.on_motion)
        c.bind("<MouseWheel>", self.on_wheel)
        c.bind("<Shift-MouseWheel>", lambda e: self.wheel_scroll(e, "x"))
        c.bind("<Control-MouseWheel>", self.on_ctrl_wheel)

    # ---- layout ----
    def fit_scale(self, mode):
        doc = self.app.doc
        sizes = self.app.page_sizes
        if not sizes:
            return 1.0
        vw = max(50, self.canvas.winfo_width() - 2 * self.GAP)
        vh = max(50, self.canvas.winfo_height() - 2 * self.GAP)
        i = min(self.app.current, len(sizes) - 1) if doc else 0
        w, h = sizes[i] if mode == "Fit page" else (max(s[0] for s in sizes), 0)
        return min(vw / w, vh / h) if mode == "Fit page" else vw / w

    def zoom_percent(self):
        return round(self.scale * 72 / SCREEN_DPI * 100)

    def set_zoom(self, zoom, keep=True, at=None):
        """Zoom to a ZOOMS name or a percent, keeping the same spot of the page in view - at
        the top, or under the mouse: at, (x, y) in the view."""
        self.zoom = zoom
        self.layout(keep=keep, at=at)

    def layout(self, keep=True, at=None, quick=False):
        """Place every page at the current zoom (the pictures are drawn again as they come
        into view). keep: the same spot of the current page stays at the top - or, at: (x, y)
        in the view, the spot there stays there (zooming at the mouse). quick: while a pinch
        goes on, the pages shown as a quick preview."""
        c = self.canvas
        anchor = self.view_anchor() if keep else None
        spot = self.spot_at(*at) if at and self.rects else None
        self.close_editor(commit=True)
        c.delete("all")
        self.tiles = {}
        sizes = self.app.page_sizes if self.app.doc else []
        if sizes and self.start_panel is not None:
            self.start_panel.destroy()
            self.start_panel = None
        if not sizes:
            self.rects = []
            c.config(scrollregion=(0, 0, 1, 1))
            # (over the view, not on the canvas: it's drawn like the rest of the app - made
            # once and kept: placed in the middle, it stays there as the window changes size)
            if self.start_panel is None or not self.start_panel.winfo_exists():
                self.start_panel = self.app.start_screen(self.box)
                self.start_panel.place(relx=0.5, rely=0.5, anchor="center")
            return
        self.scale = (self.fit_scale(self.zoom) if isinstance(self.zoom, str)
                      else self.zoom / 100 * SCREEN_DPI / 72)
        self.stretch = screen_stretch(self)  # (see page_picture)
        s, G = self.scale, self.GAP
        view_w = c.winfo_width()
        widest = max(w for w, h in sizes) * s
        total_w = max(view_w, widest + 2 * G)
        y, self.rects = G, []
        for i, (w, h) in enumerate(sizes):
            pw, ph = w * s, h * s
            x = round((total_w - pw) / 2)
            self.rects.append((x, round(y), round(pw), round(ph)))
            # a shadow down and right, then the paper (#FFFFFE: white in every theme)
            c.create_rectangle(x + 3, y + 3, x + pw + 3, y + ph + 3, fill="#4B4B4B", outline="",
                               tags="paper")
            c.create_rectangle(x, y, x + pw, y + ph, fill="#FFFFFE", outline="#000000",
                               tags=("paper", f"page{i}"))
            y += ph + G
        c.config(scrollregion=(0, 0, total_w, max(y, c.winfo_height())))
        if spot:  # (the spot under the mouse: under it still)
            i, fx, fy, sx, sy = spot
            x, py, w, h = self.rects[min(i, len(self.rects) - 1)]
            c.xview_moveto(max(0, x + fx * w - sx) / max(1, total_w))
            self.scroll_to_y(py + fy * h - sy)
        elif anchor:
            i, fy = anchor
            i = min(i, len(self.rects) - 1)
            x, py, w, h = self.rects[i]
            self.scroll_to_y(py + fy * h - G / 2)
        self.draw_overlays()
        if quick:
            self.draw_previews()
        self.app.schedule_render()

    def spot_at(self, sx, sy):
        """The page spot at (sx, sy) in the view: (page, how far across it, how far down, sx,
        sy) - for a spot between pages, the nearest page (its fractions past 0..1)."""
        cx, cy = self.canvas.canvasx(sx), self.canvas.canvasy(sy)
        i = min(range(len(self.rects)), key=lambda k: 0 if self.rects[k][1] <= cy <=
                self.rects[k][1] + self.rects[k][3] else min(abs(cy - self.rects[k][1]),
                                                             abs(cy - self.rects[k][1] - self.rects[k][3])))
        x, y, w, h = self.rects[i]
        return i, (cx - x) / max(1, w), (cy - y) / max(1, h), sx, sy

    def view_anchor(self):
        """(page at the top of the view, how far down it the top is: 0..1)."""
        if not self.rects:
            return None
        top = self.canvas.canvasy(0) + self.GAP / 2
        for i, (x, y, w, h) in enumerate(self.rects):
            if y + h + self.GAP > top:
                return i, min(max((top - y) / h, 0), 1)
        return len(self.rects) - 1, 0

    def scroll_to_y(self, y):
        x0, y0, x1, y1 = (float(v) for v in str(self.canvas.cget("scrollregion")).split())
        self.canvas.yview_moveto(max(0, y) / max(1, y1))

    def on_resize(self, e):
        size = (e.width, e.height)
        if size == self._last_size:
            return
        self._last_size = size
        if self._layout_job:
            self.after_cancel(self._layout_job)
        # a fitted zoom follows the window's size; others just re-centre the pages - keeping
        # the spot at the view's top left there, across and down (a zoom that brings in the
        # scrollbar along the bottom makes the view shorter: what's under the mouse stays put)
        self._layout_job = self.after(80, self.relayout_after_resize)

    def relayout_after_resize(self):
        """The view's size changed: the pages laid out again - unless text is being typed (that
        would close its box): then once it's done."""
        self._layout_job = None
        if self.editor:
            self._relayout_later = True
            return
        self.layout(keep=not self.rects, at=(0, 0) if self.rects else None)

    def on_xscroll(self, first, last):
        """The bar along the bottom only shows while there's something to scroll to."""
        self.hsb.set(first, last)
        self.place_text_bar()
        need = float(first) > 0 or float(last) < 1
        if need != bool(self.hsb.winfo_manager()):
            for w in (self.hsb, self.corner):
                w.grid() if need else w.grid_remove()

    def on_yscroll(self, first, last):
        self.vsb.set(first, last)
        self.place_text_bar()
        page = self.page_in_view()
        if page is not None and page != self.app.current:
            self.app.set_current(page, scroll=False)
        self.app.schedule_render()
        if self.app.tool == "link" and not getattr(self, "_links_job", None):
            # (the Link tool's dotted boxes: drawn for the pages scrolled into view too)
            self._links_job = self.after(60, self.redraw_link_boxes)

    def redraw_link_boxes(self):
        self._links_job = None
        self.draw_overlays()

    def page_in_view(self):
        """The page filling most of the middle of the view."""
        if not self.rects:
            return None
        c = self.canvas
        top, bottom = c.canvasy(0), c.canvasy(c.winfo_height())
        best, best_h = None, -1
        for i, (x, y, w, h) in enumerate(self.rects):
            shown = min(bottom, y + h) - max(top, y)
            if shown > best_h:
                best, best_h = i, shown
            if y > bottom:
                break
        return best

    def go_to(self, i):
        if 0 <= i < len(self.rects):
            self.scroll_to_y(self.rects[i][1] - self.GAP / 2)

    def visible_pages(self, margin=1.0):
        """The pages in view, and those within margin views of it (drawn ahead)."""
        c = self.canvas
        h = c.winfo_height()
        top, bottom = c.canvasy(0) - margin * h, c.canvasy(h) + margin * h
        return [i for i, (x, y, w, ph) in enumerate(self.rects) if y + ph >= top and y <= bottom]

    # ---- drawing the pages (called by the app's render loop, PDF_LOCK held) ----
    def next_render(self):
        """A job drawing the next tile of a page that's in view (or just out of it) but not
        drawn at this zoom - the ones in the middle of the view first - or None. Tiles far
        from view are let go (they'd only use memory)."""
        c = self.canvas
        vw, vh = max(1, c.winfo_width()), max(1, c.winfo_height())
        left, top = c.canvasx(0), c.canvasy(0)

        def region(mx, my):
            return left - mx * vw, top - my * vh, left + vw + mx * vw, top + vh + my * vh

        def meets(a, b):
            return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]
        if self.pinching():
            return None  # (a pinch going on: its quick preview shows meanwhile)
        keep, near = region(1.0, 2.0), region(0.25, 0.75)
        for key in [k for k in self.tiles if k[0] >= len(self.rects)
                    or not meets(self.tile_box(k), keep)]:
            c.delete(self.tiles.pop(key)[2])
        mid = (left + vw / 2, top + vh / 2)
        best = None
        for i in self.visible_pages(margin=0.75):
            x, y, w, h = self.rects[i]
            for ty in range(max(0, int((near[1] - y) // TILE)), int(min(h, near[3] - y) // TILE) + 1):
                for tx in range(max(0, int((near[0] - x) // TILE)), int(min(w, near[2] - x) // TILE) + 1):
                    key = (i, tx, ty)
                    box = self.tile_box(key)
                    if box[0] >= box[2] or box[1] >= box[3] or not meets(box, near):
                        continue
                    if key in self.tiles and self.tiles[key][0] == self.scale:
                        continue
                    # (in view: by how far from its middle; out of it: after all of those)
                    d = (abs((box[0] + box[2]) / 2 - mid[0]) + abs((box[1] + box[3]) / 2 - mid[1])
                         + (0 if meets(box, region(0, 0)) else 1e6))
                    if best is None or d < best[0]:
                        best = (d, key)
        if best is None or best[0] >= 1e6:  # (all of the view drawn: the preview can go)
            c.delete("preview")
            self._preview_photos = []
        return (lambda key=best[1]: self.render(key)) if best else None

    def tile_box(self, key):
        """A tile's place on the canvas: (x0, y0, x1, y1)."""
        i, tx, ty = key
        x, y, w, h = self.rects[i]
        return (x + tx * TILE, y + ty * TILE, x + min(w, (tx + 1) * TILE), y + min(h, (ty + 1) * TILE))

    def render(self, key):
        i, tx, ty = key
        if i >= len(self.rects):
            return
        page = self.app.doc[i]
        s = self.scale
        x, y, w, h = self.rects[i]
        x0, y0, x1, y1 = self.tile_box(key)
        img = tile_picture(page, s, getattr(self, "stretch", 1.0), (x0 - x, y0 - y, x1 - x, y1 - y),
                           (w, h))
        photo = ImageTk.PhotoImage(img, master=self.canvas)
        if key in self.tiles:
            self.canvas.delete(self.tiles[key][2])
        item = self.canvas.create_image(x0, y0, image=photo, anchor="nw", tags="pic")
        # (over the page - and over a pinch's quick preview, so it turns sharp tile by tile)
        self.canvas.tag_raise(item, "preview" if self.canvas.find_withtag("preview") else f"page{i}")
        self.canvas.tag_raise("overlay")
        self.canvas.tag_raise("editcover")
        self.canvas.tag_raise("editor")
        self.tiles[key] = (s, photo, item)

    def invalidate(self, pages):
        """These pages changed (an annotation added...): draw them again."""
        for i in pages:
            self._previews.pop(i, None)
        for key in self.tiles:
            if key[0] in pages:  # (kept on screen until redrawn)
                self.tiles[key] = (None,) + self.tiles[key][1:]
        self.draw_overlays()
        self.app.schedule_render()

    # ---- from the canvas to the page and back ----
    def page_at(self, cx, cy, slack=0):
        for i, (x, y, w, h) in enumerate(self.rects):
            if x - slack <= cx <= x + w + slack and y - slack <= cy <= y + h + slack:
                return i
        return None

    def to_pdf(self, i, cx, cy):
        """A canvas point -> the point on page i, in the page's own (unrotated) space."""
        x, y = self.rects[i][:2]
        page = self.app.doc[i]
        return pymupdf.Point((cx - x) / self.scale, (cy - y) / self.scale) * page.derotation_matrix

    def to_canvas(self, i, rect):
        """A rectangle in page i's own space -> (x0, y0, x1, y1) on the canvas."""
        x, y = self.rects[i][:2]
        r = pymupdf.Rect(rect) * self.app.doc[i].rotation_matrix
        s = self.scale
        return x + r.x0 * s, y + r.y0 * s, x + r.x1 * s, y + r.y1 * s

    def annot_at(self, i, cx, cy, skip=()):
        """The topmost annotation under a canvas point on page i: (xref, kind, rect), or None.
        (Only what's needed is kept: an annotation can't be used once its page is let go.)
        skip: kinds left out."""
        found, page = None, self.app.doc[i]
        for annot in page.annots():
            if annot.type[1] in skip:
                continue
            x0, y0, x1, y1 = self.to_canvas(i, annot.rect)
            if x0 - 3 <= cx <= x1 + 3 and y0 - 3 <= cy <= y1 + 3:
                found = (annot.xref, annot.type[1], pymupdf.Rect(annot.rect))
        return found

    def markup_at(self, i, cx, cy):
        """The text mark (highlight, strike-out...) under a canvas point on page i - its
        xref - or None. Found by its pieces, not its whole box (a mark over two lines
        doesn't cover the space beside them)."""
        found, page, p = None, self.app.doc[i], self.to_pdf(i, cx, cy)
        for annot in page.annots():
            if annot.type[1] in MARKUPS and any(r.contains(p) for r in quad_rects(annot)):
                found = annot.xref
        return found

    def annot_pieces(self, i, xref):
        """A text mark's pieces (in the page's own space), or [] if it's not there."""
        page = self.app.doc[i]
        annot = page.load_annot(xref)
        return quad_rects(annot) if annot is not None and annot.type[1] in MARKUPS else []

    def selected_info(self):
        """(kind, rect) of the annotation picked up, or None."""
        if not self.selected or not self.app.doc:
            return None
        i, xref = self.selected
        if i >= self.app.doc.page_count:
            return None
        page = self.app.doc[i]
        for annot in page.annots():
            if annot.xref == xref:
                return annot.type[1], pymupdf.Rect(annot.rect)
        return None

    def select(self, sel):
        if self.text_bar is not None and self.text_bar.kind == "annot":
            self.close_bar()  # (the mark's toolbar goes with it)
        self.selected = sel
        self.draw_overlays()
        self.app.update_ui()

    # ---- what's drawn over the pages: the picked-up annotation, the found text ----
    def over_picked_picture(self):
        """Whether the mouse is on the picture picked up with the Edit tool (or just round it,
        on its handles) - the text under it is still found, but not outlined there."""
        sel = self.edit_sel
        if not sel or sel[1]["kind"] != "image" or sel[0] >= len(self.rects):
            return False
        px, py = getattr(self, "_pointer", (-1e9, -1e9))
        x0, y0, x1, y1 = self.to_canvas(sel[0], sel[1]["rect"])
        return x0 - 8 <= px <= x1 + 8 and y0 - 8 <= py <= y1 + 8

    def draw_overlays(self):
        c = self.canvas
        c.delete("overlay")
        info = self.selected_info()
        if info is not None and info[0] in MARKUPS:  # a text mark: a dotted box round each
            color = untranslated(select_colors()[0], "edge")  # piece (it can't be dragged)
            for r in self.annot_pieces(*self.selected):
                x0, y0, x1, y1 = self.to_canvas(self.selected[0], r)
                c.create_rectangle(x0 - 1, y0 - 1, x1 + 1, y1 + 1, outline=color, dash=(2, 2),
                                   tags="overlay")
        elif info is not None:
            x0, y0, x1, y1 = self.to_canvas(self.selected[0], info[1])
            color = untranslated(select_colors()[0], "edge")
            c.create_rectangle(x0 - 2, y0 - 2, x1 + 2, y1 + 2, outline=color, dash=(2, 2),
                               width=1, tags="overlay")
            for hx, hy in ((x0, y0), (x1, y0), (x0, y1), (x1, y1)):  # little corner handles
                c.create_rectangle(hx - 3, hy - 3, hx + 3, hy + 3, fill=color, outline="",
                                   tags="overlay")
        if self.app.tool == "edit":  # the Edit tool: a dotted box round what's under the
            # mouse, a solid one round what's picked up (with handles on a picture's corners)
            color = untranslated(select_colors()[0], "edge")
            sel = self.edit_sel
            over_pic = self.over_picked_picture()  # (the text under it isn't outlined)
            if self.hover and not (sel and self.hover[1] is sel[1]) and not over_pic                     and self.hover[0] < len(self.rects):
                x0, y0, x1, y1 = self.to_canvas(self.hover[0], self.hover[1]["rect"])
                c.create_rectangle(x0 - 2, y0 - 2, x1 + 2, y1 + 2, outline=color, dash=(1, 2),
                                   tags="overlay")
            if sel and sel[0] < len(self.rects) and not self.editor:  # (typing: no box)
                x0, y0, x1, y1 = self.to_canvas(sel[0], sel[1].get("box") or sel[1]["rect"])
                c.create_rectangle(x0 - 2, y0 - 2, x1 + 2, y1 + 2, outline=color, tags="overlay")
                if sel[1].get("frame"):  # a frame: handles at its corners and sides (wider /
                    for _code, hx, hy in self.frame_handles(x0, y0, x1, y1):  # narrower: rewrapped)
                        c.create_oval(hx - 4, hy - 4, hx + 4, hy + 4, fill="#FFFFFE",
                                      outline=color, width=2, tags="overlay")
                elif sel[1]["kind"] == "text":  # round handles at its ends: bigger / smaller
                    for hx, hy in self.text_handles(x0, y0, x1, y1):
                        c.create_oval(hx - 4, hy - 4, hx + 4, hy + 4, fill="#FFFFFE",
                                      outline=color, width=2, tags="overlay")
                if sel[1]["kind"] == "image":
                    for hx, hy in self.handles(x0, y0, x1, y1):
                        c.create_rectangle(hx - 3, hy - 3, hx + 4, hy + 4, fill="#FFFFFE",
                                           outline="#000000", tags="overlay")
                    self.draw_layout_button(x1 + 10, y0 - 2)
        if self.app.tool == "link" and self.app.doc:  # where the links are: dotted boxes
            for i in self.visible_pages(margin=0.2):
                for link in self.app.links_on(i):
                    c.create_rectangle(*self.display_to_canvas(i, pymupdf.Rect(link["from"])),
                                       outline=untranslated("#0000EE", "edge"), dash=(2, 2),
                                       tags="overlay")
        ts = self.text_sel
        self._sel_photos = []
        if ts and ts["page"] < len(self.rects):  # the text selected: tinted in the selection
            rgb = hex_rgb_255(select_colors()[0])  # colour, see-through
            for r in ts["boxes"]:
                x0, y0, x1, y1 = (round(v) for v in self.to_canvas(ts["page"], r))
                tint = Image.new("RGBA", (max(1, x1 - x0), max(1, y1 - y0)), rgb + (90,))
                photo = ImageTk.PhotoImage(tint, master=c)
                self._sel_photos.append(photo)  # (kept, or Tk would lose the pictures)
                c.create_image(x0, y0, image=photo, anchor="nw", tags="overlay")
        self.place_text_bar()
        app = self.app
        if app.find_text and app.find_hits:  # (every place the words were found: yellow, as
            tints = {}  # Word's; the one you're at in the theme's selection colour, see-through)
            picked = hex_rgb_255(select_colors()[0])
            for k, (i, rect) in enumerate(app.find_hits):
                if i >= len(self.rects):
                    continue
                x0, y0, x1, y1 = (round(v) for v in self.to_canvas(i, rect))
                now = k == app.find_pos
                size = (max(1, x1 - x0), max(1, y1 - y0), now)
                if size not in tints:
                    tint = Image.new("RGBA", size[:2], picked + (110,) if now else (255, 225, 0, 120))
                    tints[size] = ImageTk.PhotoImage(tint, master=c)
                    self._sel_photos.append(tints[size])
                c.create_image(x0, y0, image=tints[size], anchor="nw", tags="overlay")

    def draw_layout_button(self, x, y):
        """The Layout Options button beside a picture picked up (as Word's): a raised button
        with the picture's layout on it."""
        c = self.canvas
        mode = self.app.picture_layout(*self.edit_sel)
        self._layout_photo = ImageTk.PhotoImage(layout_icon(mode, small=True), master=c)
        tags = ("overlay", "layoutbtn")
        W = H = 24
        c.create_rectangle(x, y, x + W, y + H, fill=BG, outline="", tags=tags)
        c.create_line(x, y + H - 1, x, y, x + W - 1, y, fill=EDGE_LIGHT, tags=tags)
        c.create_line(x + 1, y + H - 2, x + 1, y + 1, x + W - 2, y + 1, fill=BG, tags=tags)
        c.create_line(x, y + H - 1, x + W - 1, y + H - 1, x + W - 1, y - 1, fill="#000000", tags=tags)
        c.create_line(x + 1, y + H - 2, x + W - 2, y + H - 2, x + W - 2, y, fill=thin_shadow(),
                      tags=tags)
        c.create_image(x + W // 2, y + H // 2, image=self._layout_photo, tags=tags)

    def open_layout_panel(self):
        sel = self.edit_sel
        if not sel or sel[1]["kind"] != "image":
            return
        if getattr(self, "layout_panel", None) is not None:
            self.layout_panel.close()
        box = self.canvas.bbox("layoutbtn")
        if not box:
            return
        x = self.canvas.winfo_rootx() + round(box[2] - self.canvas.canvasx(0)) + 6
        y = self.canvas.winfo_rooty() + round(box[1] - self.canvas.canvasy(0))
        self.layout_panel = LayoutPanel(self, sel, x, y)

    def show_rect(self, i, rect):
        """Scroll so a spot on page i (in its own space) is in view."""
        x0, y0, x1, y1 = self.to_canvas(i, rect)
        c = self.canvas
        top, bottom = c.canvasy(0), c.canvasy(c.winfo_height())
        if not (top + 20 <= y0 and y1 <= bottom - 20):
            self.scroll_to_y(y0 - c.winfo_height() / 3)
        sx0, sy0, sx1, sy1 = (float(v) for v in str(c.cget("scrollregion")).split())
        left, right = c.canvasx(0), c.canvasx(c.winfo_width())
        if not (left <= x0 and x1 <= right):
            c.xview_moveto(max(0, x0 - 40) / max(1, sx1))

    # ---- the mouse ----
    CURSORS = {"select": "", "move": "hand2", "edit": "", "text": "xterm", "highlight": "pencil",
               "pen": "pencil", "eraser": "crosshair", "image": "crosshair",
               "shape": "crosshair",
               "sign": "crosshair", "link": "crosshair"}
    BOX_TOOLS = ("eraser", "image", "shape", "link")  # (tools you drag a box with)

    def display_to_canvas(self, i, rect):
        """A rectangle on page i as it's shown (not its own space: links come like this)."""
        x, y = self.rects[i][:2]
        s = self.scale
        return x + rect.x0 * s, y + rect.y0 * s, x + rect.x1 * s, y + rect.y1 * s

    def link_at(self, i, cx, cy):
        """The link under a canvas point on page i, or None."""
        for link in self.app.links_on(i):
            x0, y0, x1, y1 = self.display_to_canvas(i, pymupdf.Rect(link["from"]))
            if x0 <= cx <= x1 and y0 <= cy <= y1:
                return link
        return None

    def on_motion(self, e):
        if not self.rects or self.press:
            return
        cx, cy = self.canvas.canvasx(e.x), self.canvas.canvasy(e.y)
        self._pointer = (cx, cy)  # (where the mouse is: see draw_overlays)
        on_pic = self.over_picked_picture()
        if on_pic != getattr(self, "_on_pic", False):  # (on / off the picture: its outline
            self._on_pic = on_pic  # of the text under it hidden / shown)
            self.draw_overlays()
        i = self.page_at(cx, cy)
        tool = self.app.tool
        cursor = ""
        rubbing = tool == "eraser" and self.app.variant["eraser"] == "rub"
        if rubbing and i is not None:  # (the eraser: its ring for a pointer)
            self.eraser_ring(cx, cy)
        elif self.canvas.find_withtag("eraserring"):
            self.canvas.delete("eraserring")
        if tool == "edit":
            h = self.handle_at(cx, cy)
            obj = self.app.object_at(i, self.to_pdf(i, cx, cy)) if i is not None else None
            hover = (i, obj) if obj else None
            if (hover and hover[1]) is not (self.hover and self.hover[1]):
                self.hover = hover
                self.draw_overlays()
            if h is not None:
                if isinstance(h, str):  # (a frame's: its corners slanted, its sides across)
                    cursor = ("size_nw_se" if h in ("nw", "se") else
                              "size_ne_sw" if h in ("ne", "sw") else "sb_h_double_arrow")
                else:
                    cursor = ("sb_h_double_arrow" if h > 3 else
                              "size_nw_se" if h in (0, 3) else "size_ne_sw")
            elif obj:
                cursor = "xterm" if obj["kind"] == "text" else "fleur"
        elif tool == "select" and i is not None:
            found = self.annot_at(i, cx, cy, skip=MARKUPS)
            link = self.link_at(i, cx, cy)
            if found is not None and (found[1] in MOVABLE or not self.word_at(i, cx, cy)):
                cursor = "fleur"
            elif link:
                cursor = "hand2"
                self.app.say(self.app.link_text(link) + " (click to go there)")
            elif self.word_at(i, cx, cy):
                cursor = "xterm"
        elif i is not None:
            cursor = "none" if rubbing and i is not None else self.CURSORS[tool]
            if tool == "move" and self.annot_at(i, cx, cy) is not None:
                cursor = "fleur"
            elif tool == "move":  # a link: the pointing hand, and where it goes
                link = self.link_at(i, cx, cy)
                if link:
                    self.app.say(self.app.link_text(link))
        elif tool == "move":
            cursor = "hand2"
        if str(self.canvas.cget("cursor")) != cursor:
            self.canvas.config(cursor=cursor)

    def bar_scroll(self, view, *args):
        """The scrollbars: their arrows a step (20 pixels) at a time, as before - the canvas
        itself now scrolls by the pixel."""
        if len(args) == 3 and args[0] == "scroll" and args[2] == "units":
            view("scroll", int(args[1]) * 20, "units")
        else:
            view(*args)

    def hook_wheel(self):
        """Read the mouse wheel and touchpad straight from Windows. Tk (8.6) adds a touchpad's
        small steps up into whole wheel notches, so scrolling and pinching with it would jump
        a notch at a time, and its sideways scrolling is lost. Every wheel message the app's
        thread gets is looked at - whichever of its windows Windows sends it to (a pinch goes
        to the one with the keyboard, not always the one under the fingers) - and taken when
        the mouse is over the pages. They're only noted here (Tk mustn't be called from inside
        Windows' message handling) and acted on a moment later (see take_wheel)."""
        if sys.platform != "win32":
            return
        try:
            import ctypes
            from ctypes import wintypes as W
            u, k = ctypes.windll.user32, ctypes.windll.kernel32
            LRESULT = ctypes.c_ssize_t

            class MSG(ctypes.Structure):
                _fields_ = [("hwnd", W.HWND), ("message", W.UINT), ("wParam", W.WPARAM),
                            ("lParam", W.LPARAM), ("time", W.DWORD), ("pt", W.POINT)]
            hook_type = ctypes.WINFUNCTYPE(LRESULT, ctypes.c_int, W.WPARAM, W.LPARAM)
            u.SetWindowsHookExW.restype = ctypes.c_void_p
            u.SetWindowsHookExW.argtypes = [ctypes.c_int, hook_type, ctypes.c_void_p, W.DWORD]
            u.CallNextHookEx.restype = LRESULT
            u.CallNextHookEx.argtypes = [ctypes.c_void_p, ctypes.c_int, W.WPARAM, W.LPARAM]
            self._view_box = (0, 0, 0, 0)  # (the pages' place on the screen: kept up to date)
            log = os.environ.get("MASTERPDF_WHEEL_LOG")

            def hook(code, wp, lp):
                if code >= 0:
                    m = ctypes.cast(lp, ctypes.POINTER(MSG)).contents
                    if m.message in (0x020A, 0x020E):  # WM_MOUSEWHEEL, WM_MOUSEHWHEEL
                        x = ctypes.c_short(m.lParam & 0xFFFF).value
                        y = ctypes.c_short((m.lParam >> 16) & 0xFFFF).value
                        x0, y0, x1, y1 = self._view_box
                        if x0 <= x < x1 and y0 <= y < y1:
                            if wp == 1:  # (taken off the queue: noted once)
                                delta = ctypes.c_short((m.wParam >> 16) & 0xFFFF).value
                                ctrl = bool(m.wParam & 8) or bool(u.GetKeyState(0x11) & 0x8000)
                                self._wheel.append((m.message == 0x020E, delta, ctrl,
                                                    bool(m.wParam & 4), x, y))
                                if log:
                                    with open(log, "a") as f:
                                        f.write(f"{time.monotonic():.3f} {'H' if m.message == 0x020E else 'V'}"
                                                f" delta={delta} ctrl={ctrl} hwnd={m.hwnd}\n")
                            m.message = 0  # (WM_NULL: Tk doesn't act on it too)
                return u.CallNextHookEx(None, code, wp, lp)
            self._hook_proc = hook_type(hook)  # (kept: Windows calls it as long as the app runs)
            self._hook = u.SetWindowsHookExW(3, self._hook_proc, None, k.GetCurrentThreadId())
        except Exception:
            return  # (then Tk's own wheel events do it, a notch at a time)
        self.take_wheel()

    def take_wheel(self):
        """What the wheel / touchpad did since last time, all at once: a mouse wheel scrolls
        and zooms a notch at a time; a touchpad (its steps aren't whole notches) scrolls as far
        as the fingers move, and pinching zooms smoothly - at the mouse, both. Notches of
        Ctrl + wheel coming in a quick run (how some touchpads pinch) zoom smoothly too:
        glided to, not jumped."""
        try:
            c = self.canvas
            x, y = c.winfo_rootx(), c.winfo_rooty()
            self._view_box = (x, y, x + c.winfo_width(), y + c.winfo_height())
        except tk.TclError:
            return
        msgs, self._wheel[:] = list(self._wheel), []
        now = time.monotonic()
        dx = dy = 0.0
        zoom, at = 1.0, None
        for sideways, delta, ctrl, shift, x, y in msgs:
            pad = delta % 120 != 0 or now - self._smooth < 0.4
            if delta % 120:
                self._smooth = now
            if ctrl and not sideways and self.rects:  # zoom, at the mouse
                at = (x - self.canvas.winfo_rootx(), y - self.canvas.winfo_rooty())
                burst = now - getattr(self, "_last_notch", 0) < 0.08  # (a pinch: 10-40 ms apart)
                self._last_notch = now
                if pad:  # (a touchpad's fine steps: followed as they come)
                    zoom *= 2 ** (delta / 480)
                    continue
                base = self._glide[0] if self._glide else self.zoom_percent_exact()
                if burst and self._glide:  # (notches in a quick run - a pinch: smoothly on)
                    target = base * 2 ** (delta / 480)
                else:  # (a mouse notch: to the next 25% step - glided there, quickly)
                    steps = [z for z in ZOOM_STEPS if (z > base + 1 if delta > 0 else z < base - 1)]
                    target = (steps[0] if delta > 0 else steps[-1]) if steps else base
                self._glide = (min(max(target, ZOOM_STEPS[0]), ZOOM_STEPS[-1]), at)
                continue
            px = delta / 2  # (a notch: 60 pixels, as before; a touchpad: as far as it moves)
            if sideways:
                dx += px
            elif shift:
                dx -= px
            else:
                dy -= px
        if dx or dy:
            self.scroll_by(dx, dy)
        if zoom != 1.0 and at:
            self.pinch(zoom, at)
        if self._glide:
            target, where = self._glide
            here = self.zoom_percent_exact()
            step = (target - here) * 0.35
            if abs(target - here) < 0.3:
                self._glide = None
                step = target - here
            if step:
                self.pinch((here + step) / here, where)
        if self._pinch_end and now > self._pinch_end and not self._glide:
            self._pinch_end = 0.0  # (the pinch over: drawn sharp now)
            self.app.schedule_render()
        self.after(8 if (msgs or self._pinch_end or self._glide) else 16, self.take_wheel)

    def zoom_percent_exact(self):
        return self.scale * 72 / SCREEN_DPI * 100

    def scroll_by(self, dx, dy):
        """Scroll the pages by (dx, dy) pixels - parts of pixels carried over to next time."""
        c = self.canvas
        sx0, sy0, sx1, sy1 = (float(v) for v in str(c.cget("scrollregion")).split())
        for k, (d, view, first, size) in enumerate((
                (dx, c.xview_moveto, c.canvasx(0), sx1), (dy, c.yview_moveto, c.canvasy(0), sy1))):
            d += self._rest[k]
            whole = int(d)
            self._rest[k] = d - whole
            if whole:
                view(max(0, first + whole) / max(1, size))

    def pinching(self):
        return self._pinch_end > 0

    def pinch(self, factor, at):
        """A touchpad pinch: zoomed smoothly at the mouse - the pages shown as a quick
        preview meanwhile (drawing them sharp at every step couldn't keep up), drawn sharp
        once the fingers stop."""
        now = self.zoom_percent_exact()
        new = min(max(now * factor, ZOOM_STEPS[0]), ZOOM_STEPS[-1])
        if abs(new - now) < 0.05:
            return
        self._pinch_end = time.monotonic() + 0.22
        self.zoom = round(new, 2)
        self.layout(keep=False, at=at, quick=True)
        self.app.update_ui()

    def draw_previews(self):
        """The pages in view, quickly: each page's quick picture (drawn once, without the
        sharpening) cut to what's shown and stretched to the zoom."""
        c = self.canvas
        self._preview_photos = []
        left, top = c.canvasx(0), c.canvasy(0)
        right, bottom = left + c.winfo_width(), top + c.winfo_height()
        for i in self.visible_pages(margin=0):
            x, y, w, h = self.rects[i]
            vx0, vy0, vx1, vy1 = max(x, left), max(y, top), min(x + w, right), min(y + h, bottom)
            if vx1 - vx0 < 1 or vy1 - vy0 < 1:
                continue
            src = self.preview_source(i)
            if src is None:
                continue
            k = src.width / w
            part = src.crop((round((vx0 - x) * k), round((vy0 - y) * k),
                             max(round((vx0 - x) * k) + 1, round((vx1 - x) * k)),
                             max(round((vy0 - y) * k) + 1, round((vy1 - y) * k))))
            part = part.resize((round(vx1 - vx0), round(vy1 - vy0)), Image.BILINEAR)
            photo = ImageTk.PhotoImage(part, master=c)
            self._preview_photos.append(photo)
            item = c.create_image(vx0, vy0, image=photo, anchor="nw", tags=("preview", "pic"))
            c.tag_raise(item, f"page{i}")
        c.tag_raise("overlay")

    def preview_source(self, i):
        """A page's quick picture: about 1200 pixels across, made once."""
        if i not in self._previews:
            if not PDF_LOCK.acquire(blocking=False):
                return None
            try:
                page = self.app.doc[i]
                k = 1200 / max(1, page.rect.width)
                pix = page.get_pixmap(matrix=pymupdf.Matrix(k, k), alpha=False)
                self._previews[i] = Image.frombytes("RGB", (pix.width, pix.height), pix.samples)
            except Exception:
                return None
            finally:
                PDF_LOCK.release()
        return self._previews[i]

    def on_wheel(self, e):
        self.wheel_scroll(e, "y")

    def wheel_scroll(self, e, axis):
        if not e.delta:
            return
        steps = -int(e.delta / 120) * 3 or (-1 if e.delta > 0 else 1)
        (self.canvas.yview_scroll if axis == "y" else self.canvas.xview_scroll)(steps * 20, "units")

    def on_ctrl_wheel(self, e):
        if e.delta:
            self.app.zoom_step(1 if e.delta > 0 else -1, at=(e.x, e.y))
        return "break"

    def on_press(self, e):
        self.canvas.focus_set()
        if self.editor:  # typing text: a click anywhere else finishes it
            self.close_editor(commit=True)
            return
        if not self.rects:
            return
        if getattr(self, "layout_panel", None) is not None:
            self.layout_panel.close()
        bx = self.canvas.bbox("layoutbtn")  # (the Layout Options button: found by where it is -
        ex, ey = self.canvas.canvasx(e.x), self.canvas.canvasy(e.y)  # it's drawn again as
        if bx and bx[0] <= ex <= bx[2] and bx[1] <= ey <= bx[3]:  # the mouse moves)
            self.open_layout_panel()
            return
        self.clear_text_sel()
        cx, cy = self.canvas.canvasx(e.x), self.canvas.canvasy(e.y)
        i = self.page_at(cx, cy)
        tool = self.app.tool
        if tool == "edit":
            h = self.handle_at(cx, cy)
            if h is not None:  # a picture's corner: resize it; a line's round handle: the
                si, obj = self.edit_sel  # line bigger or smaller
                kind = "reflow" if isinstance(h, str) else "stretch" if h > 3 else "resize"
                self.press = (kind, si, obj, h, self.to_canvas(si, obj.get("box") or obj["rect"]))
                return
            obj = self.app.object_at(i, self.to_pdf(i, cx, cy)) if i is not None else None
            sel = self.edit_sel
            again = bool(obj and sel and sel[0] == i and sel[1] is obj)
            self.select_object((i, obj) if obj else None)
            if obj:  # (a click on the line picked up retypes it; a drag moves it)
                self.press = ("shift", i, obj, cx, cy, self.to_canvas(i, obj["rect"]), again)
            return
        if tool == "select":  # no tool: drag across text to select it
            if i is None:
                return
            found = self.annot_at(i, cx, cy, skip=MARKUPS)
            if found is not None and (found[1] in MOVABLE or not self.word_at(i, cx, cy)):
                self.press_annot(i, found, cx, cy)
                return
            if self.selected:
                self.select(None)
            # (let go without moving: a mark there is picked up, or a link followed)
            self.press = ("select", i, cx, cy, self.markup_at(i, cx, cy),
                          self.link_at(i, cx, cy), e.x, e.y)
            return
        if tool == "move":
            found = self.annot_at(i, cx, cy) if i is not None else None
            if found is not None:
                self.press_annot(i, found, cx, cy)
                return
            if self.selected:
                self.select(None)
            link = self.link_at(i, cx, cy) if i is not None else None
            if link:  # (followed if the mouse lets go without moving)
                self.press = ("pan", link, e.x, e.y)
                self.canvas.scan_mark(e.x, e.y)
                return
            self.press = ("pan",)
            self.canvas.scan_mark(e.x, e.y)
            self.canvas.config(cursor="fleur")
            return
        if i is None:
            return
        if tool == "text":  # the PDF's own text, in the font chosen
            self.app.wait_fonts()
            family = self.app.text_font
            _path, fam, bold, italic = find_font(family, 0)
            self.open_editor(i, cx, cy, family=fam, bold=bold, italic=italic,
                             commit=lambda text, p=None: None)
            ed = self.editor
            ed["commit"] = lambda text: self.app.add_real_text(i, ed["point"], text)
            ed["kind"] = "add"  # (the third toolbar's changes show in it as it's typed)
            ed["box"].bind("<KeyRelease>", lambda e: ed["box"].tag_add("align", "1.0", "end"),
                           add="+")
            self.app.restyle_editor()
        elif tool == "eraser" and self.app.variant["eraser"] == "rub":
            self.app.begin_rub(i)
            self.press = ("rub", i, [(cx, cy)], 0)
            self.rub_start(i)
            self.rub_paint([(cx, cy)])
            self.rub_now()
        elif tool in self.BOX_TOOLS:
            self.press = ("box", tool, i, cx, cy)
        elif tool == "sign":
            self.app.place_signature(i, self.to_pdf(i, cx, cy))
        elif tool == "pen":
            self.press = ("pen", i, [(cx, cy)])
        elif tool == "highlight":  # the marker: drawn freely
            self.press = ("marker", i, [(cx, cy)])

    def press_annot(self, i, found, cx, cy):
        """An annotation clicked: picked up - and dragged about, if it can be moved."""
        xref, kind, rect = found
        self.select((i, xref))
        if kind in MOVABLE:
            self.press = ("move", i, xref, cx, cy, self.to_canvas(i, rect))

    def word_at(self, i, cx, cy):
        """Is there a letter under this canvas point on page i?"""
        p = self.to_pdf(i, cx, cy)
        return any(ch[0] <= p.x <= ch[2] and ch[1] <= p.y <= ch[3] and not ch[4].isspace()
                   for ch in self.app.chars_of(i))

    # ---- selected text, and the toolbar over it ----
    def set_text_sel(self, i, boxes, text):
        app = self.app
        self.close_bar()
        self.text_sel = {"page": i, "boxes": boxes, "text": text}
        icons = app.icons
        items = [(icons["copy"], "Copy", app.copy_selection, None), None,
                 (pixel_icon(marked_rows("highlight"), {"Y": hex_rgb_255(app.color("highlight"))}),
                  "Highlight", lambda: app.mark_text("highlight"), None),
                 (icons["mark_strike"], "Strike out", lambda: app.mark_text("strike"), None),
                 (icons["mark_underline"], "Underline", lambda: app.mark_text("underline"), None),
                 (icons["mark_squiggly"], "Squiggly underline", lambda: app.mark_text("squiggly"),
                  None), None,
                 (icons["edit"], "Edit text", app.edit_selection, "Edit text")]
        if app.marks_over(i, boxes):  # (marked already: the mark can be taken off these letters)
            items += [None, (icons["eraser"], "Remove the highlight (or other mark) from the "
                             "selected letters", app.unmark_selection, "Remove")]
        self.text_bar = TextBar(self, "text", i, boxes, items)
        self.draw_overlays()
        n = len(text.replace("\n", ""))
        self.app.say(f"{n} letter{'s' if n != 1 else ''} selected: copy (Ctrl + C), mark or "
                     f"edit {'them' if n != 1 else 'it'} with the little toolbar.")

    def show_mark_bar(self, i, xref):
        """A highlight (or other text mark) clicked: picked up, with its toolbar."""
        app = self.app
        self.select((i, xref))
        pieces = self.annot_pieces(i, xref)
        if not pieces:
            return
        items = [(app.icons["copy"], "Copy its text", lambda: app.copy_mark_text(i, xref), None),
                 None,
                 (swatch_icon(app.mark_color(i, xref)), "Colour", lambda: app.recolor_mark(i, xref),
                  None), None,
                 (app.icons["eraser"], "Remove (Delete)", app.delete_annot, "Remove")]
        self.text_bar = TextBar(self, "annot", i, pieces, items)
        self.place_text_bar()
        app.say("Picked up: copy its text, change its colour, or remove it (Delete).")

    def close_bar(self):
        if self.text_bar is not None:
            self.text_bar.destroy()
            self.text_bar = None

    def drop_bars(self):
        """The text selected let go, and any floating toolbar closed - a highlight's too
        (the page changed, or another tool was taken up)."""
        self.clear_text_sel()
        self.close_bar()

    def clear_text_sel(self):
        if self.text_bar is not None and self.text_bar.kind == "text":
            self.close_bar()
        if self.text_sel is not None:
            self.text_sel = None
            self.draw_overlays()

    def place_text_bar(self):
        """The floating toolbar just over the selection (under it when there's no room
        above), hidden while the selection is scrolled out of view."""
        bar = self.text_bar
        if bar is None or bar.page >= len(self.rects):
            return
        c = self.canvas
        boxes = [self.to_canvas(bar.page, r) for r in bar.rects]
        left, top = c.canvasx(0), c.canvasy(0)
        vw, vh = c.winfo_width(), c.winfo_height()
        x0 = boxes[0][0] - left
        y_top = min(b[1] for b in boxes) - top
        y_bottom = max(b[3] for b in boxes) - top
        bw, bh = bar.winfo_reqwidth(), bar.winfo_reqheight()
        y = y_top - bh - 6 if y_top - bh - 6 >= 0 else y_bottom + 6
        if y_bottom < 0 or y_top > vh or y + bh > vh + bh // 2:
            bar.withdraw()
            return
        x = min(max(0, x0), max(0, vw - bw))
        bar.geometry(f"+{c.winfo_rootx() + round(x)}+{c.winfo_rooty() + round(y)}")
        if bar.state() != "normal":
            bar.deiconify()

    def on_drag(self, e):
        p = self.press
        if not p:
            return
        c = self.canvas
        cx, cy = c.canvasx(e.x), c.canvasy(e.y)
        if p[0] == "pan":
            c.scan_dragto(e.x, e.y, gain=1)
        elif p[0] == "box":
            _, tool, i, sx, sy = p
            c.delete("ghost")
            kind = self.app.variant.get(tool)
            color = untranslated(self.app.color(tool) if tool in self.app.tool_colors
                                 else "#000000", "edge")
            width = max(1, self.app.size("shape") * self.scale) if tool == "shape" else 1
            if tool == "shape" and kind in ("line", "arrow"):
                c.create_line(sx, sy, cx, cy, fill=color, width=width,
                              arrow="last" if kind == "arrow" else "none",
                              tags=("ghost", "overlay"))
            elif tool == "shape" and kind == "ellipse":
                c.create_oval(sx, sy, cx, cy, outline=color, width=width, tags=("ghost", "overlay"))
            elif tool == "shape":
                c.create_rectangle(sx, sy, cx, cy, outline=color, width=width,
                                   tags=("ghost", "overlay"))
            elif tool == "eraser" and kind == "whiteout":  # (the colour it'll cover with)
                c.create_rectangle(sx, sy, cx, cy, fill=untranslated(self.app.color("eraser"), "box"),
                                   outline="#000000", dash=(1, 1), tags=("ghost", "overlay"))
            else:  # erase, a link, a picture's place: a dotted box
                c.create_rectangle(sx, sy, cx, cy, outline="#000000", dash=(2, 2),
                                   tags=("ghost", "overlay"))
        elif p[0] == "resize":  # from the opposite corner, keeping the picture's shape
            _, i, obj, h, (x0, y0, x1, y1) = p
            ax, ay = (x1 if h in (0, 2) else x0), (y1 if h in (0, 1) else y0)
            w0, h0 = max(1, x1 - x0), max(1, y1 - y0)
            k = max(abs(cx - ax) / w0, abs(cy - ay) / h0, 0.05)
            nx = ax + (w0 * k if (h in (1, 3)) else -w0 * k)
            ny = ay + (h0 * k if (h in (2, 3)) else -h0 * k)
            self._ghost = (min(ax, nx), min(ay, ny), max(ax, nx), max(ay, ny))
            c.delete("ghost")
            c.create_rectangle(*self._ghost, outline="#000000", dash=(1, 1), tags=("ghost", "overlay"))
        elif p[0] == "reflow":  # a frame's handle: its side (or corner) wider / narrower
            _, i, obj, h, (x0, y0, x1, y1) = p
            least = 3 * line_size(obj) * self.scale
            tall = 1.2 * line_size(obj) * self.scale  # (at least a line high)
            if "w" in h:
                gx0, gx1 = min(cx, x1 - least), x1
            else:
                gx0, gx1 = x0, max(cx, x0 + least)
            gy0, gy1 = y0, y1  # (a corner: its top or bottom too)
            if h.startswith("n"):
                gy0 = min(cy, y1 - tall)
            elif h.startswith("s"):
                gy1 = max(cy, y0 + tall)
            self._ghost = (gx0, gx1, gy0, gy1)
            c.delete("ghost")
            c.create_rectangle(gx0, gy0, gx1, gy1, outline="#000000", dash=(1, 1),
                               tags=("ghost", "overlay"))
        elif p[0] == "stretch":  # the line's round handle: from its other end
            _, i, obj, h, (x0, y0, x1, y1) = p
            w0 = max(1, x1 - x0)
            k = max(0.2, ((cx - x0) if h == 5 else (x1 - cx)) / w0)
            nw, nh = w0 * k, (y1 - y0) * k
            gx0, gx1 = (x0, x0 + nw) if h == 5 else (x1 - nw, x1)
            self._ghost = k
            c.delete("ghost")
            c.create_rectangle(gx0, y1 - nh, gx1, y1, outline="#000000", dash=(1, 1),
                               tags=("ghost", "overlay"))
        elif p[0] == "select":
            _, i, sx, sy = p[:4]
            boxes, _text = self.app.pick_letters(i, self.to_pdf(i, sx, sy), self.to_pdf(i, cx, cy))
            self.text_sel = {"page": i, "boxes": boxes, "text": ""} if boxes else None
            self.draw_overlays()
        elif p[0] in ("move", "shift"):
            _, i, xref, sx, sy, (x0, y0, x1, y1) = p[:6]
            dx, dy = cx - sx, cy - sy
            c.delete("ghost")
            c.create_rectangle(x0 + dx, y0 + dy, x1 + dx, y1 + dy, outline="#000000",
                               dash=(1, 1), tags=("ghost", "overlay"))
        elif p[0] == "rub":
            if abs(cx - p[2][-1][0]) + abs(cy - p[2][-1][1]) >= 1:
                self.rub_paint([p[2][-1], (cx, cy)])
                p[2].append((cx, cy))
                self.rub_later()
            self.eraser_ring(cx, cy)
        elif p[0] == "pen":  # (shown smooth and soft-edged, as it'll be put in)
            pts = p[2]
            lx, ly = pts[-1]
            if abs(cx - lx) + abs(cy - ly) >= 1:
                pts.append((cx, cy))
                self.ink_later("pen", pts)
        elif p[0] == "marker":  # (Shift: straight from where it started)
            pts = p[2]
            if e.state & 1:
                del pts[1:]
            if abs(cx - pts[-1][0]) + abs(cy - pts[-1][1]) >= 1 or e.state & 1:
                pts.append((cx, cy))
            self.ink_later("marker", pts)

    def on_release(self, e):
        p, self.press = self.press, None
        c = self.canvas
        c.delete("ghost")
        if not p:
            return
        cx, cy = c.canvasx(e.x), c.canvasy(e.y)
        if p[0] == "pan":
            if len(p) == 4 and abs(e.x - p[2]) + abs(e.y - p[3]) < 3:
                self.app.follow_link(p[1])  # (a click on a link, not a drag)
            self.on_motion(e)
        elif p[0] == "box":
            _, tool, i, sx, sy = p
            a, b = self.to_pdf(i, sx, sy), self.to_pdf(i, cx, cy)
            dragged = abs(cx - sx) + abs(cy - sy) >= 4
            if tool == "image":
                self.app.add_picture(i, a if not dragged else None,
                                     rect=pymupdf.Rect(a, b).normalize() if dragged else None,
                                     clipboard=self.app.variant["image"] == "clipboard")
            elif not dragged:
                return
            elif tool == "eraser":
                self.app.erase_area(i, pymupdf.Rect(a, b).normalize())
            elif tool == "shape":
                self.app.add_shape(i, a, b)
            elif tool == "link":
                self.app.new_link(i, pymupdf.Rect(a, b).normalize())
        elif p[0] == "select":
            _, i, sx, sy, mark, link, ex, ey = p
            if abs(e.x - ex) + abs(e.y - ey) < 3:  # a click: a mark picked up, a link followed
                self.clear_text_sel()
                if mark:
                    self.show_mark_bar(i, mark)
                elif link:
                    self.app.follow_link(link)
                return
            boxes, text = self.app.pick_letters(i, self.to_pdf(i, sx, sy), self.to_pdf(i, cx, cy))
            if boxes:
                self.set_text_sel(i, boxes, text)
            else:
                self.clear_text_sel()
        elif p[0] == "reflow":
            _, i, obj, h, (x0, y0, x1, y1) = p
            g, self._ghost = self._ghost, None
            if g and (abs(g[0] - x0) > 2 or abs(g[1] - x1) > 2 or abs(g[2] - y0) > 2
                      or abs(g[3] - y1) > 2):
                g = [o if abs(n - o) <= 4 else n for n, o in zip(g, (x0, x1, y0, y1))]  # (a
                a, b = self.to_pdf(i, g[0], g[2]), self.to_pdf(i, g[1], g[3])  # side not moved
                self.app.resize_frame(i, obj, pymupdf.Rect(a, b).normalize())  # stays put)
        elif p[0] == "stretch":
            _, i, obj, h, _box = p
            k, self._ghost = self._ghost, None
            if k and abs(k - 1) > 0.02:
                self.app.scale_text(i, obj, k, right=h == 4)
        elif p[0] == "shift":  # the Edit tool: dragged - moved; the line picked up clicked
            _, i, obj, sx, sy, _box, again = p  # again - retype it
            bx0, by0, bx1, by1 = self.to_canvas(i, obj["rect"])
            dx, dy = cx - sx, cy - sy
            j = self.page_at((bx0 + bx1) / 2 + dx, (by0 + by1) / 2 + dy, slack=self.GAP)
            if obj["kind"] == "image" and abs(dx) + abs(dy) >= 3 and j is not None and j != i:
                a = self.to_pdf(j, bx0 + dx, by0 + dy)  # (its middle dropped on another page:
                b = self.to_pdf(j, bx1 + dx, by1 + dy)  # moved there)
                self.app.move_image_to_page(i, obj, j, pymupdf.Rect(a, b).normalize())
            elif abs(cx - sx) + abs(cy - sy) >= 3:
                page = self.app.doc[i]
                d = (pymupdf.Point(cx - sx, cy - sy) / self.scale) * page.derotation_matrix - \
                    pymupdf.Point(0, 0) * page.derotation_matrix
                self.app.move_object(i, obj, d)
            elif obj["kind"] == "text" and again:
                self.edit_line(i, obj)
        elif p[0] == "resize":
            _, i, obj, h, _box = p
            g, self._ghost = self._ghost, None
            if g:
                rect = pymupdf.Rect(self.to_pdf(i, g[0], g[1]), self.to_pdf(i, g[2], g[3]))
                self.app.resize_image(i, obj, rect.normalize())
        elif p[0] == "move":
            _, i, xref, sx, sy, _box = p
            if abs(cx - sx) + abs(cy - sy) >= 3:
                page = self.app.doc[i]
                d = (pymupdf.Point(cx - sx, cy - sy) / self.scale) * page.derotation_matrix - \
                    pymupdf.Point(0, 0) * page.derotation_matrix
                self.app.move_annot(i, xref, d)
        elif p[0] == "rub":
            job = getattr(self, "_rub_job", None)
            if job:
                self.after_cancel(job)
            self.press = p  # (the last of it rubbed out, then it's one change)
            self.rub_now()
            self.press = None
            self.rub_done(self.app.end_rub())
        elif p[0] == "pen":
            _, i, pts = p
            self.ink_done()
            if len(pts) > 1:
                self.app.add_ink(i, [self.to_pdf(i, x, y) for x, y in smooth_stroke(pts)])
        elif p[0] == "marker":
            _, i, pts = p
            self.ink_done()
            if len(pts) == 1:  # (a click: a dot)
                pts.append((pts[0][0] + 0.5, pts[0][1]))
            line = smooth_stroke(pts) if len(pts) > 2 else pts
            self.app.add_marker(i, [self.to_pdf(i, x, y) for x, y in line])

    # ---- the stroke being drawn, shown the way ink programs do (Snipping Tool...) ----
    INK_SS = 3  # (drawn this many times bigger, then made smaller: soft, even edges)

    def ink_later(self, kind, pts):
        """The stroke shown again - once, after the mouse moves waiting are all in (a fast
        hand sends more than can be drawn one by one)."""
        self._ink = (kind, pts)
        if not getattr(self, "_ink_job", None):
            self._ink_job = self.after_idle(self.draw_ink)

    def draw_ink(self):
        """The stroke as it's being drawn: smoothed like it'll be put in, anti-aliased (drawn
        bigger, then shrunk), in its real colour and see-through - one picture over the
        part of the page it covers."""
        self._ink_job = None
        if not getattr(self, "_ink", None):
            return
        kind, pts = self._ink
        c, app = self.canvas, self.app
        line = smooth_stroke(pts, rounds=2) if len(pts) > 2 else list(pts)
        if len(line) == 1:
            line = line * 2
        if kind == "pen":
            rgb, width, alpha = hex_rgb_255(app.color("pen")), app.size("pen"), app.opacity("pen")
        else:
            rgb, width, alpha = (hex_rgb_255(app.color("highlight")), app.size("highlight"),
                                 app.opacity("marker"))
        w = max(1.0, width * self.scale)
        left, top = c.canvasx(0), c.canvasy(0)
        right, bottom = left + c.winfo_width(), top + c.winfo_height()
        x0 = max(left, min(x for x, _ in line) - w); y0 = max(top, min(y for _, y in line) - w)
        x1 = min(right, max(x for x, _ in line) + w); y1 = min(bottom, max(y for _, y in line) + w)
        if x1 - x0 < 1 or y1 - y0 < 1:
            return
        x0, y0 = int(x0), int(y0)
        size = (int(x1 - x0) + 2, int(y1 - y0) + 2)
        ss = self.INK_SS if size[0] * size[1] < 400000 else 2
        big = Image.new("L", (size[0] * ss, size[1] * ss), 0)
        d = ImageDraw.Draw(big)
        scaled = [((x - x0) * ss, (y - y0) * ss) for x, y in line]
        d.line(scaled, fill=255, width=max(1, round(w * ss)))
        r = w * ss / 2
        if r > 1:  # (a round brush at every point: no slivers where the pieces meet, round ends)
            for x, y in scaled:
                d.ellipse((x - r, y - r, x + r, y + r), fill=255)
        mask = big.resize(size, Image.BOX)
        if alpha < 1:
            mask = mask.point(lambda v: round(v * alpha))
        img = Image.new("RGBA", size, rgb + (0,))
        img.putalpha(mask)
        self._ink_photo = ImageTk.PhotoImage(img, master=c)
        item = getattr(self, "_ink_item", None)
        if item and c.find_withtag(item):
            c.itemconfigure(item, image=self._ink_photo)
            c.coords(item, x0, y0)
        else:
            self._ink_item = c.create_image(x0, y0, image=self._ink_photo, anchor="nw",
                                            tags=("ink", "overlay"))

    def ink_done(self):
        """The mouse let go: the stroke as drawn stays shown until the page is drawn again
        with it in (no flicker in between) - it goes with the pinch previews' picture."""
        job = getattr(self, "_ink_job", None)
        if job:
            self.after_cancel(job)
            self._ink_job = None
            self.draw_ink()
        item = getattr(self, "_ink_item", None)
        if item and self.canvas.find_withtag(item):
            self.canvas.itemconfigure(item, tags=("preview", "pic"))
            self._preview_photos.append(self._ink_photo)
        self._ink, self._ink_item = None, None

    def drawing_cursors(self, colors):
        """The pencil and marker pointers, in the colours the tools draw in (made again when
        one changes - the same pictures as their buttons)."""
        c = self.canvas
        now = str(c.cget("cursor"))
        for tool, kind in (("pen", "pen"), ("highlight", "marker")):
            path = cursor_file(kind, colors.get(tool) if kind == "marker" else None)
            if path:
                try:
                    c.config(cursor="@" + path)  # (made sure Tk takes it)
                    self.CURSORS[tool] = "@" + path
                except tk.TclError:
                    pass
        c.config(cursor=self.CURSORS.get(self.app.tool, now) if now.startswith("@") else now)

    # ---- the eraser rubbing (the drawings under it go as it moves) ----
    def rub_later(self):
        if not getattr(self, "_rub_job", None):
            self._rub_job = self.after(30, self.rub_now)

    def rub_now(self):
        """What the eraser went over since last time, rubbed out (a few times a second - a
        fast hand sends more moves than can be done one by one)."""
        self._rub_job = None
        p = self.press
        if not p or p[0] != "rub":
            return
        _, i, pts, done = p
        part = pts[max(0, done - 1):]
        self.press = ("rub", i, pts, len(pts))
        r = self.app.size("eraser") / 2
        self.app.rub(i, [self.to_pdf(i, x, y) for x, y in part], r)

    def eraser_ring(self, cx, cy):
        """The eraser's pointer: a ring its size, where it rubs (black and white - seen on
        anything), as Photoshop's."""
        c = self.canvas
        r = max(2, self.app.size("eraser") * self.scale / 2)
        c.delete("eraserring")
        c.create_oval(cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, outline="#FFFFFF",
                      tags=("eraserring", "overlay"))
        c.create_oval(cx - r, cy - r, cx + r, cy + r, outline="#000000",
                      tags=("eraserring", "overlay"))
        if r > 6:  # (its middle)
            c.create_line(cx - 2, cy, cx + 3, cy, fill="#000000", tags=("eraserring", "overlay"))
            c.create_line(cx, cy - 2, cx, cy + 3, fill="#000000", tags=("eraserring", "overlay"))

    RUB_CELL = 128  # (the rubbed-out look is shown in squares this big: only those it touches redone)

    def rub_start(self, i):
        """The eraser pressed: the part of page i in view drawn as it'd be without the drawings
        the eraser takes - shown through wherever it rubs, at once."""
        self._rubview = None
        c = self.canvas
        x, y, w, h = self.rects[i]
        left, top = c.canvasx(0), c.canvasy(0)
        x0, y0 = int(max(x, left)), int(max(y, top))
        x1 = int(math.ceil(min(x + w, left + c.winfo_width())))
        y1 = int(math.ceil(min(y + h, top + c.winfo_height())))
        if x1 - x0 < 1 or y1 - y0 < 1:
            return
        with PDF_LOCK:
            page = self.app.doc[i]
            hide = [(a, a.flags) for a in page.annots() if a.type[1] in RUBBABLE]
            if not hide:
                return
            for a, flags in hide:  # (hidden just for this picture)
                a.set_flags(flags | pymupdf.PDF_ANNOT_IS_HIDDEN)
            try:
                clean = tile_picture(page, self.scale, getattr(self, "stretch", 1.0),
                                     (x0 - x, y0 - y, x1 - x, y1 - y), (w, h))
            finally:
                for a, flags in hide:
                    a.set_flags(flags)
        self._rubview = {"at": (x0, y0), "clean": clean.convert("RGBA"),
                         "mask": Image.new("L", clean.size, 0), "cells": {}}

    def rub_paint(self, line):
        """Where the eraser went (canvas points): rubbed out on the screen - a round brush,
        smooth edged."""
        v = getattr(self, "_rubview", None)
        if not v:
            return
        c = self.canvas
        ox, oy = v["at"]
        mask = v["mask"]
        r = max(0.5, self.app.size("eraser") * self.scale / 2)
        xs, ys = [x - ox for x, _ in line], [y - oy for _, y in line]
        bx0, by0 = max(0, int(min(xs) - r - 2)), max(0, int(min(ys) - r - 2))
        bx1, by1 = min(mask.width, int(max(xs) + r + 3)), min(mask.height, int(max(ys) + r + 3))
        if bx1 <= bx0 or by1 <= by0:
            return
        ss = 4 if (bx1 - bx0) * (by1 - by0) < 250000 else 2
        big = Image.new("L", ((bx1 - bx0) * ss, (by1 - by0) * ss), 0)
        d = ImageDraw.Draw(big)
        pts = [((x - bx0) * ss, (y - by0) * ss) for x, y in zip(xs, ys)]
        if len(pts) > 1:
            d.line(pts, fill=255, width=max(1, round(2 * r * ss)))
        for x, y in pts:
            d.ellipse((x - r * ss, y - r * ss, x + r * ss, y + r * ss), fill=255)
        patch = big.resize((bx1 - bx0, by1 - by0), Image.BOX)
        box = (bx0, by0, bx1, by1)
        mask.paste(ImageChops.lighter(mask.crop(box), patch), box)
        n = self.RUB_CELL
        for gy in range(by0 // n, (by1 - 1) // n + 1):
            for gx in range(bx0 // n, (bx1 - 1) // n + 1):
                cell = (gx * n, gy * n, min(mask.width, (gx + 1) * n), min(mask.height, (gy + 1) * n))
                img = v["clean"].crop(cell)
                img.putalpha(mask.crop(cell))
                photo = ImageTk.PhotoImage(img, master=c)
                old = v["cells"].get((gx, gy))
                if old and c.find_withtag(old[1]):
                    c.itemconfigure(old[1], image=photo)
                    v["cells"][(gx, gy)] = (photo, old[1])
                else:
                    item = c.create_image(ox + cell[0], oy + cell[1], image=photo, anchor="nw",
                                          tags=("rubbed", "overlay"))
                    v["cells"][(gx, gy)] = (photo, item)
        c.tag_raise("eraserring")

    def rub_done(self, changed):
        """The eraser let go: the rubbed-out look stays until the page is drawn again without
        what it took (no flicker) - or, nothing taken, goes now."""
        v, self._rubview = getattr(self, "_rubview", None), None
        if not v:
            return
        if not changed:
            self.canvas.delete("rubbed")
            return
        for photo, item in v["cells"].values():
            self.canvas.itemconfigure(item, tags=("preview", "pic"))
            self._preview_photos.append(photo)

    def on_double(self, e):
        if not self.rects:
            return
        if self.app.tool not in ("move", "select"):  # (a quick second click is a click too:
            self.on_press(e)  # the Edit tool's line clicked again is retyped)
            return
        cx, cy = self.canvas.canvasx(e.x), self.canvas.canvasy(e.y)
        i = self.page_at(cx, cy)
        found = self.annot_at(i, cx, cy, skip=MARKUPS) if i is not None else None
        if found is not None and (found[1] in MOVABLE or not self.word_at(i, cx, cy)):
            self.press = None
            self.app.edit_annot(i, found[0])
        elif i is not None and self.app.tool == "select" and self.word_at(i, cx, cy):
            self.press = None  # a word: selected
            self.select_around(i, self.to_pdf(i, cx, cy), "word")

    def on_triple(self, e):
        if not self.rects:
            return
        if self.app.tool not in ("move", "select"):
            self.on_press(e)
            return
        cx, cy = self.canvas.canvasx(e.x), self.canvas.canvasy(e.y)
        i = self.page_at(cx, cy)
        if i is not None and self.app.tool == "select" and self.word_at(i, cx, cy):
            self.press = None  # the whole line: selected
            self.select_around(i, self.to_pdf(i, cx, cy), "line")

    def select_around(self, i, point, what):
        """The word (or line) at a point selected."""
        chars = self.app.chars_of(i)
        k = next((n for n, ch in enumerate(chars) if ch[0] <= point.x <= ch[2]
                  and ch[1] <= point.y <= ch[3] and not ch[4].isspace()), None)
        if k is None:
            return
        same = (lambda n: chars[n][5] == chars[k][5] and not chars[n][4].isspace()) if what == "word" \
            else (lambda n: chars[n][5] == chars[k][5])
        lo, hi = k, k + 1
        while lo > 0 and same(lo - 1):
            lo -= 1
        while hi < len(chars) and same(hi):
            hi += 1
        boxes, text = self.app.letters(i, lo, hi)
        if boxes:
            self.set_text_sel(i, boxes, text)

    def on_right_click(self, e):
        if not self.rects:
            return
        cx, cy = self.canvas.canvasx(e.x), self.canvas.canvasy(e.y)
        i = self.page_at(cx, cy)
        if i is None:
            return
        if self.app.tool == "edit":
            self.edit_menu(i, cx, cy, e.x_root, e.y_root)
            return
        ts = self.text_sel
        if ts and ts["page"] == i and any(
                x0 <= cx <= x1 and y0 <= cy <= y1
                for x0, y0, x1, y1 in (self.to_canvas(i, r) for r in ts["boxes"])):
            menu = PopupMenu(self)
            menu.add_command(label="Copy", command=self.app.copy_selection)
            menu.add_separator()
            for kind, label in (("highlight", "Highlight"), ("strike", "Strike out"),
                                ("underline", "Underline"), ("squiggly", "Squiggly underline")):
                menu.add_command(label=label, command=lambda k=kind: self.app.mark_text(k))
            menu.add_separator()
            menu.add_command(label="Edit text", command=self.app.edit_selection)
            menu.tk_popup(e.x_root, e.y_root)
            return
        if self.app.tool == "link":
            link = self.link_at(i, cx, cy)
            if link:
                menu = PopupMenu(self)
                menu.add_command(label="Change link...", command=lambda: self.app.edit_link(i, link))
                menu.add_command(label="Go there", command=lambda: self.app.follow_link(link))
                menu.add_separator()
                menu.add_command(label="Delete link", command=lambda: self.app.delete_link(i, link))
                menu.tk_popup(e.x_root, e.y_root)
            return
        found = self.annot_at(i, cx, cy)
        menu = PopupMenu(self)
        if found is not None:
            xref, kind, _rect = found
            self.select((i, xref))
            if kind in ("FreeText", "Text"):
                menu.add_command(label="Edit text" if kind == "FreeText" else "Edit note",
                                 command=lambda: self.app.edit_annot(i, xref))
            menu.add_command(label="Delete", command=self.app.delete_annot)
        else:
            self.app.set_current(i, scroll=False)
            menu.add_command(label="Rotate page left", command=lambda: self.app.rotate_pages([i], -90))
            menu.add_command(label="Rotate page right", command=lambda: self.app.rotate_pages([i], 90))
            menu.add_separator()
            menu.add_command(label="Delete page", command=lambda: self.app.delete_pages([i]))
        menu.tk_popup(e.x_root, e.y_root)

    # ---- the Edit tool: the PDF's own text and pictures ----
    @staticmethod
    def handles(x0, y0, x1, y1):
        return ((x0, y0), (x1, y0), (x0, y1), (x1, y1))  # top left, top right, bottom...

    @staticmethod
    def text_handles(x0, y0, x1, y1):
        return ((x0 - 2, (y0 + y1) / 2), (x1 + 2, (y0 + y1) / 2))  # left end, right end

    @staticmethod
    def frame_handles(x0, y0, x1, y1):
        """A frame's handles: its four corners and the middle of its sides - (which, x, y)."""
        m = (y0 + y1) / 2
        return (("nw", x0 - 2, y0 - 2), ("ne", x1 + 2, y0 - 2), ("w", x0 - 2, m), ("e", x1 + 2, m),
                ("sw", x0 - 2, y1 + 2), ("se", x1 + 2, y1 + 2))

    def handle_at(self, cx, cy):
        """Which handle of what's picked up is at (cx, cy): a picture's corner, 0 to 3; a
        line's left end 4, its right end 5 - or None."""
        sel = self.edit_sel
        if not sel or sel[0] >= len(self.rects) or self.editor:
            return None
        box = self.to_canvas(sel[0], sel[1].get("box") or sel[1]["rect"])
        if sel[1].get("frame"):
            for code, hx, hy in self.frame_handles(*box):
                if abs(cx - hx) <= 5 and abs(cy - hy) <= 5:
                    return code
            return None
        if sel[1]["kind"] == "text":
            spots = enumerate(self.text_handles(*box), start=4)
        else:
            spots = enumerate(self.handles(*box))
        for n, (hx, hy) in spots:
            if abs(cx - hx) <= 5 and abs(cy - hy) <= 5:
                return n
        return None

    def select_object(self, sel):
        self.edit_sel = sel
        self.draw_overlays()
        self.app.edit_selected()

    def reselect(self, i, rect, kind):
        """After a change: pick up again what was changed (found by where it is now)."""
        objs = [o for o in self.app.objects_on(i) if o["kind"] == kind] if self.app.doc else []
        best = min(objs, key=lambda o: abs(o["rect"].x0 - rect.x0) + abs(o["rect"].y0 - rect.y0),
                   default=None)
        ok = best is not None and abs(best["rect"].x0 - rect.x0) + abs(best["rect"].y0 - rect.y0) < 40
        self.hover = None
        self.select_object((i, best) if ok else None)

    def edit_line(self, i, obj):
        """Retype a line of the PDF's text in a box over it, each letter in its own font,
        size, colour and marks (the line itself is hidden meanwhile). Selected letters can be
        given another look with the third toolbar; with none selected it's the whole line."""
        span = main_span(obj)
        self.app.wait_fonts()
        _path, family, bold, italic = find_font(span["font"], span["flags"])
        x0, y0, x1, y1 = self.to_canvas(i, obj["rect"])
        px = max(6, round(span["size"] * self.scale))
        runs = self.app.line_runs(i, obj)
        self.open_editor(i, x0 + 1, y0 + px // 2 + 1, "", span["size"], span_color(span),
                         family=family, bold=bold, italic=italic,
                         commit=lambda new: self.app.replace_runs(
                             i, obj, new, ed.get("align"), ed.get("dir")))
        ed = self.editor
        box = ed["box"]
        box.config(exportselection=False)  # (the font list taking the keyboard keeps it)
        ed.update(kind="line", rich=True, styles={}, default=runs[0][1] if runs else None,
                  align=None, dir=None)  # (lining up / direction chosen in it)
        if obj.get("frame"):  # (a frame: the box as wide as it, wrapping like it)
            box.config(wrap="word")
            if obj.get("box"):
                x0, _y, x1, _y1 = self.to_canvas(i, obj["box"])
            span_x = cell_span(obj, self.app.line_alignment(i, obj))
            if span_x:
                x0, x1 = self.to_canvas(i, pymupdf.Rect(span_x[0], obj["rect"].y0, span_x[1],
                                                        obj["rect"].y1))[::2]
            ed["frame"], ed["wrap"] = True, round(x1 - x0) + 6
        for text, st in runs:
            box.insert("end", text, self.style_tag(st))
        ed["was"] = self.editor_runs(ed)
        box.edit_reset()
        box.mark_set("insert", "end")

        def typed(_=None):  # (letters typed take the look of the letter before them)
            self.retag_typed(ed)
            self.app.refresh_props()
        box.bind("<KeyRelease>", typed, add="+")
        box.bind("<ButtonRelease-1>", lambda e: self.app.refresh_props(), add="+")
        box.bind("<<Selection>>", lambda e: self.app.refresh_props(), add="+")
        self.retag_typed(ed)
        ed["grow"]()
        self.canvas.create_rectangle(x0 - 1, y0 - 1, x1 + 1, y1 + 1, fill="#FFFFFE", outline="",
                                     tags="editcover")
        self.canvas.tag_raise("editor")
        self.draw_overlays()  # (the line's box and handles go while it's being typed)

    def style_tag(self, st):
        """The box's tag showing a look (made once per look)."""
        ed = self.editor
        if st in ed["styles"]:
            return ed["styles"][st]
        name = f"st{len(ed['styles'])}"
        family, size, bold, italic, color, under, strike, mark = st
        px = max(6, round(size * self.scale))
        ed["box"].tag_configure(
            name, font=(family, -px) + (("bold",) if bold else ()) + (("italic",) if italic else ())
            + (("underline",) if under else ()) + (("overstrike",) if strike else ()),
            foreground=untranslated(color, "text"),
            background=untranslated(mark, "box") if mark else "")
        ed["box"].tag_raise("sel")
        ed["styles"][st] = name
        ed.setdefault("tags", {})[name] = st
        return name

    def style_at(self, ed, index):
        """The look of the letter at a box index (or the one before it, or the line's)."""
        box = ed["box"]
        for idx in (index, f"{index}-1c"):
            for tag in box.tag_names(idx):
                if tag in ed.get("tags", {}):
                    return ed["tags"][tag]
        return ed["default"]

    def retag_typed(self, ed):
        """Letters typed without a look take the one of the letter before them."""
        box = ed["box"]
        n = len(box.get("1.0", "end-1c"))
        last = ed["default"]
        for k in range(n):
            idx = f"1.0+{k}c"
            own = [tag for tag in box.tag_names(idx) if tag in ed.get("tags", {})]
            if own:
                last = ed["tags"][own[0]]
            elif last is not None:
                box.tag_add(self.style_tag(last), idx)

    def editor_runs(self, ed):
        """The box's text as pieces in one look each: [(text, look)] (one line)."""
        box = ed["box"]
        text = box.get("1.0", "end-1c")
        runs, last = [], ed["default"]
        for k, ch in enumerate(text):
            own = [tag for tag in box.tag_names(f"1.0+{k}c") if tag in ed.get("tags", {})]
            st = ed["tags"][own[0]] if own else last
            ch = " " if ch == "\n" and not ed.get("frame") else ch  # (a frame's: paragraphs)
            if runs and runs[-1][1] == st:
                runs[-1][0] += ch
            else:
                runs.append([ch, st])
            last = st
        while runs and not runs[0][0].strip():
            runs.pop(0)
        while runs and not runs[-1][0].strip():
            runs.pop()
        if runs:
            runs[0][0] = runs[0][0].lstrip()
            runs[-1][0] = runs[-1][0].rstrip()
        return [(text, st) for text, st in runs]

    def restyle_range(self, change):
        """The selected letters (none selected: all of them) given a new look: change(look)
        gives each one's new look. The selection stays."""
        ed = self.editor
        box = ed["box"]
        sel = box.tag_ranges("sel")
        a, b = (str(sel[0]), str(sel[1])) if sel else ("1.0", "end-1c")
        n = len(box.get(a, b))
        for k in range(n):
            idx = f"{a}+{k}c"
            st = self.style_at(ed, idx)
            if st is None:
                continue
            for tag in box.tag_names(idx):
                if tag in ed["tags"]:
                    box.tag_remove(tag, idx)
            box.tag_add(self.style_tag(change(st)), idx)
        ed["grow"]()  # (its size follows the text)
        box.focus_set()

    def edit_menu(self, i, cx, cy, x, y):
        """Right-click with the Edit tool."""
        obj = self.app.object_at(i, self.to_pdf(i, cx, cy))
        menu = PopupMenu(self)
        if obj:
            self.select_object((i, obj))
            if obj["kind"] == "text":
                menu.add_command(label="Edit text", command=lambda: self.edit_line(i, obj))
            else:
                menu.add_command(label="Replace picture...",
                                 command=lambda: self.app.replace_picture(i, obj))
                menu.add_command(label="Save picture as...",
                                 command=lambda: self.app.save_picture(obj))
            menu.add_separator()
            menu.add_command(label="Copy", command=lambda: self.app.copy_object((i, obj)))
            if getattr(self.app, "_copied", None):
                point = self.to_pdf(i, cx, cy)
                menu.add_command(label="Paste", command=lambda: self.app.paste_object((i, point)))
            menu.add_separator()
            menu.add_command(label="Delete", command=self.app.delete_object)
        else:
            point = self.to_pdf(i, cx, cy)
            if getattr(self.app, "_copied", None):
                menu.add_command(label="Paste here", command=lambda: self.app.paste_object((i, point)))
                menu.add_separator()
            menu.add_command(label="Add picture here...",
                             command=lambda: self.app.add_picture(i, point))
        menu.tk_popup(x, y)

    # ---- typing a text annotation (or retyping the PDF's text), right on the page ----
    def open_editor(self, i, cx, cy, text="", size=None, color=None, replace=None,
                    family="Arial", bold=False, italic=False, commit=None):
        """A text box on the page at (cx, cy), in the text's size and colour; it grows as you
        type. Clicking anywhere else (or Ctrl + Enter) puts the text on the page; Esc
        cancels. replace: the annotation being edited (taken off when the new one goes on);
        commit: what's done with the text instead (the PDF's own text being retyped)."""
        self.close_editor(commit=True)
        size = size or self.app.size("text")
        color = color or self.app.color("text")
        px = max(6, round(size * self.scale))
        font = (family, -px) + (("bold",) if bold else ()) + (("italic",) if italic else ())
        box = tk.Text(self.canvas, font=font, fg=untranslated(color, "text"),
                      bg="#FFFFFE", relief="solid", bd=1, wrap="none", width=12, height=1,
                      padx=1, pady=0, highlightthickness=0,
                      insertbackground=untranslated(color, "text"), undo=True)
        box.insert("1.0", text)
        item = self.canvas.create_window(cx - 2, cy - px // 2 - 2, window=box, anchor="nw",
                                         tags="editor")
        self.editor = {"box": box, "item": item, "page": i, "point": self.to_pdf(i, cx, cy - px / 2),
                       "size": size, "color": color, "replace": replace, "commit": commit,
                       "was": text}

        def grow(_=None):
            """The box as big as its text, measured in pixels: each line as wide as its
            letters in their own fonts, as tall as its biggest (a count of letters fell short
            with wide or mixed fonts, and the cover hid the rest)."""
            ed = self.editor
            if ed and ed.get("wrap"):  # (a frame's box: as wide as the frame, as tall as its
                self.canvas.itemconfigure(item, width=ed["wrap"])  # lines wrap to)
                box.update_idletasks()
                tall = box.count("1.0", "end", "ypixels")
                self.canvas.itemconfigure(item, height=(tall[0] if tall else 20) + 6)
                box.yview_moveto(0)
                return
            lines = box.get("1.0", "end-1c").split("\n")
            base = font_of(box.cget("font"), box)
            width, height = base.measure("0") * 12, 0
            for n, line in enumerate(lines, start=1):
                w = box.count(f"{n}.0", f"{n}.end", "xpixels") if line else None
                width = max(width, (w[0] if w else 0) + base.measure("0") * 2)
                fonts = {str(box.cget("font"))}
                for k in range(len(line)):
                    for tag in box.tag_names(f"{n}.{k}"):
                        f = box.tag_cget(tag, "font")
                        if f:
                            fonts.add(str(f))
                height += max(font_of(f, box).metrics("linespace") for f in fonts)
            self.canvas.itemconfigure(item, width=width + 4, height=height + 2)
            box.xview_moveto(0)  # (all of it fits: none scrolled out of sight)
            box.yview_moveto(0)
        ed = self.editor
        ed["grow"] = grow
        box.bind("<KeyRelease>", grow)
        box.bind("<Map>", lambda e: self.after_idle(grow), add="+")  # (its lines only known
        box.bind("<Configure>", lambda e: self.after_idle(grow), add="+")  # once it's shown)
        box.bind("<Escape>", lambda e: self.close_editor(commit=False))
        box.bind("<Control-Return>", lambda e: self.close_editor(commit=True) or "break")
        grow()
        box.focus_set()
        box.mark_set("insert", "end")
        self.app.say("Type the text. Click outside the box (or press Ctrl + Enter) when "
                     "you're done; Esc cancels.")

    def close_editor(self, commit=True):
        ed, self.editor = self.editor, None
        if not ed:
            return
        text = ed["box"].get("1.0", "end-1c").rstrip()
        runs = self.editor_runs(ed) if ed.get("rich") else None
        self.canvas.delete(ed["item"])
        self.canvas.delete("editcover")
        ed["box"].destroy()
        self.draw_overlays()
        if getattr(self, "_relayout_later", False):  # (a resize waited for the typing to end)
            self._relayout_later = False
            self.after(10, self.relayout_after_resize)
        if ed.get("rich"):  # a line of the PDF, letter by letter: put in, if it was changed
            if commit and (runs != ed["was"] or ed.get("align") or ed.get("dir")):
                ed["commit"](runs)
        elif ed["commit"]:  # the PDF's own text: put in, if it was changed
            if commit and text != ed["was"]:
                ed["commit"](text)
        elif commit and (text or ed["replace"] is not None):
            self.app.add_text(ed["page"], ed["point"], text, ed["size"], ed["color"],
                              replace=ed["replace"])
        self.app.say_tool()


class Splitter(tk.Frame):
    """The bar between the side panel and the pages: drag it to make the panel wider or
    narrower. Like Windows 98's (Explorer's), it shows where the edge will go with a dotted
    bar while it's dragged, and the panel takes the width when the mouse lets go."""
    W = 5

    def __init__(self, parent, panel, on_change):
        super().__init__(parent, bg=BG, width=self.W, cursor="sb_h_double_arrow")
        self.panel, self.on_change = panel, on_change
        self.bar, self.start = None, None
        self.bind("<ButtonPress-1>", self.press)
        self.bind("<B1-Motion>", self.drag)
        self.bind("<ButtonRelease-1>", self.release)

    def limits(self):
        top = self.winfo_toplevel()
        return ThumbBar.MIN_WIDTH, max(ThumbBar.MIN_WIDTH, top.winfo_width() // 2)

    def press(self, e):
        self.start = (e.x_root, self.panel.width)
        h = self.winfo_height()
        if ("bar", h) not in ClassicWindow._CHECKS:  # a dotted (checkered) bar, as tall as this
            img = Image.new("RGB", (4, h))
            img.putdata([(0, 0, 0) if (i % 4 + i // 4) % 2 else (255, 255, 255) for i in range(4 * h)])
            ClassicWindow._CHECKS[("bar", h)] = ImageTk.PhotoImage(img)
        bar = self.bar = tk.Toplevel(self)
        bar.overrideredirect(True)
        bar.attributes("-topmost", True)
        tk.Label(bar, image=ClassicWindow._CHECKS[("bar", h)], bd=0, highlightthickness=0,
                 cursor="sb_h_double_arrow").pack()
        self.drag(e)

    def new_width(self, e):
        lo, hi = self.limits()
        return min(max(self.start[1] + e.x_root - self.start[0], lo), hi)

    def drag(self, e):
        if not self.bar:
            return
        x = self.winfo_rootx() + self.new_width(e) - self.start[1]
        self.bar.geometry(f"4x{self.winfo_height()}+{x}+{self.winfo_rooty()}")

    def release(self, e):
        if not self.bar:
            return
        width = self.new_width(e)
        self.bar.destroy()
        self.bar = None
        if width != self.panel.width:
            self.on_change(width)


class ThumbBar(tk.Frame):
    """The page thumbnails down the left side, like Acrobat's: click one to go to that page
    (Ctrl / Shift + click for several), drag them to put the pages in another order, and
    right-click for the page commands."""
    WIDTH, THUMB, GAP, NUM_H = 150, 92, 36, 18  # (the gaps hold the "+" buttons)
    PLUS_W, PLUS_H = 17, 15
    BOX_LINE, CURRENT = "#B0B0B0", "#7DA2CE"

    def __init__(self, parent, app):
        super().__init__(parent, bg=BG, width=self.WIDTH)
        self.width = self.WIDTH  # (now: see set_width)
        self.pack_propagate(False)  # (it keeps its width, whatever it shows: what's in it fills it)
        self.app = app
        head = tk.Frame(self, bg=BG)
        head.pack(fill="x")
        # what the panel shows: a flat button with the name and a little arrow, which presses
        # in and opens its list - like the menu bar's
        self._arrow = ImageTk.PhotoImage(self.arrow_picture(), master=self)
        self.chooser = tk.Button(head, text="Pages", image=self._arrow, compound="right",
                                 bg=BG, fg="black", font=FONT, relief="flat", bd=2, padx=4,
                                 pady=0, highlightthickness=0, activebackground=BG, takefocus=0)
        self.chooser.pack(side="left", padx=1, pady=(0, 1))
        self.chooser.bind("<ButtonPress-1>", lambda e: self.open_chooser())
        self.views = {}
        box = tk.Frame(self, bg=BG)
        self.views["pages"] = box
        c = self.canvas = tk.Canvas(box, bg="white", relief="sunken", bd=2, highlightthickness=0,
                                    width=self.WIDTH - FlatScrollbar.W - 4, yscrollincrement=20)
        self.sb = FlatScrollbar(box, command=c.yview)
        c.config(yscrollcommand=self.on_scroll)
        self.sb.pack(side="right", fill="y")
        c.pack(side="left", fill="both", expand=True)
        self.cells = []  # each page's (x, y, w, h) thumbnail box
        self.plus_down = None  # the + whose menu is open
        self.drag = None  # while pages are dragged: what's moving, and where things are
        self.thumb_pictures = {}  # page -> its small picture (Pillow), for the dragged copy
        self.images = {}  # page -> PhotoImage
        self.selected = set()
        self.last_click = None
        self.press = None
        c.bind("<Configure>", lambda e: self.layout())
        c.bind("<ButtonPress-1>", self.on_press)
        c.bind("<B1-Motion>", self.on_drag)
        c.bind("<ButtonRelease-1>", self.on_release)
        c.bind("<Button-3>", self.on_right_click)
        c.bind("<MouseWheel>", lambda e: e.delta and c.yview_scroll(
            -int(e.delta / 120) * 2 or (-1 if e.delta > 0 else 1), "units"))
        c.bind("<Control-MouseWheel>", self.ctrl_wheel)
        self.lists = {"bookmarks": self.make_list("bookmarks"),
                      "annotations": self.make_list("annotations")}
        self.views["results"] = self.make_results()
        self.view = None
        start = load_settings().get("side_panel", "pages")  # (Search results: only while
        self.show_view("pages" if start == "results" else start)  # something's being found)

    # ---- what the side panel shows: Pages, Bookmarks or Annotations ----
    VIEW_NAMES = {"pages": "Pages", "bookmarks": "Bookmarks", "annotations": "Annotations",
                  "results": "Search results"}

    @staticmethod
    def arrow_picture():
        """The little arrow beside the panel's name: black - white in the dark theme."""
        im = arrow_icon("down").crop((3, 3, 13, 13))
        return whitened(im) if THEME == "dark" else im

    def refresh_arrow(self):
        """The theme changed: the arrow in its colour."""
        self._arrow = ImageTk.PhotoImage(self.arrow_picture(), master=self)
        self.chooser.config(image=self._arrow)

    def open_chooser(self):
        self.chooser.config(relief="sunken")  # (pressed in while its list is open)
        menu = PopupMenu(self, on_close=lambda: self.chooser.config(relief="flat"))
        for key, name in self.VIEW_NAMES.items():
            menu.add_command(label=name, command=lambda k=key: self.show_view(k),
                             checked=key == self.view)
        menu.tk_popup(self.chooser.winfo_rootx(),
                      self.chooser.winfo_rooty() + self.chooser.winfo_height(),
                      min_width=self.width - 10)
        return "break"

    MIN_WIDTH = 110
    WHEEL_STEP = 20  # how much wider / narrower each notch of Ctrl + wheel makes the panel

    def ctrl_wheel(self, e):
        """Ctrl + mouse wheel over the panel: bigger thumbnails (wheel up) or smaller (down) -
        the panel gets wider or narrower, as if its splitter were dragged."""
        if e.delta:
            lo, hi = self.app.splitter.limits()
            width = min(max(self.width + (self.WHEEL_STEP if e.delta > 0 else -self.WHEEL_STEP),
                            lo), hi)
            if width != self.width:
                self.app.set_side_width(width)
        return "break"  # (not also scrolled)

    def thumb(self):
        """How wide a thumbnail is: the panel's width less its margins and scrollbar."""
        return max(40, self.width - (self.WIDTH - self.THUMB))

    def set_width(self, width):
        """Make the panel this wide: the thumbnails grow (or shrink) to fill it, and the
        lists show more of their lines."""
        self.width = width
        self.config(width=width)
        self.canvas.config(width=width - FlatScrollbar.W - 4)
        for lst in self.lists.values():
            lst["tree"].column("#0", width=width - FlatScrollbar.W - 10)
            lst["note"].config(wraplength=width - 24)
        self.layout()

    def show_view(self, key):
        if key not in self.VIEW_NAMES:
            key = "pages"
        for view in self.views.values():
            view.pack_forget()
        self.views[key].pack(fill="both", expand=True)
        self.view = key
        self.chooser.config(text=self.VIEW_NAMES[key])
        if "annots" in getattr(self.app, "buttons", {}):  # (pressed in while the list shows)
            self.app.buttons["annots"].set_latched(key == "annotations" and getattr(self.app, "show_thumbs", True))
        if key != "results":  # (what the panel shows next time - not a search from now)
            save_settings(side_panel=key)
        if key in self.lists:
            self.fill_list(key)
        else:
            self.app.schedule_render()

    def make_results(self):
        """Find's results, like Word's navigation pane: "Result 3 of 60" with buttons for the
        one before and after, then each place the words are - its line, the words in bold,
        the page - to click and go there."""
        frame = tk.Frame(self, bg=BG)
        head = tk.Frame(frame, bg=BG)
        head.pack(fill="x", padx=2, pady=(0, 2))
        self.res_count = tk.Label(head, text="", bg=BG, font=FONT, anchor="w")
        self.res_count.pack(side="left", fill="x", expand=True)
        for icon, tip, step in (("down", "Next result (Enter)", 1),
                                ("up", "Previous result (Shift + Enter)", -1)):
            b = ToolButton(head, arrow_icon(icon), tip, lambda s=step: self.app.find_next(s))
            b.whiten = 16  # (white in the dark theme)
            b.draw()
            b.pack(side="right")
        well = tk.Frame(frame, bg="white", relief="sunken", bd=2)
        sb = FlatScrollbar(frame, command=lambda *a: self.res_text.yview(*a))
        txt = tk.Text(well, bg="white", relief="flat", bd=0, wrap="word", font=FONT,
                      cursor="arrow", padx=4, pady=2, highlightthickness=0, takefocus=0,
                      spacing1=2, spacing3=4, width=1, height=1)
        txt.tag_config("hit", font=(FONT[0], FONT[1], "bold"))
        txt.tag_config("where", foreground="#808080")
        txt.tag_config("note", foreground="#808080", justify="center")
        sel_bg, sel_fg = select_colors()
        txt.tag_config("now", background=sel_bg, foreground=sel_fg)
        txt.config(yscrollcommand=sb.set, state="disabled")
        sb.pack(side="right", fill="y")
        well.pack(side="left", fill="both", expand=True)
        txt.pack(fill="both", expand=True)

        def click(e):
            for tag in txt.tag_names(txt.index(f"@{e.x},{e.y}")):
                if tag.startswith("r") and tag[1:].isdigit():
                    self.app.go_result(int(tag[1:]))
                    break
            return "break"
        txt.bind("<Button-1>", click)
        txt.bind("<B1-Motion>", lambda e: "break")  # (not selected like text)
        txt.bind("<Double-Button-1>", lambda e: "break")
        self.res_text = txt
        return frame

    def fill_results(self, rows, text):
        """The results list made again: rows [(page, before, the words, after)]."""
        txt = self.res_text
        txt.config(state="normal")
        txt.delete("1.0", "end")
        if not rows:
            txt.insert("end", f'\nNo results for "{text}".' if text else "", "note")
        for k, (page, before, words, after) in enumerate(rows):
            tag = f"r{k}"
            txt.insert("end", before, (tag,))
            txt.insert("end", words, (tag, "hit"))
            txt.insert("end", after, (tag,))
            txt.insert("end", f"   p. {page + 1}\n", (tag, "where"))
        txt.config(state="disabled")
        self.res_count.config(text=(f"{len(rows)} result{'s' if len(rows) != 1 else ''}"
                                    if rows else "No results" if text else ""))

    def set_result(self, k, total):
        """The result you're at: marked in the list (scrolled into view), and counted."""
        txt = self.res_text
        txt.tag_remove("now", "1.0", "end")
        ranges = txt.tag_ranges(f"r{k}")
        if ranges:
            txt.tag_add("now", ranges[0], ranges[1])
            txt.see(ranges[0])
            txt.see(ranges[1])
        self.res_count.config(text=f"Result {k + 1} of {total}")

    def make_list(self, key):
        """A list for bookmarks or annotations: a tree in a sunken box (with the app's
        scrollbar), or a note when there's nothing in it."""
        frame = tk.Frame(self, bg=BG)
        self.views[key] = frame
        well = tk.Frame(frame, bg="white", relief="sunken", bd=2)  # (sunken, like the
        tree = ttk.Treeview(well, show="tree", selectmode="browse")  # thumbnails' box)
        tree.column("#0", width=self.WIDTH - FlatScrollbar.W - 10, stretch=True)
        tree.pack(fill="both", expand=True)
        tree.well = well
        sb = FlatScrollbar(frame, command=tree.yview)
        tree.config(yscrollcommand=sb.set)
        note = EngravedLabel(frame, wraplength=self.WIDTH - 24, justify="center")
        tree.bind("<<TreeviewSelect>>", lambda e: self.list_go(key, double=False))
        tree.bind("<Double-Button-1>", lambda e: self.list_go(key, double=True))
        tree.bind("<Button-3>", lambda e: self.list_menu(key, e))
        tree.bind("<Control-MouseWheel>", self.ctrl_wheel)
        return {"tree": tree, "sb": sb, "note": note, "targets": {}, "stale": True}

    def refresh_lists(self):
        """The document changed: the lists are made again (now, if one is showing)."""
        for key, lst in self.lists.items():
            lst["stale"] = True
        if self.view in self.lists:
            self.fill_list(self.view)

    def fill_list(self, key):
        lst = self.lists[key]
        tree, sb, note = lst["tree"], lst["sb"], lst["note"]

        def path(iid):  # an item's place in the tree, by its and its parents' names
            names = []
            while iid:
                names.insert(0, tree.item(iid, "text"))
                iid = tree.parent(iid)
            return tuple(names)
        opened = {path(i) for i in self.all_items(tree) if tree.item(i, "open")}
        first = not lst.get("filled")  # (bookmarks start closed - just the top level shows;
        lst["filled"] = True  # what's been opened stays open when the list is made again)
        tree.delete(*tree.get_children())
        lst["targets"], lst["stale"] = {}, False
        rows = self.bookmark_rows() if key == "bookmarks" else self.annotation_rows()
        parents = {0: ""}
        names = {}
        for level, text, target in rows:
            names[level] = text
            here = tuple(names[k] for k in range(level + 1))
            show = (key == "annotations") if first else here in opened or key == "annotations"
            iid = tree.insert(parents.get(level - 1, ""), "end", text=text, open=show)
            parents[level] = iid
            lst["targets"][iid] = target
        if rows:
            note.pack_forget()
            sb.pack(side="right", fill="y")
            tree.well.pack(side="left", fill="both", expand=True)
        else:
            tree.well.pack_forget()
            sb.pack_forget()
            note.config(text="" if not self.app.doc else
                        "This PDF has no bookmarks." if key == "bookmarks" else
                        "No annotations yet. Add text, highlights, drawings or notes with the "
                        "toolbar.")
            note.pack(fill="x", padx=6, pady=12)

    @staticmethod
    def all_items(tree, parent=""):
        for iid in tree.get_children(parent):
            yield iid
            yield from ThumbBar.all_items(tree, iid)

    def bookmark_rows(self):
        """The PDF's own table of contents: (level, title, (page, where on it))."""
        doc = self.app.doc
        if not doc:
            return []
        with PDF_LOCK:
            toc = doc.get_toc(simple=False)
        rows = []
        for item in toc:
            level, title, page = item[0], item[1], item[2]
            dest = item[3] if len(item) > 3 and isinstance(item[3], dict) else {}
            to = dest.get("to")
            rows.append((level - 1, title.strip() or "(untitled)",
                         (page - 1, pymupdf.Point(to) if to is not None else None)))
        return rows

    ANNOT_NAMES = {"FreeText": "Text", "Text": "Note", "Highlight": "Highlight", "Ink": "Drawing",
                   "Square": "Highlight", "Underline": "Underline", "StrikeOut": "Strike-out"}

    def annotation_rows(self):
        """Every annotation, under its page: (level, what it is and says, (page, its xref))."""
        doc = self.app.doc
        if not doc:
            return []
        rows = []
        with PDF_LOCK:
            for i in range(doc.page_count):
                page = doc[i]
                found = []
                for annot in page.annots():
                    kind = annot.type[1]
                    says = annot.info.get("content", "").strip()
                    if kind in ("Highlight", "Underline", "StrikeOut") and not says:
                        says = page.get_textbox(annot.rect).strip()
                    says = " ".join(says.split())
                    label = self.ANNOT_NAMES.get(kind, kind)
                    found.append(f"{label}: {says[:40]}" if says else label)
                    found[-1] = (found[-1], annot.xref)
                if found:
                    rows.append((0, f"Page {i + 1}", (i, None)))
                    rows += [(1, text, (i, xref)) for text, xref in found]
        return rows

    def list_go(self, key, double):
        """An item clicked: go there (an annotation is picked up; double-click to change it)."""
        lst = self.lists[key]
        sel = lst["tree"].selection()
        if not sel or not self.app.doc:
            return
        target = lst["targets"].get(sel[0])
        if not target:
            return
        i, where = target
        if i >= self.app.doc.page_count:
            return
        self.app.set_current(i, scroll=True)
        if key == "bookmarks" and where is not None:
            self.app.view.show_rect(i, pymupdf.Rect(where, where + (1, 1)))
        elif key == "annotations" and where is not None:
            if self.app.tool not in ("move", "select"):
                self.app.set_tool("select")
            self.app.view.select((i, where))
            info = self.app.view.selected_info()
            if info:
                self.app.view.show_rect(i, info[1])
            if double:
                self.app.edit_annot(i, where)

    def list_menu(self, key, e):
        lst = self.lists[key]
        tree = lst["tree"]
        iid = tree.identify_row(e.y)
        if not iid:
            return
        tree.selection_set(iid)
        target = lst["targets"].get(iid)
        menu = PopupMenu(self)
        menu.add_command(label="Go to it", command=lambda: self.list_go(key, double=False))
        if key == "annotations" and target and target[1] is not None:
            menu.add_separator()
            menu.add_command(label="Delete", command=lambda: (self.list_go(key, double=False),
                                                              self.app.delete_annot()))
        menu.tk_popup(e.x_root, e.y_root)

    def on_scroll(self, first, last):
        self.sb.set(first, last)
        self.app.schedule_render()

    def layout(self):
        """Place every page's box (their pictures are drawn as they come into view)."""
        self.end_drag()
        c = self.canvas
        c.delete("all")
        self.images, self.thumb_pictures = {}, {}
        sizes = self.app.page_sizes if self.app.doc else []
        self.selected = {i for i in self.selected if i < len(sizes)}
        self.stretch = screen_stretch(self)  # (see page_picture)
        W = max(60, c.winfo_width() - 4)
        y, self.cells = self.GAP, []
        for w, h in sizes:
            k = min(self.thumb() / w, self.thumb() * 1.3 / h)
            tw, th = round(w * k), round(h * k)
            x = (W - tw) // 2
            self.cells.append((x, y, tw, th))
            y += th + self.NUM_H + self.GAP
        c.config(scrollregion=(0, 0, W, max(y, c.winfo_height() - 4)))
        self.draw_marks()
        self.app.schedule_render()

    def draw_marks(self):
        """The boxes, the page numbers, and which pages are selected / in view."""
        if self.drag:
            return  # (the dragging draws its own)
        c = self.canvas
        c.delete("mark")
        sel_bg, sel_fg = select_colors()
        for i, (x, y, w, h) in enumerate(self.cells):
            sel, cur = i in self.selected, i == self.app.current
            color = sel_bg if sel else self.CURRENT if cur else self.BOX_LINE
            width = 2 if sel or cur else 1
            c.create_rectangle(x - 2, y - 2, x + w + 2, y + h + 2, outline=color, width=width,
                               tags="mark")
            if i not in self.images:
                c.create_rectangle(x, y, x + w, y + h, fill="#FFFFFE", outline="", tags="mark")
            label = c.create_text(x + w // 2, y + h + 10, text=str(i + 1), font=FONT,
                                  fill=sel_fg if sel else "black", tags="mark")
            if sel:
                x0, y0, x1, y1 = c.bbox(label)
                bg = c.create_rectangle(x0 - 3, y0, x1 + 3, y1, fill=sel_bg, outline="", tags="mark")
                c.tag_lower(bg, label)
        for n in range(len(self.cells) + 1 if self.cells else 0):
            self.draw_plus(n, pressed=self.plus_down == n)
        c.tag_raise("pic")

    def visible(self):
        c = self.canvas
        top, bottom = c.canvasy(0) - 100, c.canvasy(c.winfo_height()) + 300
        return [i for i, (x, y, w, h) in enumerate(self.cells) if y + h >= top and y <= bottom]

    def next_render(self):
        if self.drag or self.view != "pages":
            return None
        for i in self.visible():
            if i not in self.images:
                return lambda i=i: self.render(i)
        return None

    def render(self, i):
        if i >= len(self.cells):
            return
        x, y, w, h = self.cells[i]
        page = self.app.doc[i]
        img = page_picture(page, w / max(page.rect.width, 1), getattr(self, "stretch", 1.0))
        self.thumb_pictures[i] = img
        self.images[i] = ImageTk.PhotoImage(img, master=self.canvas)
        self.canvas.create_image(x, y, image=self.images[i], anchor="nw", tags=("pic", f"pic{i}"))
        self.draw_marks()

    def invalidate(self, pages):
        for i in pages:
            self.images.pop(i, None)
            self.thumb_pictures.pop(i, None)
            self.canvas.delete(f"pic{i}")
        self.draw_marks()
        self.app.schedule_render()

    def see(self, i):
        if 0 <= i < len(self.cells):
            c = self.canvas
            x, y, w, h = self.cells[i]
            top, bottom = c.canvasy(0), c.canvasy(c.winfo_height())
            if y - 4 < top or y + h + self.NUM_H > bottom:
                region = float(str(c.cget("scrollregion")).split()[3])
                c.yview_moveto(max(0, y - self.GAP) / max(1, region))

    # ---- the "+" between the pages: put pages in just there ----
    def plus_y(self, n):
        """Where the + before page n goes (n = the number of pages: after the last): just
        as far from the page number above it as from the page frame below it."""
        f = font_of(FONT, self.canvas)
        # the number is centred 10 px under its page: its letters end at its baseline
        under_number = 10 - f.metrics("linespace") / 2 + f.metrics("ascent")
        half = (self.NUM_H + self.GAP - 3 - under_number) / 2  # (the frame is 3 px out)
        if n < len(self.cells):
            return round(self.cells[n][1] - 3 - half)
        x, y, w, h = self.cells[-1]
        return round(y + h + under_number + half)

    def plus_box(self, n):
        W = max(60, self.canvas.winfo_width() - 4)
        x0, y0 = (W - self.PLUS_W) // 2, self.plus_y(n) - self.PLUS_H // 2
        return x0, y0, x0 + self.PLUS_W, y0 + self.PLUS_H

    def draw_plus(self, n, pressed=False):
        """An etched line across the gap with a small raised + button in its middle, like a
        Windows 98 button (pressed in while it's held / its menu is open)."""
        c = self.canvas
        c.delete(f"plus{n}")
        tags = ("mark", f"plus{n}")
        W = max(60, c.winfo_width() - 4)
        x0, y0, x1, y1 = self.plus_box(n)
        my = self.plus_y(n)
        for a, b in ((8, x0 - 3), (x1 + 3, W - 8)):  # the line, either side of the button
            c.create_line(a, my, b, my, fill=EDGE_SHADOW, tags=tags)
            c.create_line(a, my + 1, b, my + 1, fill=EDGE_LIGHT, tags=tags)
        c.create_rectangle(x0, y0, x1, y1, fill=BG, outline="", tags=tags)
        tl, br = (EDGE_DARK, EDGE_LIGHT) if pressed else (EDGE_LIGHT, EDGE_DARK)
        c.create_line(x0, y1 - 1, x0, y0, x1 - 1, y0, fill=tl, tags=tags)
        c.create_line(x0, y1 - 1, x1 - 1, y1 - 1, x1 - 1, y0 - 1, fill=br, tags=tags)
        if pressed:
            c.create_line(x0 + 1, y1 - 2, x0 + 1, y0 + 1, x1 - 2, y0 + 1, fill=EDGE_SHADOW, tags=tags)
        else:
            c.create_line(x0 + 1, y1 - 2, x1 - 2, y1 - 2, x1 - 2, y0, fill=EDGE_SHADOW, tags=tags)
        o = 1 if pressed else 0  # (the + moves down and right when pressed, like a button)
        cx, cy = (x0 + x1) // 2 + o, (y0 + y1) // 2 + o
        c.create_rectangle(cx - 3, cy, cx + 4, cy + 1, fill="#000000", outline="", tags=tags)
        c.create_rectangle(cx, cy - 3, cx + 1, cy + 4, fill="#000000", outline="", tags=tags)

    def plus_at(self, cx, cy):
        if not self.cells:
            return None
        for n in range(len(self.cells) + 1):
            x0, y0, x1, y1 = self.plus_box(n)
            if x0 - 2 <= cx <= x1 + 2 and y0 - 2 <= cy <= y1 + 2:
                return n
        return None

    def open_plus(self, n):
        """The + was clicked: what can go in there."""
        self.plus_down = n
        self.draw_plus(n, pressed=True)
        x0, y0, x1, y1 = self.plus_box(n)
        c = self.canvas
        x = c.winfo_rootx() + 2 + int(x0 - c.canvasx(0))
        y = c.winfo_rooty() + 2 + int(y1 - c.canvasy(0)) + 2

        def closed():
            self.plus_down = None
            if self.cells:
                self.draw_plus(n)
        menu = PopupMenu(self, on_close=closed)
        menu.add_command(label="Insert pages from file...",
                         command=lambda: self.app.insert_from_file(at=n))
        menu.add_command(label="Insert blank page", command=lambda: self.app.insert_blank(at=n))
        menu.tk_popup(x, y)

    def cell_at(self, cx, cy):
        for i, (x, y, w, h) in enumerate(self.cells):
            if y - self.GAP / 2 <= cy <= y + h + self.NUM_H + self.GAP / 2:
                return i
        return None

    def gap_at(self, cy):
        """Where a dragged page would go: before page n (n = len: at the end)."""
        for i, (x, y, w, h) in enumerate(self.cells):
            if cy < y + h / 2:
                return i
        return len(self.cells)

    def selection(self):
        """The pages the page commands work on: the selected ones, or the current page."""
        return sorted(self.selected) or ([self.app.current] if self.app.doc else [])

    def on_press(self, e):
        self.canvas.focus_set()
        cx, cy = self.canvas.canvasx(e.x), self.canvas.canvasy(e.y)
        n = self.plus_at(cx, cy)
        if n is not None:
            self.open_plus(n)
            return
        i = self.cell_at(cx, cy)
        if i is None:
            return
        if e.state & 0x0004:  # Ctrl: add / take away
            self.selected ^= {i}
        elif e.state & 0x0001 and self.last_click is not None:  # Shift: a range
            a, b = sorted((self.last_click, i))
            self.selected = set(range(a, b + 1))
        else:
            if i not in self.selected:
                self.selected = {i}
            self.press = (i, cx, cy)  # (a drag from here moves the selected pages)
        self.last_click = i
        self.app.set_current(i)
        self.draw_marks()

    # ---- dragging pages into another order, the others making room as you go ----
    def on_drag(self, e):
        if not self.press:
            return
        c = self.canvas
        cx, cy = c.canvasx(e.x), c.canvasy(e.y)
        if not self.drag:
            if abs(cy - self.press[2]) < 6 and abs(cx - self.press[1]) < 6:
                return  # (still a click, not a drag)
            self.start_drag()
        if e.y < 0:  # near the ends: the list scrolls along
            c.yview_scroll(-1, "units")
        elif e.y > c.winfo_height():
            c.yview_scroll(1, "units")
        d = self.drag
        c.coords("ghost", cx - d["grab"][0], cy - d["grab"][1])
        c.tag_raise("ghost")
        # where it would go: before the first of the other pages whose middle is below the
        # mouse, as they're shown now (so the gap opened under the mouse holds steady)
        k = sum(1 for q in d["rest"] if d["shown"][q] + self.cells[q][3] / 2 < cy)
        if k != d["k"]:
            d["k"] = k
            self.slide_to(self.drag_places())

    def start_drag(self):
        """Lift the selected pages: they become a see-through copy under the mouse, and a
        dotted gap where they are (the other pages slide to make room as they're moved)."""
        moving = sorted(self.selected)
        rest = [q for q in range(len(self.cells)) if q not in self.selected]
        i, px, py = self.press
        x, y, w, h = self.cells[i]
        self.drag = {"moving": moving, "rest": rest, "k": sum(1 for q in rest if q < moving[0]),
                     "grab": (px - x, py - y), "shown": {q: self.cells[q][1] for q in range(len(self.cells))}}
        # the copy under the mouse: the page's picture, see-through, with a frame round it
        pic = self.thumb_pictures.get(i) or Image.new("RGB", (w, h), "white")
        ghost = pic.convert("RGBA").resize((w, h))
        ImageDraw.Draw(ghost).rectangle([0, 0, w - 1, h - 1], outline=(0, 0, 0, 255))
        if len(moving) > 1:  # several: how many, in a little badge
            d = ImageDraw.Draw(ghost)
            d.rectangle([w - 22, 2, w - 3, 16], fill=hex_rgb_255(select_colors()[0]) + (255,))
            d.text((w - 18, 3), str(len(moving)), fill=hex_rgb_255(select_colors()[1]) + (255,))
        ghost.putalpha(ghost.getchannel("A").point(lambda a: a * 150 // 255))
        self.drag["photo"] = ImageTk.PhotoImage(ghost, master=self.canvas)
        self.draw_drag_scene()

    def drag_places(self):
        """Where each page goes in the order being tried: {page: its top}."""
        d = self.drag
        order = d["rest"][:d["k"]] + d["moving"] + d["rest"][d["k"]:]
        places, y = {}, self.GAP
        for q in order:
            places[q] = y
            y += self.cells[q][3] + self.NUM_H + self.GAP
        return places

    def draw_drag_scene(self):
        """Every page as a group that can slide (its frame, picture and number); the pages
        being moved as dotted gaps; and the copy under the mouse."""
        c, d = self.canvas, self.drag
        c.delete("all")
        sel_bg = select_colors()[0]
        places = self.drag_places()
        d["shown"] = dict(places)
        order = sorted(places, key=places.get)
        for n, q in enumerate(order):
            x, _y, w, h = self.cells[q]
            y = places[q]
            tags = ("mark", f"g{q}")
            if q in d["moving"]:  # where it'll land: a dotted gap
                c.create_rectangle(x - 2, y - 2, x + w + 2, y + h + 2, outline=sel_bg, width=1,
                                   dash=(2, 2), tags=tags)
            else:
                c.create_rectangle(x - 2, y - 2, x + w + 2, y + h + 2, outline=self.BOX_LINE,
                                   tags=tags)
                if q in self.images:
                    c.create_image(x, y, image=self.images[q], anchor="nw", tags=tags)
                else:
                    c.create_rectangle(x, y, x + w, y + h, fill="#FFFFFE", outline="", tags=tags)
            c.create_text(x + w // 2, y + h + 10, text=str(n + 1), font=FONT, fill="black",
                          tags=tags + (f"n{q}",))
        i, px, py = self.press
        gx = self.cells[i][0]
        c.create_image(gx, py - d["grab"][1], image=d["photo"], anchor="nw", tags="ghost")

    def slide_to(self, places, steps=6):
        """Slide the pages to their new places over a few frames (about 0.1 s), their
        numbers changing to the new order."""
        d = self.drag
        if d.get("job"):
            self.after_cancel(d["job"])
        start = dict(d["shown"])
        order = sorted(places, key=places.get)
        for n, q in enumerate(order):
            self.canvas.itemconfigure(f"n{q}", text=str(n + 1))

        def frame(k):
            if self.drag is not d:
                return
            for q, goal in places.items():
                now = start[q] + (goal - start[q]) * k / steps
                self.canvas.move(f"g{q}", 0, now - d["shown"][q])
                d["shown"][q] = now
            d["job"] = self.after(16, frame, k + 1) if k < steps else None
        frame(1)

    def end_drag(self):
        d, self.drag = self.drag, None
        if d and d.get("job"):
            self.after_cancel(d["job"])
        return d

    def on_release(self, e):
        p, self.press = self.press, None
        d = self.end_drag()
        if d:  # dropped: the pages go where the gap was
            rest, k = d["rest"], d["k"]
            to = rest[k] if k < len(rest) else len(self.cells)
            order = rest[:k] + d["moving"] + rest[k:]
            if order == list(range(len(self.cells))):
                self.layout()  # (put back where it was: nothing changes)
            else:
                self.app.move_pages(d["moving"], to)
        elif p and not (e.state & 0x0005):  # a plain click: just this page
            self.selected = {p[0]}
            self.draw_marks()

    def on_right_click(self, e):
        cx, cy = self.canvas.canvasx(e.x), self.canvas.canvasy(e.y)
        i = self.cell_at(cx, cy)
        if i is None:
            return
        if i not in self.selected:
            self.selected = {i}
            self.last_click = i
            self.app.set_current(i)
        self.draw_marks()
        self.app.page_menu(e.x_root, e.y_root)


# ---- the File menu's tools, each in its own dialog box ----
EXPORT_FORMATS = {"PNG": ".png", "JPEG": ".jpg", "WEBP": ".webp", "TIFF": ".tiff", "BMP": ".bmp"}
EXPORT_DPI = ["72 dpi", "96 dpi", "150 dpi", "200 dpi", "300 dpi", "600 dpi"]
LOSSY_PICTURES = {"JPEG", "WEBP"}
SPLIT_MODES = ["Single pages", "Every few pages", "Page ranges"]
# how far the pictures inside are shrunk -> the resolution they're brought down to
COMPRESS_LEVELS = {"Small file (96 dpi)": 96, "Balanced (150 dpi)": 150,
                   "High quality (220 dpi)": 220, "Keep the pictures as they are": None}
PDF_TYPES = [("PDF files", "*.pdf"), ("All files", "*.*")]
FILE_TYPES = [("PDFs, Word documents and pictures",
               " ".join("*" + e for e in PDF_EXTS + WORD_EXTS + IMAGE_EXTS)),
              ("Word documents", " ".join("*" + e for e in WORD_EXTS)),
              ("All files", "*.*")]


SIGNATURE_FILE = os.path.join(os.path.dirname(SETTINGS_PATH), "signature.png")
SCRIPT_FONTS = ["Segoe Script", "Lucida Handwriting", "Ink Free", "Brush Script MT",
                "Freestyle Script", "Mistral", "Bradley Hand ITC", "Kristen ITC", "Segoe Print",
                "Gabriola", "Comic Sans MS"]  # handwriting-like fonts (those installed are offered)
INK_COLORS = {"Black": (0, 0, 0), "Blue": (0, 51, 204), "Dark blue": (0, 0, 128)}


def signature_dialog(owner):
    """Make a signature: drawn with the mouse, or typed in a handwriting font. Returns it as a
    picture with a see-through background (or None if cancelled)."""
    win, chrome, body = new_dialog(owner, "Signature")
    result = [None]
    top = tk.Frame(body, bg=BG)
    top.pack(fill="x", padx=12, pady=(10, 4))
    mode = tk.StringVar(value="draw")
    for text, val in (("Draw it", "draw"), ("Type it", "type")):
        tk.Radiobutton(top, text=text, variable=mode, value=val, bg=BG, activebackground=BG,
                       font=FONT, command=lambda: show()).pack(side="left", padx=(0, 10))
    ink = tk.StringVar(value="Black")
    cb = ttk.Combobox(top, textvariable=ink, values=list(INK_COLORS), state="readonly", width=9)
    cb.pack(side="right")
    tk.Label(top, text="Ink:", bg=BG, font=FONT).pack(side="right", padx=(0, 4))
    W, H = 380, 130
    area = tk.Frame(body, bg=BG)
    area.pack(padx=12)
    # drawing it: strokes made with the mouse on a white box, over a line to sign on
    draw_box = tk.Canvas(area, width=W, height=H, bg="white", relief="sunken", bd=2,
                         highlightthickness=0, cursor="pencil")
    strokes = []

    def guide():
        draw_box.create_line(20, H - 30, W - 20, H - 30, fill="#B0B0B0")
        draw_box.create_text(14, H - 34, text="x", font=FONT, fill="#888888")
    guide()

    def stroke_start(e):
        strokes.append([(e.x, e.y)])

    def stroke_on(e):
        if strokes:
            x, y = strokes[-1][-1]
            strokes[-1].append((e.x, e.y))
            draw_box.create_line(x, y, e.x, e.y, width=2, capstyle="round", smooth=True,
                                 fill="#%02X%02X%02X" % INK_COLORS[ink.get()])
    draw_box.bind("<ButtonPress-1>", stroke_start)
    draw_box.bind("<B1-Motion>", stroke_on)
    # typing it: a name, a font, and how it looks
    type_box = tk.Frame(area, bg=BG)
    row = tk.Frame(type_box, bg=BG)
    row.pack(fill="x", pady=(0, 6))
    tk.Label(row, text="Name:", bg=BG, font=FONT).pack(side="left")
    name = tk.StringVar()
    entry = tk.Entry(row, textvariable=name, font=FONT, width=24, relief="sunken", bd=2, bg="white")
    entry.pack(side="left", padx=(4, 8))
    installed = set(font_families())
    fonts = [f for f in SCRIPT_FONTS if f in installed] or ["Times New Roman"]
    family = tk.StringVar(value=fonts[0])
    fcb = ttk.Combobox(row, textvariable=family, values=fonts, state="readonly", width=18)
    fcb.pack(side="left")
    preview = tk.Canvas(type_box, width=W, height=H - 30, bg="white", relief="sunken", bd=2,
                        highlightthickness=0)
    preview.pack()
    keep = {}

    def rgb():
        return INK_COLORS[ink.get()]

    def typed_picture():
        from PIL import ImageFont
        text = name.get().strip()
        path = find_font(family.get(), 0)[0]
        if not text or not path:
            return None
        font = ImageFont.truetype(path, 120)
        box = ImageDraw.Draw(Image.new("RGBA", (1, 1))).textbbox((0, 0), text, font=font)
        im = Image.new("RGBA", (box[2] - box[0] + 20, box[3] - box[1] + 20), (0, 0, 0, 0))
        ImageDraw.Draw(im).text((10 - box[0], 10 - box[1]), text, font=font, fill=rgb() + (255,))
        return im

    def drawn_picture():
        if not strokes:
            return None
        k = 3  # (made 3 x bigger than drawn: smooth when it's placed on the page)
        im = Image.new("RGBA", (W * k, H * k), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        for s in strokes:
            pts = [(x * k, y * k) for x, y in s]
            if len(pts) > 1:
                d.line(pts, fill=rgb() + (255,), width=7, joint="curve")
            for x, y in (pts[0], pts[-1]):
                d.ellipse([x - 3, y - 3, x + 3, y + 3], fill=rgb() + (255,))
        box = im.getchannel("A").getbbox()
        return im.crop((box[0] - 8, box[1] - 8, box[2] + 8, box[3] + 8)) if box else None

    def refresh(_=None):
        preview.delete("all")
        im = typed_picture()
        if im:
            im.thumbnail((W - 20, H - 50))
            keep["p"] = ImageTk.PhotoImage(im, master=preview)
            preview.create_image(W // 2 + 2, (H - 30) // 2 + 2, image=keep["p"])
    entry.bind("<KeyRelease>", refresh)
    fcb.bind("<<ComboboxSelected>>", refresh)
    cb.bind("<<ComboboxSelected>>", refresh)

    def show():
        if mode.get() == "draw":
            type_box.pack_forget()
            draw_box.pack()
        else:
            draw_box.pack_forget()
            type_box.pack(fill="x")
            entry.focus_set()
            refresh()

    def clear():
        strokes.clear()
        draw_box.delete("all")
        guide()
        name.set("")
        refresh()

    def ok(_=None):
        im = drawn_picture() if mode.get() == "draw" else typed_picture()
        if im is None:
            dialog("Master PDF", "Sign in the box with the mouse first (or type your name)."
                   if mode.get() == "draw" else "Type your name first.", parent=win, sound="error")
            return
        result[0] = im
        win.destroy()
    buttons = tk.Frame(body, bg=BG)
    buttons.pack(fill="x", padx=12, pady=(8, 10))
    xp_button(buttons, "Clear", clear).pack(side="left")
    for text, cmd in (("Cancel", win.destroy), ("OK", ok)):
        b = xp_button(buttons, text, cmd)
        b.config(width=10)
        b.pack(side="right", padx=(6, 0))
    show()
    win.bind("<Escape>", lambda e: win.destroy())
    show_dialog(win, owner)
    return result[0]


def link_dialog(owner, pages, uri="", page=None):
    """Where a link goes: a web page, or a page of this PDF. Returns ("uri", address) or
    ("page", number from 0), or None if cancelled."""
    win, chrome, body = new_dialog(owner, "Link")
    result = [None]
    kind = tk.StringVar(value="page" if page is not None else "uri")
    box = tk.LabelFrame(body, text=" The link goes to ", bg=BG, font=FONT, padx=8, pady=6)
    box.pack(fill="x", padx=12, pady=(10, 6))
    tk.Radiobutton(box, text="A web page:", variable=kind, value="uri", bg=BG,
                   activebackground=BG, font=FONT).grid(row=0, column=0, sticky="w")
    address = tk.StringVar(value=uri or "https://")
    e1 = tk.Entry(box, textvariable=address, font=FONT, width=34, relief="sunken", bd=2, bg="white")
    e1.grid(row=0, column=1, sticky="w", padx=(4, 0), pady=2)
    tk.Radiobutton(box, text="A page of this PDF:", variable=kind, value="page", bg=BG,
                   activebackground=BG, font=FONT).grid(row=1, column=0, sticky="w")
    row = tk.Frame(box, bg=BG)
    row.grid(row=1, column=1, sticky="w", padx=(4, 0), pady=2)
    number = tk.StringVar(value=str((page or 0) + 1))
    e2 = tk.Entry(row, textvariable=number, font=FONT, width=5, relief="sunken", bd=2, bg="white")
    e2.pack(side="left")
    tk.Label(row, text=f"of {pages}", bg=BG, font=FONT).pack(side="left", padx=(4, 0))
    e1.bind("<FocusIn>", lambda e: kind.set("uri"))
    e2.bind("<FocusIn>", lambda e: kind.set("page"))

    def ok(_=None):
        if kind.get() == "uri":
            text = address.get().strip()
            if not text or text == "https://":
                dialog("Master PDF", "Type the web page's address.", parent=win, sound="error")
                return
            if not re.match(r"^[a-zA-Z][\w+.-]*:", text):
                text = "https://" + text
            result[0] = ("uri", text)
        else:
            try:
                n = int(number.get())
                assert 1 <= n <= pages
            except (ValueError, AssertionError):
                dialog("Master PDF", f"Type a page number from 1 to {pages}.", parent=win,
                       sound="error")
                return
            result[0] = ("page", n - 1)
        win.destroy()
    buttons = tk.Frame(body, bg=BG)
    buttons.pack(anchor="e", padx=12, pady=(4, 10))
    for text, cmd in (("OK", ok), ("Cancel", win.destroy)):
        b = xp_button(buttons, text, cmd)
        b.config(width=10)
        b.pack(side="left", padx=(6, 0))
    win.bind("<Return>", ok)
    win.bind("<Escape>", lambda e: win.destroy())
    show_dialog(win, owner, focus=e1 if kind.get() == "uri" else e2)
    return result[0]


def documents_folder():
    folder = os.path.join(os.path.expanduser("~"), "Documents")
    return folder if os.path.isdir(folder) else os.path.expanduser("~")


def remembered_folder():
    """The folder last chosen in a file box (Documents at first)."""
    folder = load_settings().get("last_folder")
    return folder if folder and os.path.isdir(folder) else documents_folder()


def fit_file_box(owner, tries=60):
    """Windows' file box remembers its size per program - once maximized, it opens filling
    the screen every time. So as soon as it shows, it's put back to a normal size, in the
    middle of the app's window (Windows then remembers that size instead)."""
    if sys.platform != "win32":
        return
    try:
        import ctypes
        from ctypes import wintypes
        u = ctypes.windll.user32
        boxes = []

        @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
        def look(h, _):
            name, pid = ctypes.create_unicode_buffer(16), wintypes.DWORD()
            u.GetClassNameW(h, name, 16)
            u.GetWindowThreadProcessId(h, ctypes.byref(pid))
            if name.value == "#32770" and pid.value == os.getpid() and u.IsWindowVisible(h):
                boxes.append(h)
            return True
        u.EnumWindows(look, 0)
        if boxes:
            top = owner.winfo_toplevel()
            sw, sh = top.winfo_screenwidth(), top.winfo_screenheight()
            w, h = min(860, sw - 80), min(560, sh - 80)
            x = top.winfo_rootx() + (top.winfo_width() - w) // 2
            y = top.winfo_rooty() + (top.winfo_height() - h) // 3
            x, y = min(max(0, x), sw - w), min(max(0, y), sh - h - 40)
            for box in boxes:
                if u.IsZoomed(box):
                    u.ShowWindow(box, 9)  # SW_RESTORE
                    u.SetWindowPos(box, 0, x, y, w, h, 0x0014)  # SWP_NOZORDER | SWP_NOACTIVATE
            return
    except Exception:
        return
    if tries:  # (not showing yet: look again in a moment)
        owner.after(50, fit_file_box, owner, tries - 1)


def file_dialog(kind, parent, **options):
    """Windows' file box - kind "open", "open_many", "save" or "folder" - opening in the
    folder last chosen (unless told another), at a normal size, and remembering the folder
    chosen this time. Returns what filedialog's ask... returns."""
    ask = {"open": filedialog.askopenfilename, "open_many": filedialog.askopenfilenames,
           "save": filedialog.asksaveasfilename, "folder": filedialog.askdirectory}[kind]
    options.setdefault("initialdir", remembered_folder())
    parent.after(30, fit_file_box, parent)
    result = ask(parent=parent, **options)
    if result:
        first = result if isinstance(result, str) else result[0]
        folder = first if kind == "folder" else os.path.dirname(first)
        if os.path.isdir(folder):
            save_settings(last_folder=os.path.normpath(folder))
    return result


def set_entry(entry, on):
    """A text box on, or greyed out like Windows 98's: the face colour behind, grey text.
    (Called again after a theme change: these colours aren't recoloured by themselves.)"""
    if on:
        entry.config(state="normal", fg="black", bg="white")
    else:
        entry.config(state="readonly", fg="#999999", readonlybackground=BG)


class JobDialog:
    """A Windows 98 dialog box for a job on the open PDF: its options in a group box, then a
    progress bar, a status line, and Start / Close buttons. The job works on a copy of the
    document, on a worker thread - Close turns into Cancel while it runs."""
    TITLE, START = "", "Start"

    def __init__(self, app):
        self.app = app
        self.base = app.doc_base()
        self.folder = os.path.dirname(app.path) if app.path else documents_folder()
        self.busy, self._cancel = False, False
        self.win, self.chrome, body = new_dialog(app, self.TITLE)
        self.chrome.on_close = self.close
        opt = tk.LabelFrame(body, text=" Options ", bg=BG, font=FONT, padx=8, pady=6)
        opt.pack(fill="x", padx=10, pady=(10, 8))
        self.build(opt)
        self.progress = ClassicProgress(body)
        self.progress.pack(fill="x", padx=10)
        self.status = tk.Label(body, text="Ready.", bg=BG, font=FONT, anchor="w")
        self.status.pack(fill="x", padx=10, pady=(4, 0))
        row = tk.Frame(body, bg=BG)
        row.pack(anchor="e", padx=10, pady=(8, 10))
        self.start_btn = xp_button(row, self.START, self.start)
        self.start_btn.config(width=10)
        self.start_btn.pack(side="left", padx=(0, 6))
        self.close_btn = xp_button(row, "Close", self.close)
        self.close_btn.config(width=10)
        self.close_btn.pack(side="left")
        tk.Frame(body, bg=BG, width=380, height=0).pack()
        self.win.bind("<Escape>", lambda e: self.close())
        show_dialog(self.win, app, focus=self.start_btn)

    def folder_row(self, opt, row, label="Save to:"):
        tk.Label(opt, text=label, bg=BG, font=FONT).grid(row=row, column=0, sticky="w")
        self.folder_label = tk.Label(opt, text=self.folder, bg="white", font=FONT,
                                     relief="sunken", bd=2, anchor="w", width=30)
        self.folder_label.grid(row=row, column=1, sticky="w", padx=6, pady=2)
        xp_button(opt, "Browse...", self.pick_folder).grid(row=row, column=2)

    def pick_folder(self):
        d = file_dialog("folder", self.win, title="Choose a folder",
                                    initialdir=self.folder)
        if d:
            self.folder = os.path.normpath(d)
            self.folder_label.config(text=self.folder)

    # ---- what's particular to each job ----
    def build(self, opt):
        raise NotImplementedError

    def settings(self):
        return {}

    def problem(self, s):
        """Why these settings can't be used (said in a message box), or None."""
        return None

    def work(self, doc, s, step, written):
        """Off the main thread: do the job on doc (a copy). step(0..1) raises Cancelled when
        Cancel is pressed; each file made goes in written. Returns the message for the end."""
        raise NotImplementedError

    # ---- running ----
    def start(self):
        if self.busy:
            return
        s = self.settings()
        problem = self.problem(s)
        if problem:
            dialog("Master PDF", problem, parent=self.win, sound="error")
            return
        with PDF_LOCK:
            data = self.app.doc.tobytes()  # (with any changes not saved yet)
        s["password"] = self.app.password
        self.busy, self._cancel = True, False
        self.start_btn.config(state="disabled")
        self.close_btn.config(text="Cancel")
        self.progress.config(maximum=100, value=0)
        self.status.config(text="Working...")
        events = queue.Queue()
        threading.Thread(target=self.worker, args=(data, s, events), daemon=True).start()
        self.win.after(100, self.poll, events)

    def worker(self, data, s, events):
        written = []

        def step(frac):
            if self._cancel:
                raise Cancelled
            events.put(("frac", frac))
        try:
            with PDF_LOCK:
                doc = pymupdf.open(stream=data, filetype="pdf")
                if doc.needs_pass:
                    doc.authenticate(s["password"] or "")
            try:
                msg = self.work(doc, s, step, written)
                events.put(("done", msg, written))
            finally:
                with PDF_LOCK:
                    doc.close()
        except Cancelled:
            events.put(("cancelled", None, written))
        except Exception as e:
            events.put(("error", str(e) or e.__class__.__name__, written))

    def poll(self, events):
        try:
            while True:
                kind, value, *rest = events.get_nowait()
                if kind == "frac":
                    self.progress.config(value=value * 100)
                    continue
                self.finish(kind, value, rest[0])
                return
        except queue.Empty:
            pass
        self.win.after(100, self.poll, events)

    def finish(self, kind, value, written):
        self.busy = False
        self.start_btn.config(state="normal")
        self.close_btn.config(text="Close")
        if kind == "cancelled":
            self.progress.config(value=0)
            self.status.config(text=f"Cancelled. {len(written)} file(s) saved before stopping.")
        elif kind == "error":
            self.status.config(text="It didn't work - see the message.")
            dialog("Master PDF", f"Couldn't finish:\n{value}", parent=self.win, sound="error")
        else:
            self.progress.config(value=100)
            self.status.config(text=value)
            if written:
                if dialog("Master PDF", value, ("Show files", "OK"), parent=self.win,
                          sound="done") == "Show files":
                    reveal_all(written)
            else:
                dialog("Master PDF", value, parent=self.win, sound="error")

    def close(self):
        if self.busy:
            self._cancel = True  # (the job stops at its next step)
        else:
            self.win.destroy()


class ExportDialog(JobDialog):
    """File > Export as pictures: pages as PNG / JPEG ... pictures, one per page."""
    TITLE, START = "Export as pictures", "Export"

    def build(self, opt):
        tk.Label(opt, text="Format:", bg=BG, font=FONT).grid(row=0, column=0, sticky="w")
        self.fmt = tk.StringVar(value="PNG")
        ttk.Combobox(opt, textvariable=self.fmt, values=list(EXPORT_FORMATS), state="readonly",
                     width=8).grid(row=0, column=1, sticky="w", padx=6, pady=2)
        tk.Label(opt, text="Resolution:", bg=BG, font=FONT).grid(row=1, column=0, sticky="w")
        self.dpi = tk.StringVar(value="150 dpi")
        ttk.Combobox(opt, textvariable=self.dpi, values=EXPORT_DPI, state="readonly",
                     width=8).grid(row=1, column=1, sticky="w", padx=6, pady=2)
        tk.Label(opt, text="Quality:", bg=BG, font=FONT).grid(row=2, column=0, sticky="w")
        self.quality = tk.IntVar(value=90)
        self.qscale = TrackBar(opt, self.quality, from_=0, to=100, length=200)
        self.qscale.grid(row=2, column=1, sticky="w", padx=6)
        self.qnote = EngravedLabel(opt)
        self.qnote.place(in_=self.qscale, relx=1.0, rely=1.0, x=6, y=-4, anchor="sw")
        tk.Label(opt, text="Pages:", bg=BG, font=FONT).grid(row=3, column=0, sticky="nw", pady=2)
        col = tk.Frame(opt, bg=BG)
        col.grid(row=3, column=1, columnspan=2, sticky="w", padx=6)
        self.which = tk.StringVar(value="all")
        self.range_var = tk.StringVar(value=f"1-{self.app.doc.page_count}")
        for text, val in ((f"All {self.app.doc.page_count} pages", "all"),
                          (f"Just this page ({self.app.current + 1})", "current")):
            tk.Radiobutton(col, text=text, variable=self.which, value=val, bg=BG,
                           activebackground=BG, font=FONT,
                           command=self.on_change).pack(anchor="w")
        row = tk.Frame(col, bg=BG)
        row.pack(anchor="w")
        tk.Radiobutton(row, text="Pages:", variable=self.which, value="range", bg=BG,
                       activebackground=BG, font=FONT, command=self.on_change).pack(side="left")
        self.range_entry = tk.Entry(row, textvariable=self.range_var, font=FONT, width=16,
                                    relief="sunken", bd=2, bg="white")
        self.range_entry.pack(side="left", padx=(4, 0))
        self.folder_row(opt, 4)
        self.fmt.trace_add("write", lambda *_: self.on_change())
        self.on_change()

    def on_change(self):
        lossy = self.fmt.get() in LOSSY_PICTURES
        self.qscale.config(state="normal" if lossy else "disabled")
        self.qnote.config(text="" if lossy else f"(not used by {self.fmt.get()})")
        set_entry(self.range_entry, self.which.get() == "range")

    def settings(self):
        return {"fmt": self.fmt.get(), "dpi": int(self.dpi.get().split()[0]),
                "quality": max(self.quality.get(), 1), "which": self.which.get(),
                "range": self.range_var.get(), "current": self.app.current,
                "folder": self.folder, "base": self.base}

    def problem(self, s):
        if s["which"] == "range":
            try:
                parse_ranges(s["range"])
            except ValueError as e:
                return str(e)
        return None

    def work(self, doc, s, step, written):
        n = doc.page_count
        if s["which"] == "all":
            pages = list(range(n))
        elif s["which"] == "current":
            pages = [s["current"]]
        else:
            pages = []
            for a, b in parse_ranges(s["range"]):
                pages += [p - 1 for p in range(a, min(n, b or n) + 1) if p - 1 not in pages]
            if not pages:
                raise ValueError(f"the PDF has only {n} page{'s' * (n != 1)}")
        ext, fmt = EXPORT_FORMATS[s["fmt"]], s["fmt"]
        for k, p in enumerate(pages):
            step(k / len(pages))
            with PDF_LOCK:
                im = render_page(doc[p], dpi=s["dpi"])
            name = f"{s['base']} - page {p + 1}" if n > 1 else s["base"]
            dst = unique_path(s["folder"], name, ext)
            extra = {} if fmt == "WEBP" else {"dpi": (s["dpi"], s["dpi"])}
            if fmt in LOSSY_PICTURES:
                extra["quality"] = s["quality"]
            im.save(dst, fmt, **extra)
            written.append(dst)
        return f"Saved {len(written)} picture{'s' * (len(written) != 1)}."


class SplitDialog(JobDialog):
    """File > Split: the PDF into single pages, parts of a few pages each, or just the pages
    typed in - each part its own PDF."""
    TITLE, START = "Split", "Split"

    def build(self, opt):
        tk.Label(opt, text="Split into:", bg=BG, font=FONT).grid(row=0, column=0, sticky="w")
        self.mode = tk.StringVar(value=SPLIT_MODES[0])
        ttk.Combobox(opt, textvariable=self.mode, values=SPLIT_MODES, state="readonly",
                     width=17).grid(row=0, column=1, sticky="w", padx=6, pady=2)
        tk.Label(opt, text="Pages:", bg=BG, font=FONT).grid(row=1, column=0, sticky="w")
        self.pages_var = tk.StringVar()
        self.pages_entry = tk.Entry(opt, textvariable=self.pages_var, font=FONT, width=20,
                                    relief="sunken", bd=2, bg="white")
        self.pages_entry.grid(row=1, column=1, sticky="w", padx=6, pady=2)
        self.pnote = EngravedLabel(opt)
        self.pnote.place(in_=self.pages_entry, relx=1.0, rely=0.5, x=6, anchor="w")
        self.folder_row(opt, 2)
        self._typed = {"Every few pages": "2", "Page ranges": "1-3, 5, 8-"}  # (kept per mode)
        self._mode_was = None
        self.mode.trace_add("write", lambda *_: self.on_change())
        self.on_change()

    def on_change(self):
        mode = self.mode.get()
        if self._mode_was in self._typed:  # each mode keeps what was typed for it
            self._typed[self._mode_was] = self.pages_var.get()
        self._mode_was = mode
        set_entry(self.pages_entry, mode != "Single pages")
        self.pages_var.set(self._typed.get(mode, ""))
        self.pnote.config(text={"Single pages": "(every page its own file)",
                                "Every few pages": "(how many pages in each file)",
                                "Page ranges": "(for example 1-3, 5, 8-)"}[mode])

    def settings(self):
        return {"mode": self.mode.get(), "text": self.pages_var.get().strip(),
                "folder": self.folder, "base": self.base}

    def problem(self, s):
        if s["mode"] == "Every few pages" and (not s["text"].isdigit() or int(s["text"]) < 1):
            return "Type how many pages each file gets (a number, like 2)."
        if s["mode"] == "Page ranges":
            try:
                parse_ranges(s["text"])
            except ValueError as e:
                return str(e)
        return None

    def work(self, doc, s, step, written):
        n = doc.page_count
        if s["mode"] == "Single pages":
            parts = [(p, p) for p in range(1, n + 1)]
        elif s["mode"] == "Every few pages":
            k = int(s["text"])
            parts = [(a, min(a + k - 1, n)) for a in range(1, n + 1, k)]
        else:
            parts = []
            for a, b in parse_ranges(s["text"]):
                if a > n:
                    raise ValueError(f"the PDF has only {n} page{'s' * (n != 1)}, so it has no page {a}")
                parts.append((a, n if b is None else min(b, n)))
        for k, (a, b) in enumerate(parts):
            step(k / len(parts))
            label = f"page {a}" if a == b else f"pages {a}-{b}"
            dst = unique_path(s["folder"], f"{s['base']} - {label}", ".pdf")
            with PDF_LOCK:
                part = pymupdf.open()
                try:
                    part.insert_pdf(doc, from_page=a - 1, to_page=b - 1)
                    part.save(dst, **SAVE_OPTS)
                finally:
                    part.close()
            written.append(dst)
        return f"Split into {len(written)} PDF file{'s' * (len(written) != 1)}."


class CompressDialog(JobDialog):
    """File > Compress: a smaller copy of the PDF - its pictures brought down to the chosen
    resolution and saved again as JPEG, the file cleaned up and packed tighter. The PDF
    itself isn't changed."""
    TITLE, START = "Compress", "Compress"

    def build(self, opt):
        tk.Label(opt, text="Pictures:", bg=BG, font=FONT).grid(row=0, column=0, sticky="w")
        self.level = tk.StringVar(value="Balanced (150 dpi)")
        ttk.Combobox(opt, textvariable=self.level, values=list(COMPRESS_LEVELS),
                     state="readonly", width=26).grid(row=0, column=1, sticky="w", padx=6, pady=2)
        tk.Label(opt, text="Quality:", bg=BG, font=FONT).grid(row=1, column=0, sticky="w")
        self.quality = tk.IntVar(value=70)
        self.qscale = TrackBar(opt, self.quality, from_=0, to=100, length=200)
        self.qscale.grid(row=1, column=1, sticky="w", padx=6)
        self.qnote = EngravedLabel(opt)
        self.qnote.place(in_=self.qscale, relx=1.0, rely=1.0, x=6, y=-4, anchor="sw")
        self.strip = tk.BooleanVar(value=False)
        tk.Checkbutton(opt, text="Remove the document's details (title, author, program...)",
                       variable=self.strip, bg=BG, activebackground=BG, font=FONT
                       ).grid(row=2, column=0, columnspan=3, sticky="w", pady=(2, 2))
        self.folder_row(opt, 3)
        self.level.trace_add("write", lambda *_: self.on_change())
        self.on_change()

    def on_change(self):
        used = COMPRESS_LEVELS[self.level.get()] is not None
        self.qscale.config(state="normal" if used else "disabled")
        self.qnote.config(text="" if used else "(not used: the pictures are kept)")

    def settings(self):
        app = self.app
        before = (os.path.getsize(app.path) if app.path and not app.dirty
                  and os.path.exists(app.path) else None)
        return {"dpi": COMPRESS_LEVELS[self.level.get()], "quality": max(self.quality.get(), 1),
                "strip": self.strip.get(), "folder": self.folder, "base": self.base,
                "before": before}

    def work(self, doc, s, step, written):
        step(0.05)
        dst = unique_path(s["folder"], f"{s['base']} (compressed)", ".pdf")
        with PDF_LOCK:
            before = s["before"] or len(doc.tobytes(garbage=3, deflate=True))
            if s["dpi"]:  # pictures sharper than about 1.3 x the target are brought down to it
                doc.rewrite_images(dpi_threshold=round(s["dpi"] * 1.3), dpi_target=s["dpi"],
                                   quality=s["quality"])
            if s["strip"]:
                doc.set_metadata({})
                doc.del_xml_metadata()
        step(0.7)
        with PDF_LOCK:
            doc.save(dst, garbage=4, deflate=True, deflate_images=True, deflate_fonts=True,
                     clean=True, use_objstms=1)
        after = os.path.getsize(dst)
        if after >= before:  # no smaller: don't leave a pointless copy
            os.remove(dst)
            return ("This PDF is already as small as it can be made with these settings. "
                    "Try a smaller setting for the pictures.")
        written.append(dst)
        return (f"Saved {os.path.basename(dst)}: {short_size(before)} > {short_size(after)} "
                f"({round(100 - after * 100 / before)}% smaller).")


TOOLS = [("move", "Move", "Move: drag to move around the pages; click a text, note or drawing to "
                          "pick it up - drag it to move it, Delete to remove it, double-click to "
                          "change it. Click the button again to put it down (then the mouse "
                          "selects text)."),
         ("edit", "Edit text & pictures", "Edit text & pictures: click a line of the PDF's own "
                                          "text to pick it up - drag its round handles to make "
                                          "it bigger or smaller, drag it to move it - and click "
                                          "it again to retype it (in its own font). Drag a "
                                          "picture to move it, and a corner to resize it. "
                                          "Delete removes what's picked up; right-click for "
                                          "more (replace or save a picture, add one)."),
         ("text", "Add text", "Add text: click on the page where the text goes, then type."),
         ("highlight", "Marker", "Marker: drag to draw anywhere on the page like a highlighter "
                                 "pen (hold Shift for a straight line). The bar under the tools "
                                 "sets its colour, thickness and how see-through it is. (To "
                                 "highlight text, select it with no tool chosen and use the "
                                 "little toolbar over it.)"),
         ("pen", "Draw", "Draw: drag on the page to draw freehand."),
         ("eraser", "Eraser", "Eraser: choose with the little arrow. White-out: drag a box to "
                              "cover what's there with a white (or any colour) box. Eraser: rub "
                              "it over pen and marker drawings to take out what it passes over "
                              "(a shape it touches goes) - the size in the bar under the tools."),
         ("image", "Image", "Image: click where a picture goes, or drag a box for it. The "
                            "little arrow chooses a picture file or the one you copied."),
         ("shape", "Shape", "Shape: drag on the page to draw a rectangle, an ellipse, a line or "
                            "an arrow - choose which with the little arrow."),
         ("sign", "Sign", "Sign: click where your signature goes. The little arrow lets you "
                          "make a new one - drawn with the mouse or typed in a handwriting font."),
         ("link", "Link", "Link: drag a box over what should become a link - to a web page or "
                          "to a page of this PDF. Right-click a link to change or delete it.")]
TOOL_VARIANTS = {  # the tools with a list: (what it's called, its key)
    "eraser": [("White-out (cover with a box)", "whiteout"), ("Eraser (rub out drawings)", "rub")],
    "image": [("Picture from a file...", "file"), ("Picture you copied (paste)", "clipboard")],
    "shape": [("Rectangle", "rect"), ("Ellipse", "ellipse"), ("Line", "line"), ("Arrow", "arrow")],
    "sign": [("Place my signature", "place"), ("New signature...", "new")],
}
TOOL_HINTS = {
    "select": "Drag across text to select it (then copy or mark it). Choose a tool above to "
              "add or change things.",
    "move": "Drag to move around. Click a text, note or drawing to pick it up.",
    "edit": "Click a line of text or a picture to pick it up; click the text again to retype it.",
    "text": "Click on the page where the text goes.",
    "highlight": "Draw with the marker anywhere on the page (hold Shift for a straight line).",
    "pen": "Drag on the page to draw.",
    "eraser": "White-out: drag a box to cover. Eraser: rub over drawings to take them out.",
    "image": "Click where the picture goes, or drag a box for it.",
    "shape": "Drag on the page to draw the shape.",
    "sign": "Click where your signature goes.",
    "link": "Drag a box over what should become a link. Right-click a link to change it.",
}
TEXT_SIZES = ["8", "9", "10", "11", "12", "14", "16", "18", "20", "24", "28", "36", "48"]


class App(BaseTk):
    MIN_SIZE = (780, 520)

    def __init__(self):
        super().__init__()
        self.title("Master PDF")
        self.set_icon()
        # the app's font everywhere: widgets without a font of their own (the dropdown boxes
        # and their lists) use Tk's default fonts, which are Segoe UI on Windows
        for name in ("TkDefaultFont", "TkTextFont", "TkMenuFont", "TkHeadingFont",
                     "TkCaptionFont", "TkSmallCaptionFont", "TkIconFont", "TkTooltipFont"):
            tkfont.nametofont(name).configure(family=FONT[0], size=FONT[1])
        global CAPTION_ACTIVE, SOUNDS_ON, SHOW_HELP
        settings = load_settings()
        self.palette_name, self.custom_color = theme_look(settings)[1:]
        CAPTION_ACTIVE = palette_colors(self.palette_name, self.custom_color)
        SOUNDS_ON = settings.get("sounds", True)
        SHOW_HELP = settings.get("whats_this", True)
        self.minsize(*self.MIN_SIZE)
        self.center_on_screen(1040, 760)
        self.configure(bg=BG)
        # classic navy title bar + 3D border; everything else goes inside chrome.body
        self.chrome = ClassicWindow(self, "Master PDF", self.on_close, min_size=self.MIN_SIZE)
        self.apply_icon()
        content = self.chrome.body
        self.style_ttk()  # dropdowns: classic shapes, the same in every theme
        self.brush_icon = brush_icon()  # (the Theme list's Custom item: go to Settings)
        self.icons = make_icons()

        # the open document
        self.doc, self.path, self.name, self.password = None, None, None, None
        self.dirty = False
        self.undo_stack, self.redo_stack = [], []  # (the document as it was, page, what)
        self.page_sizes, self.current = [], 0  # each page's size as shown, in points
        self.tool = "select"  # (no tool: the mouse selects text)
        self.tool_colors = {"text": "#000000", "highlight": "#FFFF00", "pen": "#FF0000",
                            "eraser": "#FFFFFF", "shape": "#FF0000",
}
        self.tool_sizes = {"text": 12, "pen": 2, "shape": 2, "highlight": 12,  # (the marker's)
                           "eraser": 16}  # (the eraser's size)
        self.variant = {tool: kinds[0][1] for tool, kinds in TOOL_VARIANTS.items()}
        self.text_font = "Arial"  # (Add text's font)
        self.text_style = {"bold": False, "italic": False, "underline": False, "strike": False,
                           "highlight": False, "align": "left", "dir": "ltr"}  # (Add text's look)
        self.text_highlight_color = "#FFFF00"  # (the text highlight button's colour)
        self.tool_opacity = {"highlight": 100, "marker": 60, "pen": 100, "shape": 100}  # (%)
        self._props_loading = False
        self.find_hits, self.find_pos, self.find_text = [], -1, ""
        self._render_job = None
        self.settings_win = None
        self._objects = {}  # page -> its text lines and pictures (the Edit tool's), as found
        self._words = {}  # page -> its words, as found
        self._chars = {}  # page -> its letters (for selecting text), as found
        self._para = {}  # (page, baseline) -> (left, right) of the paragraph a line was in
        self._frames = {}  # page -> the frames Master PDF laid out: each its lines' places
        self._owners = {}  # page -> {letter (char_key): the piece of text it belongs to}
        self._boxes = {}  # (page, piece) -> a frame's box, as its handles made it
        self._claims = {}  # page -> [(piece, boxes)]: text just written there, for that piece
        self._next_oid = 0
        self._wraps = {}  # page -> [{"xref", "rect", "mode"}]: each picture's layout (LAYOUTS)
        self._wrapped = {}  # page -> the frames laid out round pictures (see new_record)
        self._outlines = {}  # picture's xref -> image_outline
        self._links = {}  # page -> its links, as found
        # the installed fonts are looked up in the background (the Edit tool writes with them)
        self._font_scan = threading.Thread(target=scan_fonts, daemon=True)
        self._font_scan.start()

        # menu bar, then the toolbar, like Acrobat (and every Windows 98 program)
        self.menubar = MenuBar(content, [
            ("File", "menu", self.file_menu), ("Edit", "menu", self.edit_menu),
            ("View", "menu", self.view_menu), ("Pages", "menu", self.pages_menu),
            ("Theme", "list", self.theme_menu), ("Help", "menu", self.help_menu)])
        self.menubar.pack(fill="x", padx=2, pady=(1, 0))
        # etched line under it, like classic Windows' menu bars - drawn the same way as the
        # group boxes' line (a 2 px groove), so it looks the same in every theme
        self.menu_line = tk.Frame(content, bg="#EBE8D7", height=2, bd=2, relief="groove")
        self.menu_line.pack(fill="x", padx=2, pady=(1, 0))
        self.build_toolbar(content)
        etched_line(content, padx=2)
        self.build_statusbar(content)  # (packed before the middle, so it keeps its room)
        middle = tk.Frame(content, bg=BG)
        middle.pack(fill="both", expand=True, padx=2, pady=(2, 0))
        self.sidebar = ThumbBar(middle, self)
        self.sidebar.pack(side="left", fill="y")
        self.splitter = Splitter(middle, self.sidebar, self.set_side_width)
        self.splitter.pack(side="left", fill="y")
        self.view = DocView(middle, self)
        self.view.pack(side="left", fill="both", expand=True)
        width = settings.get("side_width")
        if isinstance(width, int) and width != self.sidebar.width:
            self.sidebar.set_width(max(ThumbBar.MIN_WIDTH, min(width, 600)))
        self.show_thumbs = settings.get("thumbnails", True)
        if not self.show_thumbs:
            self.sidebar.pack_forget()
            self.splitter.pack_forget()

        self.help_mode = WhatsThis(self, self.chrome, self.whats_this_note)
        if HAS_DND:  # drop PDFs (or pictures) anywhere on the window
            for w in (self, self.view.canvas, self.sidebar.canvas):
                w.drop_target_register(DND_FILES)
                w.dnd_bind("<<Drop>>", self.on_drop)
        self.bind_keys()
        # (the toolbar over selected text goes with the window when it's moved)
        self.bind("<Configure>", lambda e: e.widget is self and self.view.place_text_bar(),
                  add="+")
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.color_dropdown_lists()  # the same selection blue from the start
        self.theme_choice = saved_theme(settings)  # the theme chosen last time (Windows 98 at first)
        self.apply_look(*theme_look(settings, self.theme_choice))
        self.set_tool("select")
        self.tint_tools()
        if settings.get("start_maximized"):
            self.after_idle(self.chrome.toggle_maximize)
        self.after(800, self.startup_update_tasks)
        # opened with a file (double-clicked, "Open with", or dropped on the program)
        if len(sys.argv) > 1 and os.path.isfile(sys.argv[1]):
            self.after(300, lambda: self.open_path(sys.argv[1], asked=True))

    # ---- the toolbar and the status bar ----
    def build_toolbar(self, parent):
        bar = self.toolbar = tk.Frame(parent, bg=BG)
        bar.pack(fill="x", padx=4, pady=2)
        self.buttons = {}

        def add(key, icon, tip, cmd, note=None):
            b = ToolButton(bar, self.icons[icon] if isinstance(icon, str) else icon, tip, cmd, note)
            b.pack(side="left")
            self.buttons[key] = b
            b.whiten = WHITE_IN_DARK.get(key)  # (white in the dark theme)
            b.draw()
            return b
        add("open", "open", "Open (Ctrl+O)", self.open_dialog, "Opens a PDF.")
        add("save", "save", "Save (Ctrl+S)", self.save,
            "Saves the changes into the PDF. (File > Save As saves them as a new file.)")
        tool_separator(bar)
        add("undo", "undo", "Undo (Ctrl+Z)", self.undo, "Takes back the last change.")
        add("redo", "redo", "Redo (Ctrl+Y)", self.redo, "Puts back the change you took back.")
        tool_separator(bar)
        add("prev", "up", "Previous page (Page Up)", lambda: self.step_page(-1),
            "Goes to the page before.")
        add("next", "down", "Next page (Page Down)", lambda: self.step_page(1),
            "Goes to the next page.")
        self.page_var = tk.StringVar()
        self.page_entry = tk.Entry(bar, textvariable=self.page_var, width=4, font=FONT,
                                   relief="sunken", bd=2, bg="white", justify="right")
        self.page_entry.pack(side="left", padx=(4, 3))
        self.page_entry.bind("<Return>", lambda e: self.go_to_typed())
        self.page_entry.bind("<FocusOut>", lambda e: self.update_ui())
        self.count_label = tk.Label(bar, text="/ 0", bg=BG, font=FONT)
        self.count_label.pack(side="left", padx=(0, 2))
        tool_separator(bar)
        add("zoom_out", "zoom_out", "Zoom out (Ctrl+-)", lambda: self.zoom_step(-1),
            "Makes the pages smaller.")
        self.zoom_var = tk.StringVar()
        self.zoom_cb = ttk.Combobox(bar, textvariable=self.zoom_var, values=ZOOMS, width=9,
                                    height=len(ZOOMS))  # (the whole list, no scrolling)
        self.zoom_cb.pack(side="left", padx=2)
        self.zoom_cb.bind("<<ComboboxSelected>>", lambda e: self.zoom_typed())
        self.zoom_cb.bind("<Return>", lambda e: self.zoom_typed())
        add("zoom_in", "zoom_in", "Zoom in (Ctrl++)", lambda: self.zoom_step(1),
            "Makes the pages bigger.")
        # Find, at the right end like Acrobat's
        self.find_btn = ToolButton(bar, self.icons["find"], "Find next (Enter)", self.find_next,
                                   "Finds the next place the words typed beside it are.")
        self.find_btn.pack(side="right")
        self.find_var = tk.StringVar()
        self.find_entry = tk.Entry(bar, textvariable=self.find_var, width=18, font=FONT,
                                   relief="sunken", bd=2, bg="white")
        self.find_entry.pack(side="right", padx=(2, 1))
        self.find_entry.bind("<Return>", lambda e: self.find_next(1))
        self.find_entry.bind("<Shift-Return>", lambda e: self.find_next(-1))
        self.find_entry.bind("<Escape>", lambda e: (self.find_var.set(""), "break")[1])
        self.find_var.trace_add("write", lambda *_: self.find_soon())  # (as you type)
        tk.Label(bar, text="Find:", bg=BG, font=FONT).pack(side="right", padx=(8, 0))

        # the second toolbar: the tools, in groups like Windows 98's - then what they use
        etched_line(parent, padx=2)
        bar = self.toolbar2 = tk.Frame(parent, bg=BG)
        bar.pack(fill="x", padx=4, pady=2)
        groups = [("move", "edit"), ("text", "highlight", "pen"),
                  ("eraser", "image", "shape"), ("sign", "link")]
        info = {key: (tip, note) for key, tip, note in TOOLS}
        for n, group in enumerate(groups):
            if n:
                tool_separator(bar)
            for key in group:
                tip, note = info[key]
                b = ToolButton(bar, self.icons.get(key + "_tool", self.icons[key]), tip,
                               lambda k=key: self.set_tool(k), note,
                               menu=(lambda b, k=key: self.tool_menu(k, b)) if key in TOOL_VARIANTS else None)
                b.pack(side="left")
                self.buttons[key] = b
                b.whiten = WHITE_IN_DARK.get(key)  # (white in the dark theme)
                b.draw()
        b = ToolButton(bar, self.icons["annots"], "Annotations", self.show_annotations,
                       "Shows the list of annotations (text, highlights, drawings, notes) at "
                       "the left - click one to go to it.")
        b.pack(side="left")
        self.buttons["annots"] = b

        # the third toolbar: what the chosen tool - or what's picked up - can change
        etched_line(parent, padx=2)
        self.build_props(parent)

    WIDTHS = {"pen": (1, 20, 1), "shape": (1, 20, 1), "highlight": (4, 40, 2),
              "eraser": (4, 60, 2)}  # (lo, hi, step; the eraser's: how big it is)
    ALIGNS = (("left", "Align left"), ("center", "Centre"), ("right", "Align right"),
              ("justify", "Justify"))
    CASES = (("Sentence case.", lambda s: "\n".join(x[:1].upper() + x[1:].lower()
                                                     for x in s.split("\n"))),
             ("lowercase", str.lower), ("UPPERCASE", str.upper),
             ("Capitalize Each Word", lambda s: re.sub(r"[^\W_]+", lambda m: m[0][:1].upper()
                                                       + m[0][1:].lower(), s)),
             ("tOGGLE cASE", str.swapcase))

    def build_props(self, parent):
        """The third toolbar (like Acrobat's properties bar): it changes with the tool - the
        font, size, bold, colours and alignment for text; colour, thickness and see-through
        sliders for the highlighter, pen and shapes; what can be done to a picture picked
        up; otherwise a word on what the tool does."""
        row = self.props = tk.Frame(parent, bg=BG, height=ToolButton.H + 4)
        row.pack(fill="x", padx=4, pady=(1, 2))
        row.pack_propagate(False)  # (the same height whatever it shows)
        self.props_title = tk.Label(row, text="", bg=BG, font=FONT)
        self.props_title.pack(side="left", padx=(2, 6))
        self.prop_groups = {}

        def group(name):
            f = self.prop_groups[name] = tk.Frame(row, bg=BG)
            return f

        def button(parent, key, icon, tip, command, note=None, menu=None, label=None):
            b = ToolButton(parent, icon, tip, command, note, menu=menu, label=label)
            b.pack(side="left")
            self.buttons[key] = b
            b.whiten = WHITE_IN_DARK.get(key)  # (white in the dark theme)
            b.draw()
            return b

        # text: like Word's Font group, in one row
        f = group("text")
        self.font_var = tk.StringVar(value=self.text_font)
        self.font_cb = ttk.Combobox(f, textvariable=self.font_var, width=22, state="readonly")
        self.font_cb.pack(side="left", padx=(0, 2))
        self.font_cb.bind("<<ComboboxSelected>>", lambda e: self.font_chosen())
        self.font_cb.bind("<Button-1>", lambda e: self.fill_fonts(), add="+")
        self.size_var = tk.StringVar()
        self.size_cb = ttk.Combobox(f, textvariable=self.size_var, values=TEXT_SIZES, width=4)
        self.size_cb.pack(side="left", padx=(0, 2))
        self.size_cb.bind("<<ComboboxSelected>>", lambda e: self.size_typed())
        self.size_cb.bind("<Return>", lambda e: self.size_typed())
        button(f, "grow", self.icons["fmt_grow"], "Bigger text", lambda: self.step_text_size(1),
               "Makes the text one size bigger.")
        button(f, "shrink", self.icons["fmt_shrink"], "Smaller text",
               lambda: self.step_text_size(-1), "Makes the text one size smaller.")
        tool_separator(f)
        for key, tip in (("bold", "Bold"), ("italic", "Italic"), ("underline", "Underline"),
                         ("strike", "Strikethrough")):
            button(f, "fmt_" + key, self.icons["fmt_" + key], tip,
                   lambda k=key: self.toggle_style(k),
                   f"{tip}: on or off for the text you add - or for the line picked up with "
                   f"the Edit tool.")
        tool_separator(f)
        button(f, "text_color", format_icon("color", "#000000"), "Text colour", self.pick_color,
               "The text's colour. Click to choose another.")
        button(f, "text_highlight", format_icon("marker", self.text_highlight_color),
               "Highlight the text", lambda: self.toggle_style("highlight"),
               "Highlights the text (on or off). The little arrow chooses the colour.",
               menu=self.highlight_menu)
        tool_separator(f)
        button(f, "case", self.icons["fmt_case"], "Change case",
               lambda: self.case_menu(self.buttons["case"]),
               "Changes the text to UPPERCASE, lowercase, Sentence case...")
        self.align_group = tk.Frame(f, bg=BG)
        self.align_group.pack(side="left")
        tool_separator(self.align_group)
        for kind, tip in self.ALIGNS:
            button(self.align_group, "align_" + kind, self.icons["align_" + kind], tip,
                   lambda k=kind: self.set_align(k), f"{tip}: how the lines of the text you add "
                   f"line up - or the line picked up with the Edit tool, in its paragraph "
                   f"(which end stays put when it gets longer or shorter).")
        tool_separator(self.align_group)
        for kind, tip in (("ltr", "Left-to-right text"), ("rtl", "Right-to-left text")):
            button(self.align_group, "dir_" + kind, self.icons["dir_" + kind], tip,
                   lambda k=kind: self.set_direction(k), f"{tip}, like Word's: right-to-left "
                   f"(Arabic...) runs from the right and lines up on the right.")

        # drawing: the colour, how thick, how see-through
        f = group("color")
        button(f, "color", swatch_icon("#000000"), "Colour", self.pick_color,
               "The colour of what you add next (highlights, drawings, shapes, notes...). "
               "Click to choose another.")
        for name, label in (("width", "Thickness:"), ("opacity", "Opacity:")):
            f = group(name)
            tool_separator(f)
            title = tk.Label(f, text=label, bg=BG, font=FONT)
            title.pack(side="left", padx=(0, 2))
            setattr(self, name + "_title", title)
            var = tk.IntVar(value=0)
            bar = SlimTrackBar(f, var, 1, 20, length=110, step=1) if name == "width" else \
                SlimTrackBar(f, var, 10, 100, length=110, step=10)
            bar.pack(side="left")
            value = tk.Label(f, text="", bg=BG, font=FONT, width=6, anchor="w")
            value.pack(side="left", padx=(3, 0))
            setattr(self, name + "_var", var)
            setattr(self, name + "_bar", bar)
            setattr(self, name + "_label", value)
            var.trace_add("write", lambda *_, n=name: self.prop_slid(n))

        # a picture picked up with the Edit tool
        f = group("picture")
        button(f, "pic_replace", self.icons["image"], "Replace picture",
               lambda: self.view.edit_sel and self.replace_picture(*self.view.edit_sel),
               "Puts another picture in its place.", label="Replace...")
        button(f, "pic_save", self.icons["save"], "Save picture",
               lambda: self.view.edit_sel and self.save_picture(self.view.edit_sel[1]),
               "Saves the picture as a file.", label="Save as...")
        button(f, "pic_delete", self.icons["eraser"], "Delete picture", self.delete_object,
               "Takes the picture off the page (Delete).", label="Delete")

        # nothing to set: what the tool does
        f = group("hint")
        self.props_hint = EngravedLabel(f, text="", anchor="w")
        self.props_hint.pack(side="left")

    def build_statusbar(self, parent):
        bar = tk.Frame(parent, bg=BG)
        bar.pack(side="bottom", fill="x", padx=2, pady=2)

        def panel(width, expand=False):
            outer, inner = sunken_panel(bar)
            outer.pack(side="left", fill="x", expand=expand, padx=(0, 2))
            label = tk.Label(inner, text="", bg=BG, font=FONT, anchor="w", padx=3, width=width)
            label.pack(fill="both", expand=True)
            return label
        self.msg_label = panel(1, expand=True)  # (width 1: long messages don't widen the window)
        self.page_label = panel(14)
        self.size_label = panel(24)
        self.zoom_label = panel(6)

    def say(self, text):
        """A message in the status bar."""
        self.msg_label.config(text=text)

    def update_ui(self):
        """The title, the buttons that can be used now, and the page / zoom numbers."""
        doc, n = self.doc, self.doc.page_count if self.doc else 0
        title = f"{self.name}{' *' if self.dirty else ''} - Master PDF" if doc else "Master PDF"
        if self.chrome.title != title:
            self.chrome.title = title
            self.title(title)
            self.chrome.draw()
        b = self.buttons
        b["save"].set_enabled(bool(doc))
        b["undo"].set_enabled(bool(self.undo_stack))
        b["redo"].set_enabled(bool(self.redo_stack))
        b["prev"].set_enabled(bool(doc) and self.current > 0)
        b["next"].set_enabled(bool(doc) and self.current < n - 1)
        for key in ("zoom_in", "zoom_out", "annots") + tuple(k for k, _, _ in TOOLS):
            b[key].set_enabled(bool(doc))
        self.find_btn.set_enabled(bool(doc))
        if bool(self.props_title.cget("text")) != bool(doc):  # (a PDF opened or closed)
            self.refresh_props()
        if self.focus_get() is not self.page_entry:
            self.page_var.set(str(self.current + 1) if doc else "")
        self.count_label.config(text=f"/ {n}")
        if self.focus_get() is not self.zoom_cb:
            z = self.view.zoom
            self.zoom_var.set(z if isinstance(z, str) else f"{round(z)}%")
        self.page_label.config(text=f"Page {self.current + 1} of {n}" if doc else "")
        if doc and self.page_sizes:
            w, h = self.page_sizes[min(self.current, n - 1)]
            name = page_size_text(w, h)
            mm = f"{w * 25.4 / 72:.0f} x {h * 25.4 / 72:.0f} mm"
            self.size_label.config(text=name if name.endswith("mm") else f"{name}  ({mm})")
        else:
            self.size_label.config(text="")
        self.zoom_label.config(text=f"{self.view.zoom_percent()}%" if doc else "")

    def start_screen(self, parent):
        """With no PDF open: a raised box saying how to start, with buttons (placed in the
        middle of the view by the view)."""
        panel = RaisedEdge(parent)
        inner = tk.Frame(panel.inner, bg=BG, padx=20, pady=14)
        inner.pack()
        try:
            self._start_logo = ImageTk.PhotoImage(
                Image.open(resource_path("icon.png")).convert("RGBA").resize((64, 64), Image.NEAREST))
            tk.Label(inner, image=self._start_logo, bg=BG).pack(pady=(0, 8))
        except Exception:
            pass
        tk.Label(inner, text="No PDF is open", bg=BG, font=(FONT[0], 10, "bold")).pack()
        tk.Label(inner, text="Open a PDF, or drag one onto this window.", bg=BG,
                 font=FONT).pack(pady=(4, 10))
        row = tk.Frame(inner, bg=BG)
        row.pack()
        xp_button(row, "Open...", self.open_dialog).pack(side="left", padx=4)
        xp_button(row, "Create PDF from files...", self.create_from_files).pack(side="left", padx=4)
        return panel

    # ---- the menus ----
    def file_menu(self):
        has = self.doc is not None
        on = lambda cmd: cmd if has else None  # noqa: E731 (greyed out with no PDF open)
        items = [("Open...", self.open_dialog),
                 ("Create PDF from files...", self.create_from_files), None,
                 ("Save", on(self.save)), ("Save As...", on(self.save_as)), None,
                 ("Export as pictures...", on(lambda: ExportDialog(self))),
                 ("Split...", on(lambda: SplitDialog(self))),
                 ("Compress...", on(lambda: CompressDialog(self))), None,
                 ("Close", on(self.close_doc))]
        recent = [p for p in load_settings().get("recent", []) if os.path.exists(p)]
        if recent:  # the last few PDFs, numbered like Windows 98 programs did
            items.append(None)
            items += [(f"{n}  {os.path.basename(p)}", lambda p=p: self.open_path(p))
                      for n, p in enumerate(recent, 1)]
        return items + [None, ("Exit", self.on_close)]

    def edit_menu(self):
        has = self.doc is not None
        return [("Undo", self.undo if self.undo_stack else None),
                ("Redo", self.redo if self.redo_stack else None), None,
                ("Delete", self.delete_annot if self.view.selected else
                 self.delete_object if self.view.edit_sel else None), None,
                ("Edit text & pictures",
                 (lambda: self.set_tool("edit")) if has else None),
                ("Add picture...", self.add_picture if has else None), None,
                ("Find...", self.focus_find if has else None), None,
                ("Settings...", self.open_settings)]

    def view_menu(self):
        has = self.doc is not None
        on = lambda cmd: cmd if has else None  # noqa: E731
        return [("Zoom in", on(lambda: self.zoom_step(1))),
                ("Zoom out", on(lambda: self.zoom_step(-1))), None,
                ("Actual size", on(lambda: self.set_zoom(100))),
                ("Fit page", on(lambda: self.set_zoom("Fit page"))),
                ("Fit width", on(lambda: self.set_zoom("Fit width"))), None,
                ("Page thumbnails", self.toggle_thumbs, self.show_thumbs), None,
                ("First page", on(lambda: self.set_current(0))),
                ("Last page", on(lambda: self.set_current(self.doc.page_count - 1)))]

    def pages_menu(self):
        has = self.doc is not None
        on = lambda cmd: cmd if has else None  # noqa: E731
        sel = lambda: self.sidebar.selection()  # noqa: E731
        return [("Rotate left", on(lambda: self.rotate_pages(sel(), -90))),
                ("Rotate right", on(lambda: self.rotate_pages(sel(), 90))), None,
                ("Insert pages from file...", on(self.insert_from_file)),
                ("Insert blank page", on(self.insert_blank)),
                ("Extract pages...", on(lambda: self.extract_pages(sel()))), None,
                ("Delete pages", on(lambda: self.delete_pages(sel())))]

    def theme_menu(self):
        return [(THEME_NAMES[t], lambda t=t: self.pick_theme(t), t == self.theme_choice)
                for t in THEME_NAMES] + [
            None, ("Custom", lambda: self.pick_theme(CUSTOM_THEME),
                   self.theme_choice == CUSTOM_THEME,
                   # in use: a clickable brush that opens Settings, where Custom is edited
                   (self.brush_icon, self.open_settings))]

    def help_menu(self):
        return [("What's This?", self.help_mode.start), None,
                ("Check for updates...", self.check_updates_dialog),
                ("About Master PDF", self.about)]

    def page_menu(self, x, y):
        """Right-click on the page thumbnails."""
        menu = PopupMenu(self)
        for item in self.pages_menu():
            if item is None:
                menu.add_separator()
            else:
                menu.add_command(*item)
        menu.tk_popup(x, y)

    # ---- the keyboard ----
    def bind_keys(self):
        def typing():  # (the keys keep their usual jobs in text boxes)
            return isinstance(self.focus_get(), (tk.Entry, tk.Text, ttk.Entry))

        shortcuts = {}  # (Windows key code, Shift held) -> (action, in_text): Ctrl + a letter

        def key(seq, action, in_text=False):
            self.bind(seq, lambda e: None if (typing() and not in_text) else (action(), "break")[1])
            m = re.fullmatch(r"<Control-([A-Za-z])>", seq)
            if m:
                shortcuts[(ord(m[1].upper()), m[1].isupper())] = (action, in_text)

        # With another keyboard layout (Arabic...) the keys type other letters, and Tk matches
        # shortcuts by the letter - so Ctrl + Z wouldn't undo. The key itself is looked at
        # instead (in text boxes too: copy, paste, cut, select all, undo, redo).
        edits = {0x43: "<<Copy>>", 0x56: "<<Paste>>", 0x58: "<<Cut>>", 0x41: "<<SelectAll>>",
                 0x5A: "<<Undo>>", 0x59: "<<Redo>>"}

        def by_key(e):
            if len(e.keysym) == 1 and e.keysym.isascii():
                return None  # (a Latin letter: the usual bindings did it)
            if typing() and e.keycode in edits:
                e.widget.event_generate(edits[e.keycode])
                return "break"
            found = shortcuts.get((e.keycode, bool(e.state & 1)))
            if found and (found[1] or not typing()):
                found[0]()
                return "break"
            return None
        self.bind("<Control-KeyPress>", by_key)
        key("<Control-o>", self.open_dialog, True)
        key("<Control-s>", self.save, True)
        key("<Control-S>", self.save_as, True)
        key("<Control-w>", self.close_doc, True)
        key("<Control-z>", self.undo)
        key("<Control-y>", self.redo)
        key("<Control-f>", self.focus_find, True)
        for seq in ("<Control-plus>", "<Control-equal>", "<Control-KP_Add>"):
            key(seq, lambda: self.zoom_step(1), True)
        for seq in ("<Control-minus>", "<Control-KP_Subtract>"):
            key(seq, lambda: self.zoom_step(-1), True)
        key("<Control-Key-0>", lambda: self.set_zoom("Fit page"), True)
        key("<Prior>", lambda: self.view.canvas.yview_scroll(-1, "pages"))
        key("<Next>", lambda: self.view.canvas.yview_scroll(1, "pages"))
        key("<Home>", lambda: self.doc and self.set_current(0))
        key("<End>", lambda: self.doc and self.set_current(self.doc.page_count - 1))
        key("<Delete>", self.on_delete_key)
        for name, dx, dy in (("Left", -1, 0), ("Right", 1, 0), ("Up", 0, -1), ("Down", 0, 1)):
            for shift in (False, True):  # (the arrows: what's picked up moved - Shift: further)
                self.bind(f"<{'Shift-' if shift else ''}{name}>",
                          lambda e, dx=dx, dy=dy, s=shift: None if typing() else self.nudge(dx, dy, s))
        key("<Control-c>", self.copy_selection)
        key("<Control-v>", lambda: self.tool == "edit" and self.paste_object())
        key("<F4>", self.toggle_thumbs, True)
        key("<F2>", lambda: self.edit_line_selected() and self.view.edit_line(*self.edit_line_selected()))
        self.bind("<Escape>", lambda e: None if typing() else (
            self.view.select_object(None) if self.view.edit_sel else
            self.view.clear_text_sel() if self.view.text_sel else
            self.view.select(None) if self.view.selected else self.set_tool("select")))

    NUDGE = 1.0  # (points an arrow key moves what's picked up - with Shift, ten times as far)

    def nudge(self, dx, dy, far=False):
        """An arrow key: the text, picture or annotation picked up moved a little that way.
        Shown at once as an outline; moved when the keys stop for a moment (so a key held
        down glides it, and it's one change for Undo). "break" if there was something to move."""
        v = self.view
        if v.editor:  # (typing: the arrows move the cursor)
            return None
        if self.tool == "edit" and v.edit_sel:
            i, obj = v.edit_sel
            what = ("object", i, obj, pymupdf.Rect(obj.get("box") or obj["rect"]))
        elif v.selected:
            info = v.selected_info()
            if info is None:
                return None
            what = ("annot", v.selected[0], v.selected[1], info[1])
        else:
            return None
        n = getattr(self, "_nudge", None)
        if not n or n["what"][:3] != what[:3]:
            if n and n.get("job"):  # (something else picked up meanwhile: the last one moved)
                self.after_cancel(n["job"])
                self.end_nudge()
            n = self._nudge = {"what": what, "d": pymupdf.Point(0, 0), "job": None}
        step = self.NUDGE * (10 if far else 1)
        n["d"] = n["d"] + (dx * step, dy * step)
        i, rect = what[1], what[3]
        x0, y0, x1, y1 = v.to_canvas(i, rect + (n["d"].x, n["d"].y, n["d"].x, n["d"].y))
        c = v.canvas
        c.delete("nudge")
        c.create_rectangle(x0 - 2, y0 - 2, x1 + 2, y1 + 2, outline="#000000", dash=(1, 1),
                           tags=("nudge", "overlay"))
        if n["job"]:
            self.after_cancel(n["job"])
        n["job"] = self.after(450, self.end_nudge)
        self.say(f"Moving it {abs(n['d'].x):g} pt {'right' if n['d'].x >= 0 else 'left'}, "
                 f"{abs(n['d'].y):g} pt {'down' if n['d'].y >= 0 else 'up'} (Shift + arrow: 10 pt).")
        return "break"

    def end_nudge(self):
        n, self._nudge = getattr(self, "_nudge", None), None
        self.view.canvas.delete("nudge")
        if not n or (n["d"].x == 0 and n["d"].y == 0):
            return
        kind, i, target, _rect = n["what"]
        with PDF_LOCK:  # (the page's own direction: as dragging does)
            page = self.doc[i]
            d = n["d"] * page.derotation_matrix - pymupdf.Point(0, 0) * page.derotation_matrix
        if kind == "object":
            self.move_object(i, target, d)
        else:
            self.move_annot(i, target, d)

    def on_delete_key(self):
        """Delete: the annotation (or the Edit tool's text / picture) picked up - or, on the
        thumbnails, the selected pages."""
        if self.view.selected:
            self.delete_annot()
        elif self.view.edit_sel and self.tool == "edit":
            self.delete_object()
        elif self.focus_get() is self.sidebar.canvas and self.sidebar.selected:
            self.delete_pages(sorted(self.sidebar.selected))

    # ---- opening and saving ----
    def doc_base(self):
        return os.path.splitext(self.name or "Untitled")[0]

    def open_dialog(self):
        """File > Open: the PDF chosen - with one open already, you choose: in this window
        (its unsaved changes asked about first) or in a new window, beside it."""
        path = file_dialog("open", self, title="Open", filetypes=PDF_TYPES,
                                          initialdir=self.last_folder())
        if not path:
            return
        if self.doc:
            choice = dialog("Open", f"Open {os.path.basename(path)} in this window (instead of "
                            f"{self.name}), or in a new window?",
                            ("This window", "New window", "Cancel"))
            if choice == "New window":
                self.open_in_new_window(path)
                return
            if choice != "This window" or not self.maybe_save():
                return
        self.open_path(path, asked=True)

    def open_in_new_window(self, path):
        """Another Master PDF window, with the PDF at path open in it."""
        try:
            if getattr(sys, "frozen", False):
                args = [sys.executable, path]
            else:  # (run as a script: the same Python - without a console window)
                py = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
                args = [py if os.path.isfile(py) else sys.executable,
                        os.path.abspath(__file__), path]
            subprocess.Popen(args, cwd=app_folder(), close_fds=True,
                             creationflags=getattr(subprocess, "DETACHED_PROCESS", 0))
            self.say(f"Opening {os.path.basename(path)} in a new window...")
        except Exception as e:
            dialog("Master PDF", f"Couldn't open a new window:\n{e}", sound="error")

    def last_folder(self):
        return remembered_folder()

    def open_path(self, path, asked=False):
        """Open the PDF at path (asked: unsaved changes were already asked about)."""
        if not asked and not self.maybe_save():
            return
        path = os.path.normpath(path)
        name = os.path.basename(path)
        try:
            with open(path, "rb") as f:  # (read whole: the file stays free to be saved over)
                data = f.read()
            with PDF_LOCK:
                doc = pymupdf.open(stream=data, filetype="pdf")
        except Exception as e:
            dialog("Master PDF", f"Couldn't open {name}:\n{e}", sound="error")
            return
        password = None
        while doc.needs_pass:
            password = text_dialog(self, "Password", f"{name} is protected by a password.\n"
                                   "Type the password to open it:", multiline=False, secret=True)
            if password is None:
                return
            if not doc.authenticate(password):
                dialog("Master PDF", "That password isn't right.", sound="error")
        if doc.page_count == 0:
            dialog("Master PDF", f"{name} has no pages to show.", sound="error")
            return
        self.set_doc(doc, path, name, password=password)
        self.add_recent(path)
        self.say(f"Opened {name}.")

    def set_doc(self, doc, path, name, dirty=False, password=None):
        old = self.doc
        self._para = {}
        self._frames = {}
        self._owners, self._claims, self._boxes = {}, {}, {}
        self._wraps, self._wrapped, self._outlines = {}, {}, {}
        self.doc, self.path, self.name, self.dirty = doc, path, name, dirty
        self.password = password
        self.undo_stack.clear()
        self.redo_stack.clear()
        self.find_hits, self.find_text = [], ""
        if getattr(self, "find_var", None) is not None and self.find_var.get().strip():
            self.find_soon(jump=False)  # (the results kept up to date)
        self.view.selected = None
        self.view.edit_sel = self.view.hover = None
        self.view.zoom = "Fit width"
        self.sidebar.selected, self.sidebar.last_click = set(), None
        for lst in self.sidebar.lists.values():  # (a new PDF's bookmarks start closed)
            lst["filled"] = False
        self.current = 0
        self.refresh_all(keep=False)
        self.view.canvas.yview_moveto(0)
        self.view.canvas.xview_moveto(0)
        self.sidebar.canvas.yview_moveto(0)
        self.say_tool()
        if old is not None:
            with PDF_LOCK:
                old.close()

    def add_recent(self, path):
        recent = [p for p in load_settings().get("recent", []) if os.path.normcase(p) != os.path.normcase(path)]
        save_settings(recent=([path] + recent)[:5])

    def maybe_save(self):
        """Unsaved changes: ask whether to save them first. False: don't go on (cancelled,
        or the save didn't work)."""
        if not self.doc or not self.dirty:
            return True
        choice = dialog("Master PDF", f"Do you want to save the changes to {self.name}?",
                        ("Yes", "No", "Cancel"))
        if choice == "Yes":
            return self.save()
        return choice == "No"

    def save(self):
        if not self.doc:
            return False
        return self.write(self.path) if self.path else self.save_as()

    def save_as(self):
        if not self.doc:
            return False
        folder = os.path.dirname(self.path) if self.path else self.last_folder()
        path = file_dialog("save", self, title="Save As", filetypes=PDF_TYPES,
                                            defaultextension=".pdf", initialdir=folder,
                                            initialfile=self.doc_base() + ".pdf")
        if not path:  # (Windows' box itself asks before replacing a file)
            return False
        path = os.path.normpath(path)
        if not self.write(path):
            return False
        self.path, self.name = path, os.path.basename(path)
        self.add_recent(path)
        self.update_ui()
        return True

    def write(self, path):
        """Save the document to path: written next to it first, then put in its place - so a
        failed save never leaves a broken file."""
        tmp = path + ".saving"
        try:
            with PDF_LOCK:
                self.doc.save(tmp, **SAVE_OPTS)
            os.replace(tmp, path)
        except Exception as e:
            try:
                os.remove(tmp)
            except OSError:
                pass
            dialog("Master PDF", f"Couldn't save {os.path.basename(path)}:\n{e}\n\n"
                   "(Is it open in another program?)", sound="error")
            return False
        self.dirty = False
        self.update_ui()
        self.say(f"Saved {os.path.basename(path)} ({fmt_size(os.path.getsize(path))}).")
        return True

    def close_doc(self):
        if not self.doc or not self.maybe_save():
            return
        with PDF_LOCK:
            self.doc.close()
        self.doc, self.path, self.name, self.dirty = None, None, None, False
        self.undo_stack.clear()
        self.redo_stack.clear()
        self.view.selected = None
        self.refresh_all(keep=False)
        self.say_tool()

    def insert_files(self, doc, paths, at):
        """Put the pages of PDFs (and pictures, a page each) into doc, from page at on.
        Returns (how many pages went in, the problems)."""
        added, problems = 0, []
        for p in paths:
            try:
                if is_pdf(p):
                    src = open_pdf(p)
                else:
                    src = pymupdf.open()
                    add_picture_page(src, p)
                try:
                    n = src.page_count
                    doc.insert_pdf(src, start_at=at + added)
                finally:
                    src.close()
                added += n
            except Exception as e:
                problems.append(f"{os.path.basename(p)}: {e}")
        return added, problems

    def from_word(self, paths, then):
        """Any Word documents among paths made into PDFs (by Word, in the background, a
        progress bar meanwhile) - then then(paths, problems), each Word document's place
        taken by its PDF (one that couldn't be made: left out, and said why)."""
        docs = [p for p in paths if is_word(p)]
        if not docs:
            then(list(paths), [])
            return
        self._converted = tempfile.mkdtemp(prefix="MasterPDF-word-")
        made, problems = {}, []
        win, chrome, body = new_dialog(self, "Converting")
        job = {"cancel": False}

        def cancel():  # (X or Esc: stopped - nothing is added)
            if not job["cancel"]:
                job["cancel"] = True
                label.config(text="Cancelling...")
        chrome.on_close = cancel
        win.bind("<Escape>", lambda e: cancel())
        label = tk.Label(body, text="", bg=BG, font=FONT, anchor="w")
        label.pack(fill="x", padx=12, pady=(12, 6))
        bar = ClassicProgress(body, maximum=len(docs))
        bar.pack(fill="x", padx=12, pady=(0, 14))
        tk.Frame(body, bg=BG, width=340, height=0).pack()
        center_dialog(win, self)
        win.grab_set()
        events = queue.Queue()
        engine = "Microsoft Word" if has_word() else "LibreOffice"
        now = {"n": 0, "since": time.time()}  # (the document being made, since when)

        def work():  # (off the main thread: Word takes a few seconds)
            for n, src in enumerate(docs):
                events.put(("now", (n, src)))
                dst = os.path.join(self._converted, f"{n} " + os.path.splitext(os.path.basename(src))[0] + ".pdf")
                try:
                    word_to_pdf(src, dst, job)
                    made[src] = dst
                except Cancelled:
                    events.put(("cancelled", None))
                    return
                except Exception as e:
                    problems.append(f"{os.path.basename(src)}: {e}")
            events.put(("done", None))

        def poll():
            try:
                while True:
                    kind, value = events.get_nowait()
                    if kind == "now":
                        n, src = value
                        now.update(n=n, since=time.time())
                        label.config(text=f"Making a PDF of {os.path.basename(src)} with {engine}..."
                                     + (f"   ({n + 1} of {len(docs)})" if len(docs) > 1 else ""))
                    elif kind == "cancelled":
                        win.destroy()
                        self.clean_converted()
                        self.say("Cancelled - nothing was added.")
                        return
                    else:
                        finish(bar._value)
                        return
            except queue.Empty:
                pass
            # (Word doesn't say how far it's got: the bar fills steadily as it works - fast at
            # first, then slower, never quite full - and jumps on when a document is done)
            if not job["cancel"]:
                part = 1 - math.exp(-(time.time() - now["since"]) / 5.0)
                bar.config(value=now["n"] + min(0.95, part))
            self.after(100, poll)
        def finish(start, step=0):
            """Done: the bar filled the rest of the way (a third of a second), seen full for a
            moment - then the PDF."""
            steps = 10
            if step < steps:
                s = (step + 1) / steps
                bar.config(value=start + (len(docs) - start) * (1 - (1 - s) ** 2))
                self.after(30, lambda: finish(start, step + 1))
                return
            if step == steps:
                self.after(250, lambda: finish(start, step + 1))  # (full, for a moment)
                return
            win.destroy()
            then([made.get(p, p) for p in paths if not is_word(p) or p in made], problems)
        threading.Thread(target=work, daemon=True).start()
        self.after(50, poll)

    def clean_converted(self):
        """The PDFs made from Word documents: gone, once their pages are in."""
        folder, self._converted = getattr(self, "_converted", None), None
        if folder:
            import shutil
            shutil.rmtree(folder, ignore_errors=True)

    def create_from_files(self, paths=None):
        """File > Create PDF from files: PDFs and pictures put together into a new PDF (not
        saved until Save)."""
        if not self.maybe_save():
            return
        if paths is None:
            paths = file_dialog("open_many", self, title="Create PDF from files",
                                                filetypes=FILE_TYPES, initialdir=self.last_folder())
        paths = [p for p in paths if p.lower().endswith(PDF_EXTS + WORD_EXTS + IMAGE_EXTS)]
        if not paths:
            return
        self.from_word(paths, lambda files, left: self.create_from(paths, files, left))

    def create_from(self, paths, files, problems):
        """Create PDF from files, once any Word documents are PDFs (files: what to put in)."""
        if not files:
            if problems:
                dialog("Some files were left out", "\n".join(problems[:10]), sound="error")
            return
        with PDF_LOCK:
            doc = pymupdf.open()
            added, more = self.insert_files(doc, files, 0)
        problems = problems + more
        self.clean_converted()
        if problems:
            dialog("Some files were left out", "\n".join(problems[:10]), sound="error")
        if not added:
            return
        base = os.path.splitext(os.path.basename(paths[0]))[0]
        name = f"{base} (combined).pdf" if len(paths) > 1 else f"{base}.pdf"
        self.set_doc(doc, None, name, dirty=True)
        self.say(f"Made a new PDF from {len(paths)} file(s), {added} page(s). Save it with File > Save.")

    def on_drop(self, event):
        """Files dropped on the window: a PDF opens; with one already open, you choose: open
        it instead, or add its pages to the end."""
        paths = [p for p in self.tk.splitlist(event.data)
                 if p.lower().endswith(PDF_EXTS + WORD_EXTS + IMAGE_EXTS)]
        if not paths:
            self.say("Only PDFs, Word documents and pictures can be dropped here.")
            return event.action
        self.after(10, lambda: self.dropped(paths))  # (after the drop has finished)
        return event.action

    def dropped(self, paths):
        if not self.doc:
            if len(paths) == 1 and is_pdf(paths[0]):
                self.open_path(paths[0])
            else:
                self.create_from_files(paths)
            return
        what = os.path.basename(paths[0]) if len(paths) == 1 else f"these {len(paths)} files"
        choice = dialog("Master PDF", f"Add the pages of {what} to the end of {self.name}?\n"
                        "(Or open it on its own instead.)", ("Add pages", "Open", "Cancel"))
        if choice == "Add pages":
            self.insert_paths(paths, self.doc.page_count)
        elif choice == "Open":
            if len(paths) == 1 and is_pdf(paths[0]):
                self.open_path(paths[0])
            else:
                self.create_from_files(paths)

    # ---- changes, and taking them back ----
    def change(self, what, fn, pages=None, obj=None, meta=None):
        """Make a change to the document that Undo can take back: the document is kept as it
        was, fn changes it, and what changed is drawn again (pages: just these pages; None:
        everything, laid out again). Returns what fn returns."""
        if not self.doc:
            return None
        if self.view.editor and what not in ("add text", "change text"):
            self.view.close_editor(commit=True)  # (text still being typed goes on first)
        tag = obj.get("oid") if obj else None  # (the piece of text changed, if one is)
        meta = meta or self.wrap_state()  # (the pictures' layouts, as they were)
        with PDF_LOCK:
            before = self.doc.tobytes()
            try:
                with text_of(tag) as boxes:
                    result = fn()
            except Exception as e:  # half done: back to how it was
                self.doc.close()
                self.doc = self.reopen(before)
                failed = e
            else:
                failed = None
                if tag is not None and boxes:  # (what was written: that piece's)
                    for p in pages or ():
                        self._claims.setdefault(p, []).append((tag, list(boxes)))
        if failed is not None:
            self.refresh_all()
            dialog("Master PDF", f"Couldn't {what}:\n{failed}", sound="error")
            return None
        self.undo_stack.append((before, self.current, what, meta))
        del self.undo_stack[:-30]  # (the last 30 changes can be taken back)
        self.redo_stack.clear()
        self.dirty = True
        self.find_hits, self.find_text = [], ""  # (the pages may have moved)
        if self.find_var.get().strip():
            self.find_soon(jump=False)
        self._links.clear()
        self.view.drop_bars()  # (any floating toolbar too)
        if pages is None:
            self._objects.clear()
            self._words.clear()
            self._chars.clear()
        else:
            for p in pages:
                self._objects.pop(p, None)
                self._words.pop(p, None)
                self._chars.pop(p, None)
        self.sidebar.refresh_lists()  # (bookmarks and annotations may have changed)
        if pages is None:
            self.refresh_all()
        else:
            self.view.invalidate(pages)
            self.sidebar.invalidate(pages)
            self.update_ui()
        return result

    def reopen(self, data):
        doc = pymupdf.open(stream=data, filetype="pdf")
        if doc.needs_pass:
            doc.authenticate(self.password or "")
        return doc

    def undo(self):
        self.swap(self.undo_stack, self.redo_stack, "Undid")

    def redo(self):
        self.swap(self.redo_stack, self.undo_stack, "Redid")

    def swap(self, take, put, verb):
        if not take or not self.doc:
            return
        self.view.close_editor(commit=False)
        data, page, what, *meta = take.pop()
        self._frames = {}  # (the pages as they were: read as they are)
        with PDF_LOCK:
            put.append((self.doc.tobytes(), self.current, what, self.wrap_state()))
        if meta and meta[0] is not None:
            self.set_wrap_state(meta[0])
        with PDF_LOCK:
            self.doc.close()
            self.doc = self.reopen(data)
        self.dirty = True
        self.view.selected = None
        self.view.edit_sel = self.view.hover = None
        self.find_hits, self.find_text = [], ""
        if getattr(self, "find_var", None) is not None and self.find_var.get().strip():
            self.find_soon(jump=False)  # (the results kept up to date)
        self.current = min(page, self.doc.page_count - 1)
        self.refresh_all()
        self.say(f"{verb}: {what}.")

    def refresh_all(self, keep=True):
        """The pages' sizes again, and everything laid out and drawn again."""
        self.view.drop_bars()  # (any floating toolbar too)
        self.view._previews.clear()
        self._objects.clear()
        self._words.clear()
        self._chars.clear()
        self._links.clear()
        with PDF_LOCK:
            self.page_sizes = [(p.rect.width, p.rect.height) for p in self.doc] if self.doc else []
        if self.doc:
            self.current = min(self.current, self.doc.page_count - 1)
        self.view.layout(keep=keep)
        self.sidebar.layout()
        self.sidebar.refresh_lists()
        self.update_ui()

    # ---- drawing the pages, a few at a time so the window never stalls ----
    def schedule_render(self):
        if not self._render_job:
            self._render_job = self.after(15, self.render_some)

    def render_some(self):
        self._render_job = None
        if not self.doc:
            return
        job = self.view.next_render() or (self.show_thumbs and self.sidebar.next_render())
        if not job:
            return
        if not PDF_LOCK.acquire(blocking=False):  # a dialog's job has it: try again soon
            self._render_job = self.after(100, self.render_some)
            return
        try:
            job()
        except Exception:
            pass  # (a page that can't be drawn stays blank)
        finally:
            PDF_LOCK.release()
        self._render_job = self.after(1, self.render_some)

    # ---- pages: going to them, zooming ----
    def set_current(self, i, scroll=True):
        if not self.doc:
            return
        i = min(max(i, 0), self.doc.page_count - 1)
        changed = i != self.current
        self.current = i
        if scroll:
            self.view.go_to(i)
        if changed or scroll:
            self.sidebar.draw_marks()
            self.sidebar.see(i)
        self.update_ui()

    def step_page(self, d):
        self.set_current(self.current + d)

    def go_to_typed(self):
        try:
            self.set_current(int(self.page_var.get()) - 1)
        except ValueError:
            pass
        self.view.canvas.focus_set()

    def set_zoom(self, zoom, at=None):
        if self.doc:
            self.view.set_zoom(zoom, at=at)
            self.update_ui()

    def zoom_step(self, d, at=None):
        """One zoom step in or out - at the mouse (at: (x, y) in the view), or at the top."""
        if not self.doc:
            return
        now = self.view.zoom_percent()
        steps = [z for z in ZOOM_STEPS if (z > now + 1 if d > 0 else z < now - 1)]
        if steps:
            self.set_zoom(steps[0] if d > 0 else steps[-1], at=at)

    def zoom_typed(self):
        text = self.zoom_var.get().strip()
        if text in ("Fit page", "Fit width"):
            self.set_zoom(text)
        else:
            try:
                self.set_zoom(min(max(round(float(text.rstrip("% "))), ZOOM_STEPS[0]), ZOOM_STEPS[-1]))
            except ValueError:
                self.update_ui()
        self.view.canvas.focus_set()

    def toggle_thumbs(self):
        self.show_thumbs = not self.show_thumbs
        save_settings(thumbnails=self.show_thumbs)
        if self.show_thumbs:
            self.sidebar.pack(side="left", fill="y", before=self.view)
            self.splitter.pack(side="left", fill="y", before=self.view)
            self.sidebar.layout()
        else:
            self.sidebar.pack_forget()
            self.splitter.pack_forget()
        self.buttons["annots"].set_latched(self.show_thumbs and self.sidebar.view == "annotations")

    def set_side_width(self, width):
        """The splitter was dragged: the side panel takes the new width (remembered)."""
        self.sidebar.set_width(width)
        save_settings(side_width=width)

    # ---- tools ----
    def set_tool(self, tool, toggle=True):
        """Take up a tool - or, its button pressed again (toggle), put it down: then the mouse
        selects text ("select")."""
        if toggle and tool == self.tool:
            tool = "select"
        self.view.close_editor(commit=True)
        self.view.drop_bars()  # (any floating toolbar too)
        self.tool = tool
        self.view.edit_sel = self.view.hover = None
        self.view.draw_overlays()
        for key, _, _ in TOOLS:
            self.buttons[key].set_latched(key == tool)
        self.refresh_props()
        self.say_tool()
        self.update_ui()
        self.view.draw_overlays()  # (the Link tool shows where the links are)
        if tool == "sign" and not os.path.exists(SIGNATURE_FILE):
            self.after_idle(self.new_signature)  # (no signature yet: make one first)

    def prop_context(self):
        """What the third toolbar is about now."""
        tool = self.tool
        if not self.doc:
            return "hint"
        if tool == "text":
            return "text"
        if tool == "edit" and self.view.edit_sel:
            return "line" if self.view.edit_sel[1]["kind"] == "text" else "picture"
        if tool == "highlight":
            return "marker"
        if tool in ("pen", "shape"):
            return tool
        if tool == "eraser":
            return "whiteout" if self.variant["eraser"] == "whiteout" else "rub"
        return "hint"

    PROP_TITLES = {"text": "Add text:", "line": "Text:", "picture": "Picture:",
                   "marker": "Marker:", "pen": "Draw:",
                   "shape": "Shape:", "whiteout": "White-out:", "rub": "Eraser:"}
    PROP_GROUPS = {"text": ("text",), "line": ("text",), "picture": ("picture",),
                   "marker": ("color", "width", "opacity"),
                   "pen": ("color", "width", "opacity"), "shape": ("color", "width", "opacity"),
                   "whiteout": ("color",), "rub": ("width",), "hint": ("hint",)}

    def refresh_props(self):
        """The third toolbar shows what the tool (or what's picked up) can change."""
        if not hasattr(self, "props"):
            return
        ctx = self.prop_context()
        shown = self.PROP_GROUPS[ctx]
        if shown != getattr(self, "_shown_groups", None):  # (only when it changes: no flicker)
            self._shown_groups = shown
            for f in self.prop_groups.values():
                if f.winfo_manager():
                    f.pack_forget()
            for name in shown:
                self.prop_groups[name].pack(side="left")
        names = {key: name for key, name, _ in TOOLS}
        title = self.PROP_TITLES.get(ctx) or (names.get(self.tool, "Select") + ":"
                                              if self.doc else "")
        self.props_title.config(text=title)
        self._props_loading = True
        try:
            if ctx in ("text", "line"):
                self.fill_text_props(ctx)
            if "color" in shown:
                self.buttons["color"].set_icon(swatch_icon(self.color()))
            if "width" in shown:
                self.width_title.config(text="Size:" if ctx == "rub" else "Thickness:")
                key = self.tool
                self.width_bar.set_range(*self.WIDTHS[key])
                self.width_var.set(self.tool_sizes[key])
            if "opacity" in shown:
                self.opacity_var.set(self.tool_opacity[self.opacity_key()])
            if ctx == "hint":
                self.props_hint.config(text=TOOL_HINTS[self.tool] if self.doc else
                                       "Open a PDF to start.")
        finally:
            self._props_loading = False
        self.width_label.config(text=f"{self.width_var.get()} pt")
        self.opacity_label.config(text=f"{self.opacity_var.get()}%")

    def opacity_key(self):
        return "marker" if self.tool == "highlight" else self.tool

    def prop_slid(self, name):
        """A slider moved: the tool's thickness or how see-through it draws."""
        try:
            value = int(getattr(self, name + "_var").get())
        except (tk.TclError, ValueError):
            return
        if name == "width":
            self.width_label.config(text=f"{value} pt")
            if not self._props_loading and self.tool in self.WIDTHS:
                self.tool_sizes[self.tool] = value
        else:
            self.opacity_label.config(text=f"{value}%")
            if not self._props_loading and self.opacity_key() in self.tool_opacity:
                self.tool_opacity[self.opacity_key()] = value

    def opacity(self, key):
        return self.tool_opacity.get(key, 100) / 100

    def fill_text_props(self, ctx):
        """The text buttons show the line picked up (Edit) - or how added text will look."""
        line = self.edit_line_selected() if ctx == "line" else None
        looks = [None]
        if line and not self.editing_rich() and line[1].get("frame"):  # (a frame: all of it)
            looks = [st for t, st in self.frame_runs(*line) if t.strip()] or [None]
        if line and (self.editing_rich() or looks[0] is not None):
            if self.editing_rich():  # the letters selected in the box (all: if all)
                looks = self.selected_looks()
            first = looks[0]
            family, size, color = first[0], first[1], first[4]
            on = {key: all(st[STYLE_FIELDS.index(key)] for st in looks)
                  for key in ("bold", "italic", "underline", "strike", "highlight")}
            line = None
        elif line:
            i, obj = line
            span = main_span(obj)
            _path, family, bold, italic = find_font(span["font"], span["flags"])
            size, color = round(span["size"], 1), span_color(span)
            kinds = self.mark_kinds_over(i, [obj["rect"]])
            on = {"bold": bold, "italic": italic, "underline": "Underline" in kinds,
                  "strike": "StrikeOut" in kinds, "highlight": "Highlight" in kinds}
        else:
            family, size, color = self.text_font, self.size("text"), self.color("text")
            on = self.text_style
        if self.focus_get() is not self.font_cb:
            self.font_var.set(family)
        if self.focus_get() is not self.size_cb:
            self.size_var.set(str(int(size) if size % 1 == 0 else size))
        for key in ("bold", "italic", "underline", "strike"):
            self.buttons["fmt_" + key].set_latched(bool(on[key]))
        self.buttons["text_highlight"].set_latched(bool(on["highlight"]))
        self.buttons["text_highlight"].set_icon(format_icon("marker", self.text_highlight_color))
        self.buttons["text_color"].set_icon(format_icon("color", color))
        # lining up and direction: the text being added's - or the line's (as chosen while
        # it's typed, else as it's lined up on the page)
        ed = self.editing_rich()
        picked = self.edit_line_selected() if ctx == "line" else None
        if ctx == "text":
            align, direction = self.text_style["align"], self.text_style["dir"]
        elif picked:
            align = (ed and ed.get("align")) or self.line_alignment(*picked)
            direction = (ed and ed.get("dir")) or self.line_direction(picked[1])
        else:
            align = direction = None
        for kind, _tip in self.ALIGNS:
            self.buttons["align_" + kind].set_latched(align == kind)
        for kind in ("ltr", "rtl"):
            self.buttons["dir_" + kind].set_latched(direction == kind)

    # ---- the text buttons ----
    def picked_line(self):
        """The Edit tool's line, once any typing in it has gone on: (page, line) or None."""
        if self.view.editor and self.tool == "edit":
            self.view.close_editor(commit=True)
        return self.edit_line_selected()

    def toggle_style(self, key):
        """Bold, italic, underline, strikethrough or highlight: on or off - for the letters
        selected in the line being typed (or all of it), the line picked up, or the text
        being added."""
        if self.editing_rich():  # (on for all of them - unless they all have it already)
            field = {"bold": "bold", "italic": "italic", "underline": "underline",
                     "strike": "strike", "highlight": "highlight"}[key]
            k = STYLE_FIELDS.index(field)
            on = not all(st[k] for st in self.selected_looks())
            value = (self.text_highlight_color if on else None) if key == "highlight" else on
            self.restyle_letters(field, value)
            return
        if self.tool == "edit":
            line = self.picked_line()
            if not line:
                return
            i, obj = line
            if key in ("bold", "italic") and obj.get("frame"):  # (all of it on - or, all of it
                k = STYLE_FIELDS.index(key)  # had it, off: as the button shows it)
                on = not all(st[k] for text, st in self.frame_runs(i, obj) if text.strip())
                self.restyle_frame(i, obj, lambda st: st[:k] + (on,) + st[k + 1:])
                self.refresh_props()
                return
            if key in ("bold", "italic"):
                _path, _family, bold, italic = find_font(main_span(obj)["font"],
                                                         main_span(obj)["flags"])
                self.replace_text(i, obj, obj["text"],
                                  bold=(not bold) if key == "bold" else None,
                                  italic=(not italic) if key == "italic" else None)
            else:
                self.toggle_line_mark(i, obj, {"underline": "Underline", "strike": "StrikeOut",
                                               "highlight": "Highlight"}[key])
        else:
            self.text_style[key] = not self.text_style[key]
            self.restyle_editor()
        self.refresh_props()

    def toggle_line_mark(self, i, obj, kind, on=None, color=None):
        """Underline / strike out / highlight a whole line (as a mark over it) - or take it
        off. on: True / False to set it, None to switch it. Set on a line that has it
        already (another colour chosen), the old one's part over this line goes first - one
        change, so one Undo takes it back."""
        if obj.get("frame"):  # (a frame: on all of its text, or off)
            k = {"Underline": 5, "StrikeOut": 6, "Highlight": 7}[kind]
            runs = self.frame_runs(i, obj)
            turn = (not all(st[k] for t, st in runs if t.strip())) if on is None else on
            value = (color or self.text_highlight_color if kind == "Highlight" else True) if turn \
                else (None if kind == "Highlight" else False)
            self.replace_frame(i, obj, [(t, st[:k] + (value,) + st[k + 1:]) for t, st in runs])
            return
        rect = pymupdf.Rect(obj["rect"])
        old = self.marks_over(i, [rect], (kind,))
        turn_on = not old if on is None else on
        if not old and not turn_on:
            return
        name = {"Underline": "underline", "StrikeOut": "strike out", "Highlight": "highlight"}[kind]
        color = color or (self.text_highlight_color if kind == "Highlight"
                          else span_color(main_span(obj)))
        see = self.opacity("highlight") if kind == "Highlight" else 1

        def do():
            page = self.doc[i]  # (kept: an annotation can't outlive its page)
            cut_marks(page, old, [rect])  # (the line's part of it; any other lines keep theirs)
            if turn_on:
                add = {"Underline": page.add_underline_annot, "StrikeOut": page.add_strikeout_annot,
                       "Highlight": page.add_highlight_annot}[kind]
                annot = add([rect])
                annot.set_colors(stroke=hex_rgb(color))
                if see < 1:
                    annot.set_opacity(see)
                annot.update()
        what = (("change " + name + " colour") if old else name) if turn_on else "remove " + name
        self.change(what, do, pages={i})
        self.view.reselect(i, rect, "text")

    def highlight_menu(self, button):
        """The highlight button's little arrow: its colour, or no highlight."""
        menu = PopupMenu(self)
        for name, color in (("Yellow", "#FFFF00"), ("Green", "#00FF00"), ("Turquoise", "#00FFFF"),
                            ("Pink", "#FF00FF"), ("Orange", "#FFB000")):
            menu.add_command(label=name, command=lambda c=color: self.set_text_highlight(c),
                             checked=self.text_highlight_color == color)
        menu.add_command(label="Other colour...", command=lambda: self.set_text_highlight(
            color_dialog(self, "Color", self.text_highlight_color)))
        menu.add_separator()
        menu.add_command(label="No highlight", command=lambda: self.set_text_highlight(None))
        menu.tk_popup(button.winfo_rootx(), button.winfo_rooty() + button.winfo_height())

    def set_text_highlight(self, color):
        """A highlight colour chosen (None: no highlight): the text highlighted in it."""
        if color:
            self.text_highlight_color = color
        if self.editing_rich():
            self.restyle_letters("highlight", color or None)
            return
        if self.tool == "edit":
            line = self.picked_line()
            if line:
                self.toggle_line_mark(*line, "Highlight", on=bool(color), color=color)
        else:
            self.text_style["highlight"] = bool(color)
            self.restyle_editor()
        self.refresh_props()

    def case_menu(self, button):
        menu = PopupMenu(self)
        for label, fn in self.CASES:
            menu.add_command(label=label, command=lambda f=fn: self.change_case(f))
        menu.tk_popup(button.winfo_rootx(), button.winfo_rooty() + button.winfo_height())

    def change_case(self, fn):
        ed = self.editing_rich()
        if ed:  # the selected letters (or all), each keeping its look
            box = ed["box"]
            sel = box.tag_ranges("sel")
            a, b = (str(sel[0]), str(sel[1])) if sel else ("1.0", "end-1c")
            old = box.get(a, b)
            new = fn(old)
            if len(new) == len(old):
                for k, (c0, c1) in enumerate(zip(old, new)):
                    if c0 != c1:
                        idx = f"{a}+{k}c"
                        tags = [tg for tg in box.tag_names(idx) if tg != "sel"]
                        box.delete(idx)
                        box.insert(idx, c1, tuple(tags))
                if sel:
                    box.tag_add("sel", a, b)
            ed["grow"]()  # (capitals are wider)
            box.focus_set()
            return
        if self.tool == "edit":
            line = self.picked_line()
            if line and line[1].get("frame"):  # (a frame: the case changed across all of it)
                runs = self.frame_runs(*line)
                whole = fn("".join(t for t, _ in runs))
                if len(whole) == sum(len(t) for t, _ in runs):
                    new, k = [], 0
                    for t, st in runs:
                        new.append((whole[k:k + len(t)], st))
                        k += len(t)
                    self.replace_frame(*line, new)
                return
            if line and fn(line[1]["text"]) != line[1]["text"]:
                self.replace_text(*line, fn(line[1]["text"]))
            return
        ed = self.view.editor
        if ed and ed.get("kind") == "add":  # (the text being typed)
            box = ed["box"]
            text = box.get("1.0", "end-1c")
            box.delete("1.0", "end")
            box.insert("1.0", fn(text))
            self.restyle_editor()
        else:
            self.say("Type some text first (click on the page), then change its case.")

    def set_align(self, kind):
        """How the text lines up: the text being added - or the Edit tool's line (in its
        paragraph; while it's being typed, when it's put back)."""
        if self.tool == "edit":
            ed = self.editing_rich()
            if ed:
                ed["align"] = kind
                self.refresh_props()
                ed["box"].focus_set()
                return
            line = self.picked_line()
            if line:
                i, obj = line
                self.replace_runs(i, obj, self.line_runs(i, obj), align=kind)
            return
        self.text_style["align"] = kind
        self.restyle_editor()
        self.refresh_props()

    def set_direction(self, kind):
        """Left-to-right or right-to-left text (like Word's): right-to-left lines up on the
        right, left-to-right on the left."""
        align = "right" if kind == "rtl" else "left"
        if self.tool == "edit":
            ed = self.editing_rich()
            if ed:
                ed["dir"], ed["align"] = kind, align
                self.refresh_props()
                ed["box"].focus_set()
                return
            line = self.picked_line()
            if line:
                i, obj = line
                self.replace_runs(i, obj, self.line_runs(i, obj), align=align, direction=kind)
            return
        self.text_style["dir"], self.text_style["align"] = kind, align
        self.restyle_editor()
        self.refresh_props()

    def line_direction(self, obj):
        """A line's direction: right-to-left if its first letter is (Arabic...)."""
        first = next((c for c in obj["text"] if c.isalpha()), "")
        return "rtl" if RTL_LETTERS.match(first) else "ltr"

    def step_text_size(self, n):
        """Bigger / smaller: to the next size in the list."""
        if self.editing_rich():  # (each selected letter from its own size)
            sizes = [float(s) for s in TEXT_SIZES]

            def step(now):
                new = (next((s for s in sizes if s > now + 0.01), now + 4) if n > 0 else
                       next((s for s in reversed(sizes) if s < now - 0.01), max(1.0, now - 1)))
                return int(new) if new % 1 == 0 else new
            self.restyle_letters("size", step)
            return
        line = self.picked_line() if self.tool == "edit" else None
        if line and line[1].get("frame"):  # (a frame: each piece from its own size)
            sizes = [float(s) for s in TEXT_SIZES]

            def bigger(st):
                now = st[1]
                new = (next((s for s in sizes if s > now + 0.01), now + 4) if n > 0 else
                       next((s for s in reversed(sizes) if s < now - 0.01), max(1.0, now - 1)))
                return st[:1] + (int(new) if new % 1 == 0 else new,) + st[2:]
            self.restyle_frame(*line, bigger)
            return
        try:
            now = float(self.size_var.get())
        except ValueError:
            now = float(self.size("text"))
        sizes = [float(s) for s in TEXT_SIZES]
        if n > 0:
            new = next((s for s in sizes if s > now + 0.01), now + 4)
        else:
            new = next((s for s in reversed(sizes) if s < now - 0.01), max(1.0, now - 1))
        self.size_var.set(str(int(new) if new % 1 == 0 else new))
        self.size_typed()

    def restyle_editor(self):
        """The box text is being added in shows the text as it'll be: font, size, colour,
        bold, italic, underline, strikethrough, highlight and alignment."""
        ed = self.view.editor
        if not ed or ed.get("kind") != "add":
            return
        st = self.text_style
        family = find_family(self.text_font, st["bold"], st["italic"])[1]
        bold, italic = st["bold"], st["italic"]
        px = max(6, round(self.size("text") * self.view.scale))
        font = ((family, -px) + (("bold",) if bold else ()) + (("italic",) if italic else ())
                + (("underline",) if st["underline"] else ())
                + (("overstrike",) if st["strike"] else ()))
        ink = untranslated(self.color("text"), "text")
        box = ed["box"]
        box.config(font=font, fg=ink, insertbackground=ink,
                   bg=untranslated(self.text_highlight_color, "box") if st["highlight"]
                   else "#FFFFFE")
        box.tag_configure("align", justify="left" if st["align"] == "justify" else st["align"])
        box.tag_add("align", "1.0", "end")
        ed["size"], ed["color"] = self.size("text"), self.color("text")
        ed["grow"]()  # (its size follows the text)
        box.focus_set()

    def say_tool(self):
        self.say(TOOL_HINTS[self.tool] if self.doc else "Open a PDF to start.")

    def tool_menu(self, tool, button):
        """A tool's little arrow: its kinds (the one in use pressed in)."""
        menu = PopupMenu(self)
        for label, key in TOOL_VARIANTS[tool]:
            menu.add_command(label=label, command=lambda k=key: self.choose_variant(tool, k),
                             checked=self.variant[tool] == key and tool != "sign")
        menu.tk_popup(button.winfo_rootx(), button.winfo_rooty() + button.winfo_height())

    def choose_variant(self, tool, key):
        if tool == "sign" and key == "new":
            self.set_tool("sign", toggle=False)
            self.new_signature()
            return
        if tool == "image" and key == "clipboard":  # (a picture copied: put it in now)
            self.variant[tool] = key
            self.set_tool("image", toggle=False)
            self.say("Click where the copied picture goes.")
            return
        self.variant[tool] = key
        self.tint_tools()
        self.set_tool(tool, toggle=False)

    def show_annotations(self):
        """The Annotations button: the list of annotations at the left - or, when it's already
        showing, back to the pages (the button stays pressed in while the list shows)."""
        if self.show_thumbs and self.sidebar.view == "annotations":
            self.sidebar.show_view("pages")
            return
        if not self.show_thumbs:
            self.toggle_thumbs()
        self.sidebar.show_view("annotations")

    def fill_fonts(self):
        """The font list: every font family installed (looked up at start)."""
        if not self.font_cb.cget("values"):
            self.wait_fonts()
            self.font_cb.config(values=font_families())

    def font_chosen(self):
        family = self.font_var.get()
        if self.editing_rich():
            self.restyle_letters("family", family)
            return
        line = self.edit_line_selected()
        if line:  # the Edit tool: the line picked up, in that font
            i, obj = line
            self.replace_text(i, obj, obj["text"], family=family)
        else:
            self.text_font = family
            self.view.canvas.focus_set()
            self.restyle_editor()

    def tint_tools(self):
        """The text, highlight, pen, note and shape buttons show the colour each one draws
        in (and the shape button its kind)."""
        for tool in TINTED:
            self.buttons[tool].set_icon(tool_icon(tool, self.tool_colors[tool]))
        self.buttons["shape"].set_icon(shape_icon(self.variant["shape"], self.tool_colors["shape"]))
        self.view.drawing_cursors(self.tool_colors)  # (the pointers: the same pictures)

    def color(self, tool=None):
        return self.tool_colors[tool or self.tool]

    def size(self, tool=None):
        return self.tool_sizes.get(tool or self.tool, 1)

    def pick_color(self):
        if self.editing_rich():
            color = color_dialog(self, "Color", self.selected_looks()[0][4])
            if color:
                self.restyle_letters("color", color)
            return
        line = self.picked_line() if self.tool == "edit" else None
        if line:  # the Edit tool: the colour of the line picked up
            i, obj = line
            color = color_dialog(self, "Color", span_color(main_span(obj)))
            if color:
                self.replace_text(i, obj, obj["text"], color=color)
            return
        if self.tool not in self.tool_colors:
            return
        color = color_dialog(self, "Color", self.color())
        if color:
            self.tool_colors[self.tool] = color
            self.tint_tools()
            self.refresh_props()
            self.restyle_editor()

    def size_typed(self):
        if self.editing_rich():
            try:
                value = float(self.size_var.get())
            except ValueError:
                value = 0
            if 0 < value <= 200:
                self.restyle_letters("size", int(value) if value % 1 == 0 else value)
            else:
                self.refresh_props()
            return
        line = self.picked_line() if self.tool == "edit" else None
        if line:  # the Edit tool: the size of the line picked up
            i, obj = line
            try:
                value = float(self.size_var.get())
                if 0 < value <= 200 and abs(value - main_span(obj)["size"]) > 0.05:
                    self.replace_text(i, obj, obj["text"], size=value)
            except ValueError:
                self.edit_selected()
            self.view.canvas.focus_set()
            return
        try:
            value = float(self.size_var.get())
            if 0 < value <= 200:
                self.tool_sizes["text"] = round(value, 1) if value % 1 else int(value)
        except ValueError:
            pass
        self.size_var.set(str(self.tool_sizes["text"]))
        self.view.canvas.focus_set()
        self.restyle_editor()

    # ---- annotations: text, highlights, drawings, notes ----
    def add_text(self, i, point, text, size, color, replace=None):
        def do():
            page = self.doc[i]
            if replace is not None:
                old = page.load_annot(replace)
                if old is not None:
                    page.delete_annot(old)
            if not text:
                return None
            lines = text.split("\n")
            w = max(pymupdf.get_text_length(line, fontname="helv", fontsize=size)
                    for line in lines) + size * 0.8 + 4
            h = len(lines) * size * 1.25 + size * 0.5
            corner = point * page.rotation_matrix  # (the box is upright on the page as shown)
            rect = pymupdf.Rect(corner.x, corner.y, corner.x + w, corner.y + h) * page.derotation_matrix
            annot = page.add_freetext_annot(rect, text, fontsize=size, fontname="helv",
                                            text_color=hex_rgb(color), rotate=page.rotation)
            return annot.xref
        what = "change text" if replace is not None else "add text"
        xref = self.change(what, do, pages={i})
        self.view.selected = (i, xref) if xref and self.tool in ("move", "select") else None
        self.view.draw_overlays()

    def freetext_style(self, xref):
        """(size, colour) a text annotation was written in, read from its /DA."""
        size, color = self.size("text"), self.color("text")
        try:
            da = self.doc.xref_get_key(xref, "DA")[1]
            m = re.search(r"([\d.]+)\s+Tf", da)
            if m:
                size = float(m[1])
                size = int(size) if size % 1 == 0 else size
            m = re.search(r"([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+rg", da)
            if m:
                color = rgb_hex([float(v) for v in m.groups()])
            elif re.search(r"([\d.]+)\s+g\b", da):
                g = float(re.search(r"([\d.]+)\s+g\b", da)[1])
                color = rgb_hex([g, g, g])
        except Exception:
            pass
        return size, color

    def edit_annot(self, i, xref):
        """Double-click on an annotation: change its text (a text box), or its note."""
        page = self.doc[i]
        annot = page.load_annot(xref)
        if annot is None:
            return
        kind = annot.type[1]
        if kind == "FreeText":
            text = annot.info.get("content", "")
            size, color = self.freetext_style(xref)
            x0, y0, x1, y1 = self.view.to_canvas(i, annot.rect)
            px = max(6, round(size * self.view.scale))
            self.view.open_editor(i, x0 + 2, y0 + px / 2 + 2, text, size, color, replace=xref)
        elif kind == "Text":
            old = annot.info.get("content", "")
            new = text_dialog(self, "Sticky note", "The note says:", old)
            if new is not None and new != old:
                def do():
                    page = self.doc[i]  # (kept: an annotation can't outlive its page)
                    a = page.load_annot(xref)
                    a.set_info(content=new)
                    a.update()
                self.change("change note", do, pages={i})
        else:
            self.say("Drag it to move it, or press Delete to remove it.")

    def words_of(self, i):
        if i not in self._words:
            with PDF_LOCK:
                self._words[i] = self.doc[i].get_text("words")
        return self._words[i]

    def chars_of(self, i):
        """Page i's letters in reading order: (x0, y0, x1, y1, letter, line) - each as tall
        as its line (so a selection is even), line: (block, line) numbers."""
        if i not in self._chars:
            with PDF_LOCK:
                d = self.doc[i].get_text("rawdict")
            out = []
            for nb, block in enumerate(d["blocks"]):
                for nl, line in enumerate(block.get("lines", [])):
                    y0, y1 = line["bbox"][1], line["bbox"][3]
                    for span in line["spans"]:
                        for ch in span["chars"]:
                            out.append((ch["bbox"][0], y0, ch["bbox"][2], y1, ch["c"], (nb, nl)))
            self._chars[i] = out
        return self._chars[i]

    @staticmethod
    def caret(chars, p, near=None):
        """Where a point falls between the letters: before letter k (k), or after the last
        (len). The letter nearest the point counts - on the line the point is on, if any;
        the point on its right half means after it. None if no letter is within near."""
        best, dist = None, 1e18
        for k, ch in enumerate(chars):
            dx = max(ch[0] - p.x, 0, p.x - ch[2])
            dy = max(ch[1] - p.y, 0, p.y - ch[3])
            d = dx * dx + 9 * dy * dy  # (away from a line counts more than along it)
            if d < dist:
                best, dist = k, d
        if best is None or (near is not None and dist > near * near):
            return None
        ch = chars[best]
        return best + 1 if p.x > (ch[0] + ch[2]) / 2 else best

    def letters(self, i, lo, hi):
        """Letters lo to hi (not including hi) of page i: (one box per line, their text)."""
        chars = self.chars_of(i)
        boxes, lines, keys = [], [], []
        for ch in chars[lo:hi]:
            if keys and keys[-1] == ch[5]:
                lines[-1] += ch[4]
                if not ch[4].isspace():
                    boxes[-1] = boxes[-1] | pymupdf.Rect(ch[:4]) if boxes[-1] else pymupdf.Rect(ch[:4])
            else:
                keys.append(ch[5])
                lines.append(ch[4])
                boxes.append(None if ch[4].isspace() else pymupdf.Rect(ch[:4]))
        text = "\n".join(line.strip() for line in lines if line.strip())
        return [r for r in boxes if r is not None], text

    def pick_letters(self, i, a, b):
        """The letters from point a to point b on page i, like dragging in a text editor:
        (one box per line, their text) - ([], "") if a isn't near any text."""
        chars = self.chars_of(i)
        ka, kb = self.caret(chars, a, near=15), self.caret(chars, b)
        if ka is None or kb is None or ka == kb:
            return [], ""
        return self.letters(i, min(ka, kb), max(ka, kb))

    # ---- selected text: copied, marked, or edited (the floating toolbar over it) ----
    def copy_selection(self):
        if self.tool == "edit" and self.view.edit_sel and not self.view.editor:
            self.copy_object()
            return
        sel = self.view.text_sel
        if sel:
            self.clipboard_clear()
            self.clipboard_append(sel["text"])
            self.say("Copied the selected text.")

    # ---- copying and pasting text and pictures (the Edit tool) ----
    def copy_object(self, sel=None):
        """The text or picture picked up with the Edit tool copied, to paste (Ctrl + V) -
        text exactly as it looks (its own fonts and places), its words on Windows'
        clipboard too."""
        i, obj = sel or self.view.edit_sel
        if obj["kind"] == "text":
            others = [o for o in self.objects_on(i) if o["kind"] == "text" and o is not obj]
            with PDF_LOCK:
                snap = frame_snapshot(self.doc, i, obj, others)
            self._copied = {"kind": "text", "snap": snap, "rect": pymupdf.Rect(obj["rect"]),
                            "page": i, "doc": self.doc}
            self.clipboard_clear()
            self.clipboard_append(obj["text"])
            self.say("Copied the text - paste it with Ctrl + V (where the mouse is).")
        else:
            with PDF_LOCK:
                pix = pymupdf.Pixmap(self.doc, obj["xref"])
                smask = (self.doc.extract_image(obj["xref"]) or {}).get("smask")
                if smask:  # (its see-through parts kept)
                    pix = pymupdf.Pixmap(pix, pymupdf.Pixmap(self.doc, smask))
                if pix.n - pix.alpha > 3:  # (CMYK: as it looks on screen)
                    pix = pymupdf.Pixmap(pymupdf.csRGB, pix)
                png = pix.tobytes("png")
            self._copied = {"kind": "image", "xref": obj["xref"], "png": png,
                            "rect": pymupdf.Rect(obj["rect"]), "page": i, "doc": self.doc}
            self.say("Copied the picture - paste it with Ctrl + V (where the mouse is).")

    def paste_target(self):
        """Where Ctrl + V pastes: (page, the spot) under the mouse - or None (not over a page)."""
        v = self.view
        c = v.canvas
        px, py = c.winfo_pointerxy()
        if not (c.winfo_rootx() <= px < c.winfo_rootx() + c.winfo_width()
                and c.winfo_rooty() <= py < c.winfo_rooty() + c.winfo_height()):
            return None
        cx, cy = c.canvasx(px - c.winfo_rootx()), c.canvasy(py - c.winfo_rooty())
        i = v.page_at(cx, cy)
        return (i, v.to_pdf(i, cx, cy)) if i is not None else None

    def paste_object(self, at=None):
        """Ctrl + V with the Edit tool: what was copied put on the page - at at (page, spot),
        else where the mouse is, else just down and right of where it was copied from."""
        c = getattr(self, "_copied", None)
        if not c or not self.doc:
            return
        if self.view.editor:
            return
        at = at or self.paste_target()
        r = c["rect"]
        if at is not None:
            i, spot = at
            dx, dy = spot.x - r.x0, spot.y - r.y0
        else:
            i = self.current
            same = c["doc"] is self.doc and c["page"] == i
            dx = dy = 12.0 if same else 0.0
        with PDF_LOCK:
            pw, ph = self.doc[i].rect.width, self.doc[i].rect.height
        dx = min(max(dx, -r.x0), pw - r.x1) if r.width < pw else -r.x0  # (on the page)
        dy = min(max(dy, -r.y0), ph - r.y1) if r.height < ph else -r.y0
        put = r + (dx, dy, dx, dy)
        if c["kind"] == "text":
            self._next_oid += 1
            data, clip = c["snap"]

            def do():
                draw_snapshot(self.doc[i], data, clip, dx, dy)
            self.change("paste", do, pages={i}, obj={"oid": self._next_oid})
        else:
            def do():
                page = self.doc[i]
                if c["doc"] is self.doc:  # (the same picture, drawn once more)
                    page.insert_image(put, xref=c["xref"])
                else:
                    page.insert_image(put, stream=c["png"])
            self.change("paste", do, pages={i})
        if self.tool != "edit":
            self.set_tool("edit")
        self.view.reselect(i, put, c["kind"])
        self.say("Pasted.")

    MARKS = {"highlight": "highlight", "strike": "strike out", "underline": "underline",
             "squiggly": "squiggly underline"}

    def mark_text(self, kind):
        """The selected text highlighted, struck out, underlined or squiggly-underlined."""
        sel = self.view.text_sel
        if not sel:
            return
        i, boxes = sel["page"], sel["boxes"]
        rgb = hex_rgb({"highlight": self.color("highlight"), "underline": "#0000FF"}.get(kind, "#FF0000"))

        def do():
            page = self.doc[i]  # (kept: an annotation can't outlive its page)
            add = {"highlight": page.add_highlight_annot, "strike": page.add_strikeout_annot,
                   "underline": page.add_underline_annot,
                   "squiggly": page.add_squiggly_annot}[kind]
            annot = add(boxes)
            annot.set_colors(stroke=rgb)
            if kind == "highlight" and self.opacity("highlight") < 1:
                annot.set_opacity(self.opacity("highlight"))
            annot.update()
        self.change(self.MARKS[kind], do, pages={i})

    def mark_kinds_over(self, i, boxes):
        """The kinds of text mark ("Highlight", "Underline"...) over some of these boxes."""
        xrefs = set(self.marks_over(i, boxes))
        with PDF_LOCK:
            page = self.doc[i]
            return {annot.type[1] for annot in page.annots() if annot.xref in xrefs}

    def marks_over(self, i, boxes, kinds=MARKUPS):
        """The text marks (their xrefs) on page i that cover some of these boxes."""
        out = []
        with PDF_LOCK:
            page = self.doc[i]
            for annot in page.annots():
                if annot.type[1] in kinds and any(
                        same_line(r, b) and min(r.x1, b.x1) - max(r.x0, b.x0) > 1
                        for r in quad_rects(annot) for b in boxes):
                    out.append(annot.xref)
        return out

    def unmark_selection(self):
        """Remove: the marks taken off just the selected letters - what's either side stays
        marked (a mark over a whole line, taken off two words, is cut in two)."""
        sel = self.view.text_sel
        if sel:
            self.remove_marks(sel["page"], sel["boxes"])

    def remove_marks(self, i, boxes, kinds=MARKUPS, what="remove highlight"):
        """The marks (of these kinds) taken off just these boxes; what's beside them stays."""
        xrefs = self.marks_over(i, boxes, kinds)
        holes = [pymupdf.Rect(b) for b in boxes]

        def do():
            page = self.doc[i]  # (kept: an annotation can't outlive its page)
            cut_marks(page, xrefs, holes)
        self.change(what, do, pages={i})

    def mark_text_of(self, i, xref):
        """The text under a mark: the letters inside its pieces."""
        pieces = self.view.annot_pieces(i, xref)
        lines = {}  # (line -> its letters inside the pieces, in order)
        for ch in self.chars_of(i):
            middle = pymupdf.Point((ch[0] + ch[2]) / 2, (ch[1] + ch[3]) / 2)
            if any(r.contains(middle) for r in pieces):
                lines[ch[5]] = lines.get(ch[5], "") + ch[4]
        return "\n".join(s.strip() for s in lines.values() if s.strip())

    def copy_mark_text(self, i, xref):
        text = self.mark_text_of(i, xref)
        if text:
            self.clipboard_clear()
            self.clipboard_append(text)
            self.say("Copied the marked text.")

    def mark_color(self, i, xref):
        with PDF_LOCK:
            page = self.doc[i]
            annot = page.load_annot(xref)
            stroke = annot.colors.get("stroke") if annot is not None else None
        return rgb_hex(stroke) if stroke else "#FFFF00"

    def recolor_mark(self, i, xref):
        color = color_dialog(self, "Color", self.mark_color(i, xref))
        if not color:
            return

        def do():
            page = self.doc[i]
            annot = page.load_annot(xref)
            annot.set_colors(stroke=hex_rgb(color))
            annot.update()
        self.change("change colour", do, pages={i})
        self.view.show_mark_bar(i, xref)  # (still picked up, its toolbar back)

    def edit_selection(self):
        """Edit text: the line the selection starts in, retyped with the Edit tool."""
        sel = self.view.text_sel
        if not sel:
            return
        i, first = sel["page"], sel["boxes"][0]
        lines = [o for o in self.objects_on(i) if o["kind"] == "text" and o["rect"].intersects(first)]
        obj = max(lines, key=lambda o: abs(o["rect"] & first), default=None)
        self.set_tool("edit", toggle=False)
        if obj:
            self.view.select_object((i, obj))
            self.view.edit_line(i, obj)

    def add_ink(self, i, points):
        rgb, width = hex_rgb(self.color("pen")), self.size("pen")
        see = self.opacity("pen")

        def do():
            page = self.doc[i]  # (kept: an annotation can't outlive its page)
            annot = page.add_ink_annot([[(p.x, p.y) for p in points]])
            annot.set_colors(stroke=rgb)
            annot.set_border(width=width)
            if see < 1:
                annot.set_opacity(see)
            annot.update()
        self.change("draw", do, pages={i})

    def add_marker(self, i, points):
        """A free marker stroke: a thick line in the highlight colour that multiplies with
        the page (the text under it stays readable, like a real highlighter)."""
        rgb, width = hex_rgb(self.color("highlight")), self.size("highlight")
        see = self.opacity("marker")

        def do():
            page = self.doc[i]  # (kept: an annotation can't outlive its page)
            annot = page.add_ink_annot([[(p.x, p.y) for p in points]])
            annot.set_colors(stroke=rgb)
            annot.set_border(width=width)
            annot.set_blendmode(pymupdf.PDF_BM_Multiply)
            annot.set_opacity(see)
            annot.set_info(subject="Marker")
            annot.update()
        self.change("marker", do, pages={i})

    def move_annot(self, i, xref, d):
        def do():
            page = self.doc[i]
            annot = page.load_annot(xref)
            if annot.type[1] == "Ink":  # (a drawing's lines are moved: it's made again)
                lines = [[(x + d.x, y + d.y) for x, y in line] for line in annot.vertices]
                stroke, width = annot.colors.get("stroke"), annot.border.get("width", 1)
                opacity = annot.opacity
                page.delete_annot(annot)
                annot = page.add_ink_annot(lines)
                annot.set_colors(stroke=stroke)
                annot.set_border(width=width)
                if 0 <= opacity < 1:
                    annot.set_opacity(opacity)
            elif annot.type[1] in ("Line", "PolyLine", "Polygon"):  # (no box of their own to
                kind = annot.type[1]  # move: drawn again where they go, as they were)
                pts = [(x + d.x, y + d.y) for x, y in annot.vertices]
                stroke, fill = annot.colors.get("stroke"), annot.colors.get("fill")
                width, ends, opacity = (annot.border or {}).get("width", 1), annot.line_ends, annot.opacity
                info = annot.info
                page.delete_annot(annot)
                annot = (page.add_line_annot(*pts[:2]) if kind == "Line" else
                         page.add_polyline_annot(pts) if kind == "PolyLine" else
                         page.add_polygon_annot(pts))
                annot.set_colors(stroke=stroke, fill=fill)
                annot.set_border(width=width)
                if ends and any(ends):
                    annot.set_line_ends(*ends)
                if 0 <= opacity < 1:
                    annot.set_opacity(opacity)
                annot.set_info(content=info.get("content", ""), title=info.get("title", ""))
            else:
                r = annot.rect + (d.x, d.y, d.x, d.y)
                if annot.type[1] in ("Square", "Circle"):  # (its box is round its line: the
                    w = (annot.border or {}).get("width") or 0  # line's box, so it stays as big)
                    r = r + (w, w, -w, -w)
                annot.set_rect(r)
            annot.update()
            return annot.xref
        new = self.change("move", do, pages={i})
        if new:
            self.view.select((i, new))

    def delete_annot(self):
        sel = self.view.selected
        if not sel:
            return
        i, xref = sel
        self.view.select(None)  # (put down: its floating toolbar goes with it)

        def do():
            page = self.doc[i]
            page.delete_annot(page.load_annot(xref))
        self.change("delete", do, pages={i})
        self.view.draw_overlays()

    # ---- whole pages ----
    def rotate_pages(self, pages, degrees):
        if not pages:
            return

        def do():
            for p in pages:
                page = self.doc[p]
                page.set_rotation((page.rotation + degrees) % 360)
        self.change("rotate", do)
        self.say(f"Turned {len(pages)} page(s) {'left' if degrees < 0 else 'right'}.")

    def delete_pages(self, pages):
        if not pages:
            return
        if len(pages) >= self.doc.page_count:
            dialog("Master PDF", "A PDF needs at least one page, so not all of them can be "
                   "deleted.", sound="error")
            return
        what = f"page {pages[0] + 1}" if len(pages) == 1 else f"these {len(pages)} pages"
        if dialog("Master PDF", f"Delete {what}?", ("Yes", "No")) != "Yes":
            return
        self.sidebar.selected = set()
        self.change("delete pages", lambda: self.doc.delete_pages(pages))
        self.say(f"Deleted {len(pages)} page(s).")

    def move_pages(self, pages, to):
        """Put pages (dragged on the thumbnails) before page to."""
        n = self.doc.page_count
        rest = [p for p in range(n) if p not in pages]
        k = to - sum(1 for p in pages if p < to)
        order = rest[:k] + list(pages) + rest[k:]
        if order == list(range(n)):
            return
        self.change("move pages", lambda: self.doc.select(order))
        self.sidebar.selected = set(range(k, k + len(pages)))
        self.set_current(k)

    def insert_paths(self, paths, at):
        self.from_word(paths, lambda files, left: self.insert_ready(files, at, left))

    def insert_ready(self, files, at, problems):
        """Insert pages from files, once any Word documents are PDFs."""
        result = self.change("insert pages", lambda: self.insert_files(self.doc, files, at)) \
            if files else (0, [])
        self.clean_converted()
        if result:
            added, more = result
            problems = problems + more
            if problems:
                dialog("Some files were left out", "\n".join(problems[:10]), sound="error")
            if added:
                self.set_current(at)
                self.say(f"Added {added} page(s).")

    def insert_from_file(self, at=None):
        """The pages of PDFs (or pictures) put in before page at (None: after this page)."""
        paths = file_dialog("open_many", self, title="Insert pages from file",
                                            filetypes=FILE_TYPES, initialdir=self.last_folder())
        if paths:
            self.insert_paths(paths, self.current + 1 if at is None else at)

    def insert_blank(self, at=None):
        """A blank page put in before page at (None: after this page), the size of the page
        next to it."""
        at = self.current + 1 if at is None else at
        w, h = self.page_sizes[min(max(at - 1, 0), len(self.page_sizes) - 1)]
        self.change("insert a page", lambda: self.doc.new_page(pno=at, width=w, height=h))
        self.set_current(at)
        self.say(f"Added a blank page (page {at + 1}).")

    def extract_pages(self, pages):
        """The chosen pages saved as a new PDF (the document itself isn't changed)."""
        if not pages:
            return
        folder = os.path.dirname(self.path) if self.path else self.last_folder()
        path = file_dialog("save", self, title="Extract pages",
                                            filetypes=PDF_TYPES, defaultextension=".pdf",
                                            initialdir=folder,
                                            initialfile=f"{self.doc_base()} (pages).pdf")
        if not path:
            return
        try:
            with PDF_LOCK:
                new = pymupdf.open()
                try:
                    for p in pages:
                        new.insert_pdf(self.doc, from_page=p, to_page=p)
                    new.save(path, **SAVE_OPTS)
                finally:
                    new.close()
        except Exception as e:
            dialog("Master PDF", f"Couldn't save the pages:\n{e}", sound="error")
            return
        if dialog("Master PDF", f"Saved {len(pages)} page(s) as {os.path.basename(path)}.",
                  ("Show file", "OK"), sound="done") == "Show file":
            reveal_all([os.path.normpath(path)])

    # ---- the Edit tool: the PDF's own text lines and pictures ----
    def wait_fonts(self):
        """The installed fonts' list is made at start; wait for it if it isn't ready yet."""
        if self._font_scan.is_alive():
            self.say("Looking up the installed fonts...")
            self.update_idletasks()
            self._font_scan.join()

    def objects_on(self, i):
        """What's on page i that the Edit tool can pick up. Its text is grouped into pieces
        (frames) the first time it's read; each piece is numbered and its letters remembered,
        so later it's always the same piece - text moved over another, or another moved over
        it, never mixes with it (and taking one out never takes the other)."""
        if i not in self._objects:
            if i not in self._owners:  # (the first time: Master PDF's own pieces, by label)
                with PDF_LOCK:
                    self._owners[i] = tagged_owners(self.doc, i)
                if self._owners[i]:
                    self._next_oid = max(self._next_oid, max(self._owners[i].values()))
            else:  # (after a change: Master PDF's pieces as their labels say - exactly, even
                with PDF_LOCK:  # where one is over another)
                    self._owners[i].update(tagged_owners(self.doc, i))
            owners = self._owners[i]
            claims = self._claims.pop(i, [])

            def owner(ch):
                who = owners.get(char_key(ch))
                if who is None and claims:  # (just written: the piece it was written for)
                    x0, y0, x1, y1 = ch["bbox"]
                    at = pymupdf.Point((x0 + x1) / 2, (y0 + y1) / 2)
                    for tag, boxes in reversed(claims):
                        if any(b.contains(at) for b in boxes):
                            return tag
                return who
            with PDF_LOCK:
                page = self.doc[i]
                objs = page_objects(page, owner if owners or claims else None)
            for o in objs:
                if o["kind"] != "text":
                    continue
                if o.get("oid") is None:
                    self._next_oid += 1
                    o["oid"] = self._next_oid
                for line in o.get("lines") or [o]:
                    for s in line["spans"]:
                        for ch in s.get("chars", []):
                            owners[char_key(ch)] = o["oid"]
                box = self._boxes.get((i, o["oid"]))
                if box is not None and o.get("frame"):  # (its box, as made - and as big as
                    o["box"] = pymupdf.Rect(box) | o["rect"]  # its text, if that's more)
            self._objects[i] = objs
        return self._objects[i]

    def remembered_frames(self, i, objs, mid):
        """The frames Master PDF laid out on page i (while the PDF is open) put together again
        from the lines it wrote - so a frame that grew past a drawn line, say, is still one
        (and its next edit takes all of it). A remembered frame no longer all there is let go."""
        def match(r, s):  # (a line Master PDF wrote: where it was written, not just touching)
            both = pymupdf.Rect(r) & s
            return not both.is_empty and abs(both) >= 0.6 * max(abs(pymupdf.Rect(r)), abs(s))
        keep = []
        for spots in self._frames.get(i, []):
            texts = [o for o in objs if o["kind"] == "text"]
            mine = [o for o in texts if any(match(line["rect"], s) for s in spots
                                                for line in o.get("lines") or [o])]
            lines = sorted((line for o in mine for line in o.get("lines") or [o]),
                           key=lambda line: (line["rect"].y0, line["rect"].x0))
            lines = [line for line in lines if any(match(line["rect"], s) for s in spots)]
            if len(lines) < 2 or not all(any(match(line["rect"], s) for line in lines) for s in spots):
                continue  # (changed since - not Master PDF's frame any more)
            rest = [line for o in mine for line in o.get("lines") or [o] if line not in lines]
            edges = next((o["edges"] for o in mine if o.get("edges")), ())
            pr = next((o["page_rect"] for o in mine if o.get("page_rect")), None)
            objs = [o for o in objs if o not in mine] + frame_objects(lines, mid, None, edges, pr)
            for line in rest:  # (other lines that were in those objects: on their own)
                objs += frame_objects([line], mid, None, edges, pr)
            keep.append(spots)
        self._frames[i] = keep
        return objs

    def object_at(self, i, point):
        """The text line (or, if none, the picture) at a point on page i - the smallest one
        there - or None."""
        p = pymupdf.Point(point)
        hits = [o for o in self.objects_on(i)
                if ((o.get("box") or o["rect"]) + (-2, -2, 2, 2)).contains(p)]
        with PDF_LOCK:
            area = abs(self.doc[i].rect)
        over = [o for o in hits if o["kind"] == "image" and abs(o["rect"]) < 0.5 * area
                and self.picture_layout(i, o) != "behind"]  # (a picture over the text - not
        if over:  # a page's background, or one put behind it: picked, as Word's)
            return min(over, key=lambda o: abs(o["rect"]))
        pool = [o for o in hits if o["kind"] == "text" and (
            not any(h["kind"] == "image" for h in hits)  # (a picture behind: the text only
            or any((line["rect"] + (-2, -3, 2, 3)).contains(p)  # where its lines are)
                   for line in o.get("lines") or [o]))] or             [o for o in hits if o["kind"] == "image"] or hits
        return min(pool, key=lambda o: o["rect"].width * o["rect"].height, default=None)

    def edit_line_selected(self):
        """(page, line) when the Edit tool has a line of text picked up, else None."""
        sel = self.view.edit_sel if self.tool == "edit" else None
        return sel if sel and sel[1]["kind"] == "text" else None

    def edit_selected(self):
        """Something was picked up (or put down) with the Edit tool: the size box and the
        colour button show the line's own."""
        line = self.edit_line_selected()
        self.refresh_props()
        if line:
            span = main_span(line[1])
            size = round(span["size"], 1)
            self.say(f"{line[1]['text'][:60]}  -  {find_font(span['font'], span['flags'])[1]}, "
                     f"{int(size) if size % 1 == 0 else size} pt. Click it again to retype it; "
                     f"drag the round handles to resize it, or the line to move it.")
        elif self.tool == "edit":
            sel = self.view.edit_sel
            self.say("Drag the picture to move it, or a corner to resize it." if sel
                     else TOOL_HINTS["edit"])
        self.update_ui()

    def frame_runs(self, i, obj):
        """A frame's text as pieces in one look each: its lines' (line_runs), a space where a
        paragraph goes on to its next line, a new line ("\\n") between paragraphs."""
        rec = self.wrap_record(i, obj)
        if rec:  # (laid out round a picture: its text as it was given)
            return list(rec["runs"])
        runs = []
        for para in obj["paras"]:
            for k, line in enumerate(para):
                pieces = self.line_runs(i, line)
                if not pieces:
                    continue
                if runs:
                    runs.append(("\n" if k == 0 else " ", runs[-1][1]))
                runs += pieces
        out = []
        for text, st in runs:
            if out and out[-1][1] == st:
                out[-1] = (out[-1][0] + text, st)
            else:
                out.append((text, st))
        return out

    def resize_frame(self, i, obj, box):
        """A frame's handles dragged: its box made box - the text wrapped across it, from its
        top; the box kept that big (as big as the text, if that's more)."""
        old = obj.get("box") or obj["rect"]
        dy = box.y0 - old.y0  # (the top moved: the text with it)
        meta = self.wrap_state()  # (as things were: for Undo)
        self._boxes[(i, obj["oid"])] = pymupdf.Rect(box)
        self.replace_frame(i, obj, self.frame_runs(i, obj), x0=box.x0, x1=box.x1, dy=dy,
                           meta=meta)
        self._boxes[(i, obj["oid"])] = pymupdf.Rect(box)
        self._objects.pop(i, None)
        self.view.reselect(i, box, "text")

    def replace_frame(self, i, obj, runs, align=None, direction=None, x0=None, x1=None, dy=0.0,
                      meta=None):
        """A frame's text put back, word-wrapped across it - from x0 to x1 if given (its
        handles dragged: kept for next time), else as wide as it was; lined up as it was (or
        as chosen); its underline / strike-out / highlight marks put where the words are."""
        rects = [pymupdf.Rect(o["rect"]) for o in obj["lines"]]
        if not "".join(text for text, _ in runs).strip():
            def gone():
                page = self.doc[i]
                for r in rects:
                    remove_text(page, r)
            self.change("delete text", gone, pages={i}, obj=obj)
            self.view.select_object(None)
            return
        self.wait_fonts()
        how, ref = self.line_ref(i, obj, align)
        memo = self._para[(i, round(obj["spans"][0]["origin"][1]))]
        if x0 is not None:
            memo["frame_x"] = (x0, x1)
        if align:  # (chosen: across the frame itself)
            how = align
        box = self._boxes.get((i, obj.get("oid")))
        fx0, fx1 = ((x0, x1) if x0 is not None else (box.x0, box.x1) if box is not None else
                    memo.get("frame_x") or cell_span(obj, how) or (obj["rect"].x0, obj["rect"].x1))
        if "cell" not in obj and not any(RTL_LETTERS.search(o["text"]) for o in obj["lines"]):
            meta = meta or self.wrap_state()  # (as things were: for Undo)
            rec = self.wrap_record(i, obj) or self.new_record(i, obj)  # (the frame's text and
            rec["runs"], rec["edited"] = list(runs), True  # place kept: laid out round any
            if align:  # picture, what's under it pushed down as it grows - and back up)
                rec["how"] = align
            box = self._boxes.get((i, obj.get("oid")))
            if x0 is not None:
                rec["full"] = (x0, x1)
            elif box is not None:
                rec["full"] = (box.x0, box.x1)
            rec["home_y"] += dy
            if box is not None:
                rec["box_bottom"] = box.y1
            self.flow_change(i, what="change text", meta=meta)
            self.view.reselect(i, pymupdf.Rect(rec["full"][0], obj["rect"].y0 + dy, rec["full"][1],
                                               obj["rect"].y1 + dy), "text")
            return
        paras = runs_to_paras(runs)
        old = self.marks_over(i, rects, ("Underline", "StrikeOut", "Highlight"))
        see = self.opacity("highlight")
        written = []

        def do():
            page = self.doc[i]  # (kept: an annotation can't outlive its page)
            cut_marks(page, old, rects)
            for r in rects:
                remove_text(page, r)
            placed = layout_frame(page, obj, paras, fx0, fx1, how,
                                  wrap=len(obj["lines"]) > 1 or "frame_x" in memo
                                  or "cell" in obj or box is not None,
                                  start_y=obj["lines"][0]["spans"][0]["origin"][1] + dy)
            written[:] = written_rows(placed)
            put_marks(page, placed, see)
        frames = [s for s in self._frames.get(i, [])  # (this frame: remembered as written now)
                  if not any(r.intersects(q) for r in rects for q in s)]
        self.change("change text", do, pages={i}, obj=obj)
        if written:
            self._frames[i] = frames + [written]
            self._objects.pop(i, None)
        self.view.reselect(i, pymupdf.Rect(fx0, obj["rect"].y0, fx1, obj["rect"].y1), "text")

    def restyle_frame(self, i, obj, change):
        """Every piece of a frame given a new look (change(look) -> look), and put back."""
        self.replace_frame(i, obj, [(text, change(st)) for text, st in self.frame_runs(i, obj)])

    def line_runs(self, i, obj):
        """A line as pieces in one look each: [(text, look)] - each letter's font, size,
        bold, italic and colour, and the underline / strike-out / highlight over it."""
        if obj.get("frame"):
            return self.frame_runs(i, obj)
        marks = []  # (kind, pieces, colour) over this line
        with PDF_LOCK:
            page = self.doc[i]
            for annot in page.annots():
                if annot.type[1] in ("Underline", "StrikeOut", "Highlight"):
                    pieces = [r for r in quad_rects(annot) if same_line(r, obj["rect"])]
                    if pieces:
                        stroke = annot.colors.get("stroke")
                        marks.append((annot.type[1], pieces, rgb_hex(stroke) if stroke else "#FFFF00"))
        runs = []
        for span in obj.get("all") or obj["spans"]:
            _path, family, bold, italic = find_font(span["font"], span["flags"])
            size = round(span["size"], 1)
            size = int(size) if size % 1 == 0 else size
            look = (family, size, bold, italic, span_color(span))
            for ch in span.get("chars") or [{"c": c, "bbox": span["bbox"]} for c in span["text"]]:
                c = ch["c"].replace("\xa0", " ").replace("\ufffd", " ")
                if c == " " and runs and runs[-1][0].endswith(" "):
                    continue  # (a run of spaces is one: a widened gap reads back as two)
                x0, y0, x1, y1 = ch["bbox"]
                mid = pymupdf.Point((x0 + x1) / 2, (y0 + y1) / 2)
                on = {kind: color for kind, pieces, color in marks
                      if any(r.contains(mid) for r in pieces)}
                st = look + ("Underline" in on, "StrikeOut" in on, on.get("Highlight"))
                if runs and runs[-1][1] == st:
                    runs[-1][0] += c
                else:
                    runs.append([c, st])
        while runs and not runs[0][0].strip():
            runs.pop(0)
        while runs and not runs[-1][0].strip():
            runs.pop()
        if runs:
            runs[0][0] = runs[0][0].lstrip()
            runs[-1][0] = runs[-1][0].rstrip()
        return [(text, st) for text, st in runs]

    def replace_runs(self, i, obj, runs, align=None, direction=None):
        """The Edit tool's box put back: the line written piece by piece in each one's look,
        its underline / strike-out / highlight marks put where the pieces now are - lined up
        as it was (align: as chosen, in its paragraph). direction: "rtl" / "ltr" chosen."""
        if obj.get("frame"):  # (a frame: wrapped across it)
            self.replace_frame(i, obj, runs, align, direction)
            return
        if not runs or not "".join(text for text, _ in runs).strip():
            self.replace_text(i, obj, "")
            return
        if direction == "rtl" or any(RTL_LETTERS.search(text) for text, _ in runs):
            st = runs[0][1]  # (right-to-left: one piece)
            self.replace_text(i, obj, "".join(text for text, _ in runs), size=st[1],
                              color=st[4], family=st[0], bold=st[2], italic=st[3],
                              align=align, direction=direction)
            return
        how, ref = self.line_ref(i, obj, align)
        self.wait_fonts()
        rect = pymupdf.Rect(obj["rect"])
        old = self.marks_over(i, [rect], ("Underline", "StrikeOut", "Highlight"))
        see = self.opacity("highlight")

        def do():
            page = self.doc[i]  # (kept: an annotation can't outlive its page)
            cut_marks(page, old, [rect])  # (this line's part of them: put back as styled)
            placed = retype_runs(page, obj, runs, how, ref)  # (see change: labelled)
            for kind, field in (("Underline", 5), ("StrikeOut", 6), ("Highlight", 7)):
                groups = {}  # colour -> the pieces' boxes
                for box, st in placed:
                    if st[field]:
                        color = st[7] if kind == "Highlight" else st[4]
                        groups.setdefault(color, []).append(box)
                for color, boxes in groups.items():
                    add = {"Underline": page.add_underline_annot,
                           "StrikeOut": page.add_strikeout_annot,
                           "Highlight": page.add_highlight_annot}[kind]
                    annot = add(boxes)
                    annot.set_colors(stroke=hex_rgb(color))
                    if kind == "Highlight" and see < 1:
                        annot.set_opacity(see)
                    annot.update()
        self.change("change text", do, pages={i}, obj=obj)
        self.view.reselect(i, rect, "text")

    def line_ref(self, i, obj, align=None):
        """align_ref, remembering the paragraph a line was in: once retyped, a line is a
        paragraph of its own in the PDF, and the next alignment chosen still lines it up in
        the old one (for as long as the PDF is open)."""
        key = (i, round(obj["spans"][0]["origin"][1]))
        if key not in self._para:  # (first seen: its paragraph as it is now - kept)
            self._para[key] = {"range": align_ref(obj, "left")[1], "align": None,
                               "single": self.lone_or_last(obj)}
        memo = self._para[key]
        if align is None and not memo["align"]:  # (as it looks: keeping its own edges - an
            r = obj["rect"]  # indent stays)
            return line_align(obj), (r.x0, r.x1)
        align = align or memo["align"]  # (chosen - now or before: in its paragraph)
        memo["align"] = align
        if align == "justify" and memo["single"]:  # (like Word: a paragraph's last line, or
            return "left", memo["range"]  # a line on its own, isn't stretched)
        return align, memo["range"]

    @staticmethod
    def lone_or_last(obj):
        """Is a line on its own, or its paragraph's last?"""
        peers = obj.get("peers") or [obj["rect"]]
        return len(peers) == 1 or obj["rect"].y0 >= max(p.y0 for p in peers) - 0.5

    def line_alignment(self, i, obj):
        """How a line is lined up: as chosen here (while the PDF is open), else as it looks."""
        memo = self._para.get((i, round(obj["spans"][0]["origin"][1])))
        return (memo and memo["align"]) or line_align(obj)

    def editing_rich(self):
        """The Edit tool's box when a line is being typed in it (the toolbar then changes the
        letters selected in it - or all of them), else None."""
        ed = self.view.editor
        return ed if ed and ed.get("rich") and self.tool == "edit" else None

    def restyle_letters(self, field, value):
        """The selected letters (or all) given value for one part of their look; value can be
        a function of the old value."""
        k = STYLE_FIELDS.index(field)
        self.view.restyle_range(lambda st: st[:k] + ((value(st[k]) if callable(value) else value),)
                                + st[k + 1:])
        self.refresh_props()

    def selected_looks(self):
        """The looks of the letters selected in the Edit tool's box (or all of them)."""
        ed = self.editing_rich()
        box = ed["box"]
        sel = box.tag_ranges("sel")
        a, b = (str(sel[0]), str(sel[1])) if sel else ("1.0", "end-1c")
        n = max(1, len(box.get(a, b)))
        looks = [self.view.style_at(ed, f"{a}+{k}c") for k in range(n)]
        return [st for st in looks if st is not None] or [ed["default"]]

    def replace_text(self, i, obj, text, size=None, color=None, family=None, bold=None,
                     italic=None, align=None, direction=None):
        """A line retyped (or in another size, colour, font...), lined up as it was - or as
        chosen (align), in its paragraph."""
        if obj.get("frame"):  # (a frame: all of it in the size / colour / font... asked)
            if not text.strip():
                self.replace_frame(i, obj, [("", None)])
                return

            def look(st):
                f, s, b, it, c = st[:5]
                if family:  # (another font: bold / italic go with it only if chosen - not
                    b, it = chosen_style(f, b, it)  # the old font's own, like Lucida's slant)
                return ((family or f, size or s, b if bold is None else bold,
                         it if italic is None else italic, color or c) + st[5:])
            self.replace_frame(i, obj, [(t, look(st)) for t, st in self.frame_runs(i, obj)],
                               align, direction)
            return
        self.wait_fonts()
        how, ref = self.line_ref(i, obj, align)

        def do():
            page = self.doc[i]  # (kept: the page's objects can't outlive it)
            retype_line(page, obj, text, size, color, family, bold, italic, how, ref, direction)
        what = "delete text" if not text.strip() else "change text"
        self.change(what, do, pages={i}, obj=obj)
        self.view.reselect(i, obj["rect"], "text")

    def scale_text(self, i, obj, k, right=False):
        """The Edit tool's round handle dragged: the line k times as big."""
        self.wait_fonts()

        def do():
            page = self.doc[i]
            scale_line(page, obj, k, right)
        self.change("resize text", do, pages={i}, obj=obj)
        r = obj["rect"]
        ax = r.x1 if right else r.x0
        self.view.reselect(i, pymupdf.Rect(ax + (r.x0 - ax) * k, r.y1 - r.height * k,
                                           ax + (r.x1 - ax) * k, r.y1), "text")

    def move_object(self, i, obj, d):
        self.wait_fonts()
        moved = obj["rect"] + (d.x, d.y, d.x, d.y)

        def do():
            page = self.doc[i]
            if obj["kind"] == "text":
                for line in obj.get("lines") or [obj]:  # (a frame: every line of it)
                    move_line(page, line, d)
            else:
                place_image(page, obj, moved)
        if obj["kind"] == "image" and self.picture_layout(i, obj) != "front":
            self.flow_change(i, obj, moved, what="move picture")
            self.view.reselect(i, moved, "image")
            return
        self.change("move text" if obj["kind"] == "text" else "move picture", do, pages={i},
                    obj=obj if obj["kind"] == "text" else None)
        rec = self.wrap_record(i, obj) if obj["kind"] == "text" else None
        if rec is not None:  # (moved: it's where it is now)
            self._wrapped[i].remove(rec)
        if obj["kind"] == "text" and (i, obj.get("oid")) in self._boxes:  # (its box with it)
            self._boxes[(i, obj["oid"])] += (d.x, d.y, d.x, d.y)
            self._objects.pop(i, None)
        self.view.reselect(i, moved, obj["kind"])

    # ---- a picture's layout (Word's Layout Options): text round it, or behind / in front ----
    def picture_layout(self, i, obj):
        entry = self.wrap_entry(i, obj)
        return entry["mode"] if entry else "front"

    def wrap_entry(self, i, obj):
        """The layout kept for this picture on page i (found by what it is and where: the
        same picture drawn elsewhere is another), or None."""
        for entry in self._wraps.get(i, []):
            if entry["xref"] == obj.get("xref") and all(
                    abs(a - b) < 1.5 for a, b in zip(entry["rect"], obj["rect"])):
                return entry
        return None

    def wrap_state(self):
        """The pictures' layouts and the frames laid out round them - kept with each Undo."""
        return ({i: [dict(e) for e in es] for i, es in self._wraps.items()},
                {i: [dict(r, spots=list(r["spots"])) for r in rs] for i, rs in self._wrapped.items()},
                {k: pymupdf.Rect(b) for k, b in self._boxes.items()})

    def set_wrap_state(self, state):
        wraps, wrapped, *boxes = state
        if boxes:
            self._boxes = {k: pymupdf.Rect(b) for k, b in boxes[0].items()}
        self._wraps = {i: [dict(e) for e in es] for i, es in wraps.items()}
        self._wrapped = {i: [dict(r, spots=list(r["spots"])) for r in rs] for i, rs in wrapped.items()}

    def set_picture_layout(self, i, obj, mode):
        """The picture's layout chosen: the text on the page laid out round it (or not)."""
        rect = pymupdf.Rect(obj["rect"])
        if mode == "inline":  # (in line: at the left of the text it's in, the text over and under)
            best = None
            for o in self.objects_on(i):
                if o["kind"] == "text" and o.get("frame"):
                    rec = self.wrap_record(i, o)
                    full = rec["full"] if rec else (o["rect"].x0, o["rect"].x1)
                    over = (min(rect.x1, full[1]) - max(rect.x0, full[0])) * \
                        max(0, min(rect.y1 + 40, o["rect"].y1) - max(rect.y0 - 40, o["rect"].y0))
                    if over > 0 and (best is None or over > best[0]):
                        best = (over, full)
            if best:
                rect += (best[1][0] - rect.x0, 0, best[1][0] - rect.x0, 0)
        self.flow_change(i, obj, rect, what="picture layout", mode=mode)
        self.view.reselect(i, rect, "image")
        name = dict(LAYOUTS)[mode]
        self.say(f"Picture: {name}." + (" The text flows round it." if mode in FLOWING else ""))

    def wrap_obstacles(self, i, obj=None, rect=None, mode=None):
        """The pictures on page i text flows round: [(rect, layout, outline)] - obj at rect,
        if given (being moved), in layout mode, if given (being chosen)."""
        mine = self.wrap_entry(i, obj) if obj is not None else None
        out = []
        for o in self.objects_on(i):
            if o["kind"] != "image":
                continue
            entry = self.wrap_entry(i, o)
            this = obj is not None and o["xref"] == obj["xref"] and (
                entry is mine if mine is not None else
                all(abs(a - b) < 1.5 for a, b in zip(o["rect"], obj["rect"])))
            how = (mode or (entry and entry["mode"])) if this else (entry and entry["mode"])
            if how not in FLOWING:
                continue
            r = rect if this and rect is not None else o["rect"]
            outline = None
            if how in ("tight", "through"):
                if o["xref"] not in self._outlines:
                    with PDF_LOCK:
                        self._outlines[o["xref"]] = image_outline(self.doc, o["xref"])
                outline = self._outlines[o["xref"]]
            out.append((pymupdf.Rect(r), how, outline))
        return out

    def wrap_record(self, i, obj):
        """What's kept of a frame laid out round pictures (where it was, as wide as it was,
        its text and spacing) - found by where it's written now - or None."""
        oid = obj.get("oid")
        lines = obj.get("lines") or [obj]
        for rec in self._wrapped.get(i, []):  # (by which piece it is - two pieces one over the
            if rec.get("oid") is not None or oid is not None:  # other aren't mixed up)
                if rec.get("oid") == oid:
                    return rec
            elif any(line["rect"].intersects(s) for line in lines for s in rec["spots"]):
                return rec
        return None

    def new_record(self, i, obj):
        """A frame about to be laid out round pictures: where it is and how, kept - so it
        goes back as it was when the picture goes."""
        memo = self._para.get((i, round(obj["spans"][0]["origin"][1]))) or {}
        r = obj["rect"]
        rec = {"home_y": obj["lines"][0]["spans"][0]["origin"][1], "home_top": r.y0,
               "home_bottom": r.y1, "full": memo.get("frame_x") or (r.x0, r.x1),
               "how": self.line_alignment(i, obj), "runs": self.frame_runs(i, obj),
               "geom": frame_geometry(obj), "spots": [pymupdf.Rect(line["rect"]) for line in obj["lines"]]}
        rec.update(self.frame_original(i, obj))
        rec["oid"] = obj.get("oid")
        self._wrapped.setdefault(i, []).append(rec)
        return rec

    def frame_original(self, i, obj):
        """A frame as the page has it now - its lines, letter by letter, and the marks over it
        - to put back exactly when no picture is in its way any more."""
        rects = [pymupdf.Rect(line["rect"]) for line in obj["lines"]]
        marks = []
        with PDF_LOCK:
            page = self.doc[i]
            for xref in self.marks_over(i, rects, ("Underline", "StrikeOut", "Highlight")):
                annot = page.load_annot(xref)
                if annot is None:
                    continue
                boxes = [q for q in quad_rects(annot) if any(q.intersects(r) for r in rects)]
                if boxes:
                    marks.append((annot.type[1], annot.colors.get("stroke"), annot.opacity, boxes))
            snap = None
            if page.rotation == 0:  # (an exact copy of it, to draw back)
                try:
                    others = [o for o in (self._objects.get(i) or []) if o["kind"] == "text"
                              and o is not obj]
                    snap = frame_snapshot(self.doc, i, obj, others)
                except Exception:
                    snap = None
        return {"orig": [dict(line, spans=[dict(s) for s in line["spans"]]) for line in obj["lines"]],
                "orig_marks": marks, "orig_pdf": snap}

    def flow_change(self, i, obj=None, rect=None, what="picture layout", mode=None, meta=None):
        """One change (one Undo): the picture obj put at rect in its layout (behind the text or
        over it), then page i's text laid out round the pictures text flows round - a frame
        in the way wrapped beside or under them, the frames under it pushed down as it grows
        (and back up as it shrinks), a frame no longer in the way back as it was."""
        self.wait_fonts()
        obstacles = self.wrap_obstacles(i, obj, rect, mode)
        zones = [r + (-WRAP_SIDE, -WRAP_TOP, WRAP_SIDE, WRAP_TOP) for r, _m, _o in obstacles]
        kept = meta or self.wrap_state()  # (the records made now: kept only if it's made)
        plan = []
        for o in self.objects_on(i):
            if o["kind"] != "text" or not o.get("frame") or "cell" in o:
                continue
            rec = self.wrap_record(i, o)
            if rec is None and not any(z.intersects(o["rect"]) for z in zones):
                rec_like = None  # (not in the way: moved only if what's over it grows)
            else:
                rec_like = rec or self.new_record(i, o)
            info = rec_like or {"home_y": o["lines"][0]["spans"][0]["origin"][1],
                                "home_top": o["rect"].y0, "home_bottom": o["rect"].y1,
                                "full": (o["rect"].x0, o["rect"].x1), "how": None, "runs": None,
                                "geom": None}
            plan.append({"obj": o, "rec": rec_like, "info": info,
                         "rects": [pymupdf.Rect(line["rect"]) for line in o["lines"]]})
        for e in plan:  # (what each may need, read before the change)
            e["marks"] = self.marks_over(i, e["rects"], ("Underline", "StrikeOut", "Highlight"))
            if e["info"]["runs"] is None:
                e["lazy"] = (self.frame_runs(i, e["obj"]), self.line_alignment(i, e["obj"]),
                             frame_geometry(e["obj"]))
            info = e["info"]  # (a frame laid out before, now out of every picture's way:
            home = pymupdf.Rect(info["full"][0], info["home_top"], info["full"][1],  # back
                                info["home_bottom"])  # as it was, exactly)
            e["clear"] = e["rec"] is not None and "orig" in info and not info.get("edited") and not any(
                z.intersects(home) for z in zones)
        see = self.opacity("highlight")
        avoid = wrap_band(obstacles) if obstacles else None
        layout = (mode or self.picture_layout(i, obj)) if obj is not None else None
        done = []

        def do():
            page = self.doc[i]
            if obj is not None:
                place_image(page, obj, rect, behind=layout == "behind")
            moved = []
            done.append(None)  # (it ran: see after)
            for e in sorted(plan, key=lambda e: e["info"]["home_top"]):
                info = e["info"]
                a0, a1 = info["full"]
                push = max([m["grow"] for m in moved if min(a1, m["info"]["full"][1])
                            - max(a0, m["info"]["full"][0]) > 0
                            and m["info"]["home_top"] < info["home_top"]] + [0])
                if e["rec"] is None and push < 0.5:
                    continue
                if e["clear"] and push < 0.5:  # (back exactly as it was: its own letters,
                    cut_marks(page, e["marks"], e["rects"])  # fonts, places and marks)
                    with text_of(e["obj"].get("oid")) as boxes:
                        for r in e["rects"]:
                            remove_text(page, r)
                        if info.get("orig_pdf"):  # (drawn back exactly as it was)
                            restore_snapshot(page, *info["orig_pdf"])
                        else:
                            for line in info["orig"]:
                                for s in line["spans"]:
                                    write_text(page, pymupdf.Point(s["origin"]), s["text"], s)
                    if e["obj"].get("oid") is not None and boxes:
                        self._claims.setdefault(i, []).append((e["obj"]["oid"], list(boxes)))
                    for kind, stroke, opacity, rects in info["orig_marks"]:
                        add = {"Highlight": page.add_highlight_annot,
                               "StrikeOut": page.add_strikeout_annot,
                               "Underline": page.add_underline_annot,
                               "Squiggly": page.add_squiggly_annot}.get(kind)
                        if add:
                            annot = add(rects)
                            if stroke:
                                annot.set_colors(stroke=stroke)
                            if 0 <= opacity < 1:
                                annot.set_opacity(opacity)
                            annot.update()
                    e["grow"], e["restored"] = 0.0, True
                    moved.append(e)
                    done.append(e)
                    continue
                runs, how, geom = (info["runs"], info["how"], info["geom"]) if e["rec"] else e["lazy"]
                if not e["rec"]:  # (pushed for the first time: as it was, kept)
                    e["orig"] = self.frame_original(i, e["obj"])
                cut_marks(page, e["marks"], e["rects"])
                with text_of(e["obj"].get("oid")) as boxes:  # (each frame its own piece)
                    for r in e["rects"]:
                        remove_text(page, r)
                    placed = layout_frame(page, e["obj"], runs_to_paras(runs), a0, a1,
                                          how or "left", start_y=info["home_y"] + push,
                                          avoid=avoid, geometry=geom)
                if e["obj"].get("oid") is not None and boxes:
                    self._claims.setdefault(i, []).append((e["obj"]["oid"], list(boxes)))
                put_marks(page, placed, see)
                e["written"] = written_rows(placed)
                bottom = max((r.y1 for r in e["written"]), default=info["home_bottom"])
                if info.get("box_bottom") is not None:  # (its box, as made, too)
                    bottom = max(bottom, info["box_bottom"] + push)
                e["grow"] = bottom - info["home_bottom"]
                if not e["rec"]:  # (pushed down: kept, so it comes back up as it was)
                    e["new"] = dict({"home_y": info["home_y"], "home_top": info["home_top"],
                                     "home_bottom": info["home_bottom"], "full": info["full"],
                                     "how": how, "runs": runs, "geom": geom}, **e["orig"])
                moved.append(e)
                done.append(e)
        last = self.undo_stack[-1] if self.undo_stack else None
        self.change(what, do, pages={i}, meta=kept)
        if not self.undo_stack or self.undo_stack[-1] is last:  # (it failed: the records
            # made for it go)
            self.set_wrap_state(kept)
            return
        if obj is not None:  # (the picture's layout: kept where it is now)
            entry = self.wrap_entry(i, obj)
            if entry is None and mode is not None:
                entry = {"xref": obj["xref"], "rect": pymupdf.Rect(rect), "mode": mode}
                self._wraps.setdefault(i, []).append(entry)
            if entry is not None:
                entry["rect"] = pymupdf.Rect(rect)
                entry["mode"] = mode or entry["mode"]
        frames = self._frames.get(i, [])
        for e in done:  # (remembered as written now: read back as one frame each)
            if e is None:
                continue
            key = (i, e["obj"].get("oid"))
            if key in self._boxes:  # (its box goes where its text went)
                now = e.get("written") or [line["rect"] for line in e["info"].get("orig") or []]
                if now:
                    dy = min(r.y0 for r in now) - min(r.y0 for r in e["rects"])
                    self._boxes[key] = self._boxes[key] + (0, dy, 0, dy)
            frames = [s for s in frames if not any(r.intersects(q) for r in e["rects"] for q in s)]
            if e.get("restored"):  # (back as it was: nothing more to keep)
                if e["rec"] in self._wrapped.get(i, []):
                    self._wrapped[i].remove(e["rec"])
                continue
            if e.get("written"):
                frames.append(e["written"])
                rec = e["rec"] or dict(e["new"], oid=e["obj"].get("oid"))
                rec["spots"] = e["written"]
                if not e["rec"]:
                    self._wrapped.setdefault(i, []).append(rec)
        self._frames[i] = frames
        self._objects.pop(i, None)
        with PDF_LOCK:
            low = self.doc[i].rect.height
        if any(r.y1 > low for e in done if e for r in e.get("written") or []):
            self.say("Some text went past the bottom of the page - make the picture smaller or "
                     "move it.")

    def move_image_to_page(self, i, obj, j, rect):
        """A picture on page i moved to page j, at rect there (one change, for Undo) - the
        same picture, not a copy; its layout goes with it."""
        def do():
            delete_image(self.doc[i], obj)
            self.doc[j].insert_image(rect, xref=obj["xref"])
        self.change("move picture", do, pages={i, j})
        entry = self.wrap_entry(i, obj)
        if entry:
            self._wraps[i].remove(entry)
            self._wraps.setdefault(j, []).append(dict(entry, rect=pymupdf.Rect(rect)))
        self.view.reselect(j, rect, "image")
        self.say(f"Picture moved to page {j + 1}.")

    def resize_image(self, i, obj, rect):
        if rect.width < 2 or rect.height < 2:
            return

        def do():
            page = self.doc[i]
            place_image(page, obj, rect)
        if self.picture_layout(i, obj) != "front":
            self.flow_change(i, obj, rect, what="resize picture")
            self.view.reselect(i, rect, "image")
            return
        self.change("resize picture", do, pages={i})
        self.view.reselect(i, rect, "image")

    def delete_object(self):
        sel = self.view.edit_sel
        if not sel:
            return
        i, obj = sel
        self.view.select_object(None)

        def do():
            page = self.doc[i]
            if obj["kind"] == "text":
                for line in obj.get("lines") or [obj]:  # (a frame: every line of it)
                    remove_text(page, line["rect"])
            else:
                delete_image(page, obj)
        self.change("delete text" if obj["kind"] == "text" else "delete picture", do, pages={i},
                    obj=obj if obj["kind"] == "text" else None)

    def ask_picture(self, title):
        types = [("Pictures", " ".join("*" + e for e in IMAGE_EXTS)), ("All files", "*.*")]
        return file_dialog("open", self, title=title, filetypes=types,
                                          initialdir=self.last_folder())

    def add_picture(self, i=None, point=None, rect=None, clipboard=False):
        """Put a picture on a page - the one copied (clipboard), or one chosen from a file -
        in the box asked for (rect), or at a point, or in the middle of the page you're on, at
        a sensible size; then it can be moved and resized with the Edit tool."""
        if not self.doc:
            return
        try:
            if clipboard:
                from PIL import ImageGrab
                got = ImageGrab.grabclipboard()
                if isinstance(got, list):  # (files copied in Explorer: the first picture)
                    got = next((f for f in got if f.lower().endswith(IMAGE_EXTS)), None)
                if got is None:
                    dialog("Master PDF", "There's no picture copied. Copy one first (for "
                           "example with Print Screen, or Copy in another program).", sound="error")
                    return
                im = got if isinstance(got, Image.Image) else Image.open(got)
            else:
                path = self.ask_picture("Add picture")
                if not path:
                    return
                im = Image.open(path)
            im = ImageOps.exif_transpose(im)
            im.load()
        except Exception as e:
            dialog("Master PDF", f"Couldn't open that picture:\n{e}", sound="error")
            return
        self.put_picture(self.current if i is None else i, im, point, rect, "add picture")

    def put_picture(self, i, im, point=None, rect=None, what="add picture", width=None):
        """Put a Pillow picture on page i (see add_picture); width: how wide, in points."""
        w, h = im.size
        pw, ph = self.page_sizes[i]
        with PDF_LOCK:
            page = self.doc[i]
            if rect is not None:  # in the box: as big as fits, keeping its shape
                box = rect * page.rotation_matrix
                k = min(box.width / w, box.height / h)
                x0, y0 = box.x0 + (box.width - w * k) / 2, box.y0 + (box.height - h * k) / 2
            else:
                k = width / w if width else min(pw * 0.4 / w, ph * 0.4 / h, 72 / 96)
                at = (point * page.rotation_matrix) if point is not None else None
                x0 = at.x if at else (pw - w * k) / 2
                y0 = at.y if at else (ph - h * k) / 2
            place = pymupdf.Rect(x0, y0, x0 + w * k, y0 + h * k) * page.derotation_matrix
        buf = io.BytesIO()
        alpha = im.mode in ("RGBA", "LA", "P") or "transparency" in im.info
        if alpha:
            im.convert("RGBA").save(buf, "PNG")
        else:
            im.convert("RGB").save(buf, "JPEG", quality=92)

        def do():
            page = self.doc[i]
            page.insert_image(place, stream=buf.getvalue())
        self.change(what, do, pages={i})
        if self.tool != "edit":
            self.set_tool("edit")
        self.view.reselect(i, place, "image")

    def replace_picture(self, i, obj):
        path = self.ask_picture("Replace picture")
        if not path:
            return
        try:
            with Image.open(path) as im:
                w, h = ImageOps.exif_transpose(im).size
        except Exception as e:
            dialog("Master PDF", f"Couldn't open that picture:\n{e}", sound="error")
            return
        r = obj["rect"]  # the new picture in the old one's place, keeping its own shape
        k = min(r.width / w, r.height / h)
        fit = pymupdf.Rect(r.x0 + (r.width - w * k) / 2, r.y0 + (r.height - h * k) / 2,
                           r.x0 + (r.width + w * k) / 2, r.y0 + (r.height + h * k) / 2)

        def do():
            page = self.doc[i]
            if obj.get("uses", 1) > 1:  # the same picture elsewhere too (a signature in every
                # row...): just this one is taken off, and the new picture put there
                delete_image(page, obj)
                page.insert_image(fit, filename=path)
            else:
                page.replace_image(obj["xref"], filename=path)
                place_image(page, obj, fit)
        self.change("replace picture", do, pages={i})
        self.view.reselect(i, fit, "image")

    def save_picture(self, obj):
        with PDF_LOCK:
            info = self.doc.extract_image(obj["xref"])
        ext = {"jpx": "jp2"}.get(info.get("ext"), info.get("ext") or "png")
        path = file_dialog("save", self, title="Save picture as",
                                            defaultextension="." + ext, initialdir=self.last_folder(),
                                            initialfile=f"{self.doc_base()} picture.{ext}",
                                            filetypes=[(ext.upper(), "*." + ext), ("All files", "*.*")])
        if not path:
            return
        try:
            with open(path, "wb") as f:
                f.write(info["image"])
        except OSError as e:
            dialog("Master PDF", f"Couldn't save the picture:\n{e}", sound="error")
            return
        self.say(f"Saved {os.path.basename(path)}.")

    # ---- the Eraser, shapes, marks, the signature, Add text ----
    def erase_area(self, i, rect):
        """White-out: a box of the eraser's colour over the area."""
        if rect.width < 1 or rect.height < 1:
            return
        rgb = hex_rgb(self.color("eraser"))

        def do():
            page = self.doc[i]
            page.draw_rect(rect, color=None, fill=rgb, overlay=True)
        self.change("white-out", do, pages={i})

    def begin_rub(self, i):
        """The eraser pressed on page i: the document kept as it is (one Undo takes back the
        whole rub)."""
        with PDF_LOCK:
            self._rub = {"page": i, "before": self.doc.tobytes(), "changed": False}

    def rub(self, i, path, r):
        """The eraser moved along path (page points): drawings under it rubbed out now."""
        rub = getattr(self, "_rub", None)
        if not rub or rub["page"] != i:
            return
        with PDF_LOCK:
            page = self.doc[i]  # (kept: an annotation can't outlive its page)
            changed = rub_out(page, path, r)
        if changed:  # (drawn again when let go: the screen shows it rubbed out meanwhile)
            rub["changed"] = True

    def end_rub(self):
        """The eraser let go: what it rubbed out is one change, for Undo."""
        rub, self._rub = getattr(self, "_rub", None), None
        if not rub or not rub["changed"]:
            return False
        i = rub["page"]
        self.undo_stack.append((rub["before"], self.current, "erase", self.wrap_state()))
        del self.undo_stack[:-30]
        self.redo_stack.clear()
        self.dirty = True
        self.view.drop_bars()
        for cache in (self._objects, self._words, self._chars):
            cache.pop(i, None)
        self.sidebar.refresh_lists()
        self.view.invalidate({i})
        self.sidebar.invalidate({i})
        self.update_ui()
        return True

    def add_shape(self, i, a, b):
        kind = self.variant["shape"]
        rgb, width = hex_rgb(self.color("shape")), self.size("shape")
        see = self.opacity("shape")
        if kind in ("rect", "ellipse") and pymupdf.Rect(a, b).normalize().is_empty:
            return

        def do():
            page = self.doc[i]
            if kind in ("rect", "ellipse"):
                r = pymupdf.Rect(a, b).normalize()
                annot = page.add_rect_annot(r) if kind == "rect" else page.add_circle_annot(r)
                annot.set_colors(stroke=rgb)
            else:
                annot = page.add_line_annot(a, b)
                if kind == "arrow":
                    annot.set_line_ends(pymupdf.PDF_ANNOT_LE_NONE, pymupdf.PDF_ANNOT_LE_CLOSED_ARROW)
                    annot.set_colors(stroke=rgb, fill=rgb)
                else:
                    annot.set_colors(stroke=rgb)
            annot.set_border(width=width)
            if see < 1:
                annot.set_opacity(see)
            annot.update()
        self.change({"rect": "draw a rectangle", "ellipse": "draw an ellipse", "line": "draw a line",
                     "arrow": "draw an arrow"}[kind], do, pages={i})

    def new_signature(self):
        """Make (or remake) the signature; it's kept for next time."""
        im = signature_dialog(self)
        if im is None:
            if not os.path.exists(SIGNATURE_FILE) and self.tool == "sign":
                self.set_tool("select")
            return False
        try:
            os.makedirs(os.path.dirname(SIGNATURE_FILE), exist_ok=True)
            im.save(SIGNATURE_FILE)
        except OSError as e:
            dialog("Master PDF", f"Couldn't keep the signature:\n{e}", sound="error")
            return False
        self.say("Signature ready. Click where it goes.")
        return True

    def place_signature(self, i, point):
        if not os.path.exists(SIGNATURE_FILE) and not self.new_signature():
            return
        try:
            with Image.open(SIGNATURE_FILE) as im:
                im = im.convert("RGBA")
        except Exception as e:
            dialog("Master PDF", f"Couldn't read the signature:\n{e}", sound="error")
            return
        self.put_picture(i, im, point, what="sign", width=140)  # (then movable and resizable)

    def add_real_text(self, i, top, text):
        """Add text: written into the page itself, in the chosen font, size and colour (so
        the Edit tool can retype and move it later)."""
        if not text.strip():
            return
        self.wait_fonts()
        family, size, color = self.text_font, self.size("text"), self.color("text")
        st, mark = dict(self.text_style), self.text_highlight_color
        see = self.opacity("highlight")

        def do():
            page = self.doc[i]  # (kept: an annotation can't outlive its page)
            boxes = write_new_text(page, top, text, family, size, color, st["bold"],
                                   st["italic"], st["align"], st.get("dir"))
            for key, add, ink in (("highlight", page.add_highlight_annot, mark),
                                  ("underline", page.add_underline_annot, color),
                                  ("strike", page.add_strikeout_annot, color)):
                if st[key] and boxes:  # (marks over the lines: they can be taken off later)
                    annot = add(boxes)
                    annot.set_colors(stroke=hex_rgb(ink))
                    if key == "highlight" and see < 1:
                        annot.set_opacity(see)
                    annot.update()
        self._next_oid += 1  # (a new piece of text)
        self.change("add text", do, pages={i}, obj={"oid": self._next_oid})

    # ---- links ----
    def links_on(self, i):
        if i not in self._links:
            with PDF_LOCK:
                page = self.doc[i]
                self._links[i] = [link for link in page.get_links()
                                  if link.get("kind") in (pymupdf.LINK_URI, pymupdf.LINK_GOTO)]
        return self._links[i]

    @staticmethod
    def link_text(link):
        if link.get("kind") == pymupdf.LINK_URI:
            return f"Link to {link.get('uri', '')}"
        return f"Link to page {link.get('page', 0) + 1}"

    def follow_link(self, link):
        """A link clicked: a web page (asked first), or a page of this PDF."""
        if link.get("kind") == pymupdf.LINK_URI:
            uri = link.get("uri", "")
            if uri and dialog("Master PDF", f"Open this web page?\n{uri}", ("Open", "Cancel")) == "Open":
                import webbrowser
                webbrowser.open(uri)
        elif link.get("kind") == pymupdf.LINK_GOTO and 0 <= link.get("page", -1) < self.doc.page_count:
            self.set_current(link["page"])

    def link_spec(self, rect, choice):
        kind, value = choice
        if kind == "uri":
            return {"kind": pymupdf.LINK_URI, "from": rect, "uri": value}
        return {"kind": pymupdf.LINK_GOTO, "from": rect, "page": value, "to": pymupdf.Point(0, 0)}

    def new_link(self, i, rect):
        choice = link_dialog(self, self.doc.page_count)
        if choice:
            def do():
                page = self.doc[i]
                page.insert_link(self.link_spec(rect, choice))
            self.change("add a link", do, pages={i})
            self.view.draw_overlays()

    def edit_link(self, i, link):
        uri = link.get("uri", "") if link.get("kind") == pymupdf.LINK_URI else ""
        page_no = link.get("page") if link.get("kind") == pymupdf.LINK_GOTO else None
        choice = link_dialog(self, self.doc.page_count, uri, page_no)
        if choice:
            def do():
                page = self.doc[i]
                rect = pymupdf.Rect(link["from"]) * page.derotation_matrix  # (links come as shown)
                page.delete_link(link)
                page.insert_link(self.link_spec(rect, choice))
            self.change("change a link", do, pages={i})
            self.view.draw_overlays()

    def delete_link(self, i, link):
        def do():
            page = self.doc[i]
            page.delete_link(link)
        self.change("delete a link", do, pages={i})
        self.view.draw_overlays()

    # ---- Find ----
    def focus_find(self):
        self.find_entry.focus_set()
        self.find_entry.select_range(0, "end")

    def find_soon(self, jump=True):
        """The words in Find changed (or the PDF did): searched again a moment after the
        typing stops - every place shown at once, as Word's navigation pane does."""
        job = getattr(self, "_find_job", None)
        if job:
            self.after_cancel(job)
        self._find_job = self.after(250, lambda: self.search(self.find_var.get().strip(), jump))

    def search(self, text, jump=True):
        """Every place text is in the PDF: highlighted on the pages and listed in the side
        panel (which shows the results while there's something to find) - and, jump, the
        first one from this page on shown. Not jump: the list kept up to date (the PDF was
        changed) without moving the view."""
        self._find_job = None
        if not self.doc or not text:
            self.find_text, self.find_hits, self.find_pos = "", [], -1
            self.sidebar.fill_results([], "")
            back = getattr(self, "_view_before_find", None)
            if back and self.sidebar.view == "results":
                self.sidebar.show_view(back)
            self._view_before_find = None
            self.view.draw_overlays()
            return
        was = self.found_now()
        hits, rows = [], []
        with PDF_LOCK:
            for i in range(self.doc.page_count):
                found = page_find(self.doc[i], text)
                if not found:
                    continue
                for r, row in self.reading_order(found):
                    hits.append((i, pymupdf.Rect(r)))
                    rows.append((i,) + row)
                if len(hits) > 2000:  # (enough to go through)
                    break
        self.find_text, self.find_hits = text, hits
        if self.sidebar.view != "results":
            self._view_before_find = self.sidebar.view
            if self.show_thumbs:
                self.sidebar.show_view("results")
        self.sidebar.fill_results(rows, text)
        if not hits:
            self.find_pos = -1
            self.view.draw_overlays()
            self.say(f'Can\'t find "{text}".')
            return
        if jump:
            k = next((k for k, (i, r) in enumerate(hits) if i >= self.current), 0)
        else:  # (where you were: the same place, or the nearest)
            k = 0
            if was:
                k = min(range(len(hits)), key=lambda n: (abs(hits[n][0] - was[0]),
                        abs(hits[n][1].y0 - was[1].y0) + abs(hits[n][1].x0 - was[1].x0)))
        self.go_result(k, scroll=jump)

    def reading_order(self, found):
        """A page's results as you'd read them: line by line from the top - on a line, left
        to right, or right to left if it's Arabic (or other right-to-left text). (The PDF
        keeps them in the order it draws them: a table's heading can come before the page's
        title.) [(rect, (before, words, after))]"""
        items = [(pymupdf.Rect(r), row) for r, row in found]
        items.sort(key=lambda it: (it[0].y0 + it[0].y1) / 2)
        rows = []  # (results on one line: their middles within half a line of each other)
        for it in items:
            r = it[0]
            mid = (r.y0 + r.y1) / 2
            if rows and abs(mid - rows[-1][0]) < 0.5 * max(r.height, 1):
                rows[-1][1].append(it)
            else:
                rows.append([mid, [it]])
        out = []
        for _mid, row in rows:
            rtl = any(RTL_LETTERS.search("".join(it[1])) for it in row)
            out += sorted(row, key=lambda it: -it[0].x1 if rtl else it[0].x0)
        return out

    def find_next(self, step=1):
        """Enter in Find (Shift + Enter: back): the next place the words are."""
        text = self.find_var.get().strip()
        if not self.doc or not text:
            return
        if text != self.find_text:
            self.search(text)
            return
        if not self.find_hits:
            self.say(f'Can\'t find "{text}".')
            play_sound("error")
            return
        self.go_result((self.find_pos + step) % len(self.find_hits))

    def go_result(self, k, scroll=True):
        """Result k: shown on its page, marked in the list."""
        if not 0 <= k < len(self.find_hits):
            return
        self.find_pos = k
        i, rect = self.find_hits[k]
        if scroll:
            self.set_current(i, scroll=False)
            self.view.show_rect(i, rect)
        self.view.draw_overlays()
        self.sidebar.set_result(k, len(self.find_hits))
        self.say(f'"{self.find_text}": result {k + 1} of {len(self.find_hits)} '
                 f'(Enter: next, Shift + Enter: back, Esc: done).')

    def found_now(self):
        if self.find_text and 0 <= self.find_pos < len(self.find_hits):
            return self.find_hits[self.find_pos]
        return None

    # ---- closing ----
    def on_close(self):
        if not self.maybe_save():
            return
        self.destroy()

    def center_on_screen(self, w, h):
        """Open in the middle of the screen (nudged up a little for the title bar and taskbar)."""
        sw, sh = self.winfo_screenwidth(), self.winfo_screenheight()
        w, h = min(w, sw - 40), min(h, sh - 80)  # small screens: still fits
        x = max(0, (sw - w) // 2)
        y = max(0, (sh - h) // 2 - 20)
        self.geometry(f"{w}x{h}+{x}+{y}")

    def set_icon(self):
        try:
            if sys.platform == "win32":
                # default= also gives every dialog our icon, not Tk's feather
                self.iconbitmap(default=resource_path("icon.ico"))
            else:
                self._icon = ImageTk.PhotoImage(Image.open(resource_path("icon.png")))
                self.iconphoto(True, self._icon)
        except Exception:
            pass  # missing icon file: keep the default one


    # ---- updates ----
    def startup_update_tasks(self):
        """Just updated? Show what's new. Then look for a newer version in the background."""
        self.show_update_notes()
        self.check_for_updates()

    def show_update_notes(self):
        info = load_settings().get("just_updated")
        if not info:
            return
        save_settings(just_updated=None)  # show it once
        if info.get("version") != APP_VERSION:
            return  # the update didn't go through: nothing to announce
        notes_dialog(self, f"Master PDF was updated to version {APP_VERSION}.",
                     plain_notes(info.get("notes")))

    def check_for_updates(self):
        """Ask GitHub, off the main thread, whether there's a newer version. Skipped when
        running from the source code (no version) or if turned off in the settings file."""
        if parse_version(APP_VERSION) is None or not load_settings().get("check_updates", True):
            return
        self.ask_github(lambda latest: latest and self.offer_update(*latest))

    def ask_github(self, then):
        """Look for the newest release off the main thread, then call then(result)."""
        found = queue.Queue()
        threading.Thread(target=lambda: found.put(fetch_latest_release()), daemon=True).start()

        def wait():
            try:
                latest = found.get_nowait()
            except queue.Empty:
                self.after(200, wait)
                return
            then(latest)
        self.after(200, wait)

    def offer_update(self, version, notes, url):
        if parse_version(version) <= parse_version(APP_VERSION):
            return  # up to date
        if load_settings().get("skipped_version") == version:
            return  # skipped: not asked again (Help > Check for updates can still install it)
        choice = dialog("Update available",
                        f"Master PDF {version} is available.\nYou have version {APP_VERSION}.",
                        ("Update now", "Not now", "Skip this version"), sound="done")
        if choice == "Update now":
            self.install_update(version, notes, url)
        elif choice == "Skip this version":
            save_settings(skipped_version=version)  # no button, and not asked again
        else:  # Not now (or the box closed): the update button in the title bar, for later -
            self.show_update_button(version, notes, url)  # and asked again next time

    def show_update_button(self, version, notes, url):
        """The update button in the title bar (after "Not now"): install the update from it
        whenever you like. Its box's Not now keeps the button there."""
        def clicked():
            choice = dialog("Update available",
                            f"Master PDF {version} is available.\nYou have version {APP_VERSION}.",
                            ("Update now", "Not now"))
            if choice == "Update now":
                self.install_update(version, notes, url)
        try:
            self.chrome.set_update_button(clicked)
        except tk.TclError:
            pass  # the app was closed while the update check was finishing

    def check_updates_dialog(self):
        """Help > Check for updates: ask GitHub now and say what was found."""
        self.say("Checking for updates...")

        def found(latest):
            self.say_tool()
            if latest is None:
                dialog("Master PDF", "Couldn't reach GitHub - check the internet connection "
                       "and try again.", sound="error")
            elif parse_version(APP_VERSION) is None:
                dialog("Master PDF", f"The newest version is {latest[0]}.\nThis copy runs from "
                       "the source code, so it can't update itself.")
            elif parse_version(latest[0]) > parse_version(APP_VERSION):
                save_settings(skipped_version=None)
                self.offer_update(*latest)
            else:
                dialog("Master PDF", f"You have the newest version ({APP_VERSION}).", sound="done")
        self.ask_github(found)

    def install_update(self, version, notes, url):
        """Download the new installer (with a progress bar), run it silently - no questions:
        it installs over this version where it is - and close; the installer starts the new
        version, which then shows the release notes."""
        if not self.maybe_save():
            return
        win, chrome, body = new_dialog(self, "Updating")
        chrome.on_close = lambda: None
        label = tk.Label(body, text=f"Downloading Master PDF {version}...", bg=BG,
                         font=FONT, anchor="w")
        label.pack(fill="x", padx=12, pady=(12, 6))
        bar = ClassicProgress(body, maximum=100)
        bar.pack(fill="x", padx=12, pady=(0, 14))
        tk.Frame(body, bg=BG, width=320, height=0).pack()
        center_dialog(win, self)
        win.focus_force()
        win.grab_set()
        dst = os.path.join(tempfile.gettempdir(), f"MasterPDF-Setup-{version}.exe")
        events = queue.Queue()

        def download():  # off the main thread
            import urllib.request
            try:
                req = urllib.request.Request(url, headers={"User-Agent": "MasterPDF"})
                with urllib.request.urlopen(req, timeout=30) as r, open(dst + ".part", "wb") as f:
                    total, got = int(r.headers.get("Content-Length") or 0), 0
                    while chunk := r.read(256 * 1024):
                        f.write(chunk)
                        got += len(chunk)
                        if total:
                            events.put(("frac", (got, total)))
                os.replace(dst + ".part", dst)
                events.put(("done", None))
            except Exception as e:
                events.put(("error", str(e)))

        def poll():
            try:
                while True:
                    kind, value = events.get_nowait()
                    if kind == "frac":
                        got, total = value
                        bar.config(value=got / total * 100)
                        label.config(text=f"Downloading Master PDF {version}...   "
                                          f"{got / 1048576:.0f} of {total / 1048576:.0f} MB")
                    elif kind == "done":
                        label.config(text="Installing the update...")
                        bar.config(value=100)
                        win.update()
                        # remembered for the new version to show once it's running
                        save_settings(just_updated={"version": version, "notes": notes})
                        run_installer_after_exit(dst)
                        self.destroy()
                        return
                    else:
                        win.destroy()
                        dialog("Master PDF", f"The update couldn't be downloaded:\n{value}",
                               sound="error")
                        return
            except queue.Empty:
                pass
            self.after(100, poll)
        threading.Thread(target=download, daemon=True).start()
        self.after(100, poll)

    # ---- themes ----
    def pick_theme(self, choice):
        """Theme menu: a built-in theme (its own fixed look), or Custom - your own appearance
        and title bar, as you last left them in Settings."""
        settings = load_settings()
        if choice == CUSTOM_THEME and not settings.get("custom_theme"):
            # never set up: Custom starts as what's on screen now
            save_settings(custom_theme={"appearance": THEME, "palette": self.palette_name,
                                        "custom_color": self.custom_color})
            settings = load_settings()
        self.theme_choice = choice
        save_settings(theme=choice)
        self.apply_look(*theme_look(settings, choice))

    def apply_look(self, appearance, palette, custom_color):
        """Show an appearance (see THEME_COLORS) with a title bar palette. In the Custom
        theme, everything highlighted (selected pages, menu items under the mouse, dropdown
        lists, selected text) is in the title bar's colour."""
        global CAPTION_ACTIVE, CUSTOM_SELECT
        self.palette_name, self.custom_color = palette, custom_color
        CAPTION_ACTIVE = palette_colors(palette, custom_color)
        self.chrome._grad = None  # its gradient picture is made again in the new colours
        select = (selection_for(CAPTION_ACTIVE[0]) if self.theme_choice == CUSTOM_THEME
                  else None)
        changed, CUSTOM_SELECT = select != CUSTOM_SELECT, select
        self.set_theme(appearance, force=changed)  # (same appearance: the highlights still change)
        for chrome in list(ClassicWindow.alive):  # every title bar open - Settings' too, the
            try:  # window in front: the change shows at once, not when it's closed
                if chrome.win.winfo_exists():
                    chrome.draw()
            except tk.TclError:
                pass
        if self.settings_win is not None:  # Settings shows what's in use
            self.palette_var.set(palette)
            self.appearance_var.set(APPEARANCE_NAMES[appearance])
            self.draw_palette_strip()

    def save_custom(self, appearance, palette, custom_color):
        """A change made in Settings: it becomes (and is saved as) the Custom theme."""
        self.theme_choice = CUSTOM_THEME
        save_settings(theme=CUSTOM_THEME, custom_theme={
            "appearance": appearance, "palette": palette, "custom_color": custom_color})
        self.apply_look(appearance, palette, custom_color)

    def set_appearance(self, name):  # Settings > Appearance
        appearance = next(k for k, v in APPEARANCE_NAMES.items() if v == name)
        self.save_custom(appearance, self.palette_name, self.custom_color)

    def set_theme(self, theme, force=False):
        """Switch the whole app to another appearance (see THEME_COLORS). force: colour
        everything again even if it's the same one (the Custom theme's highlight changed)."""
        global THEME, DARK_MODE
        if theme == THEME and not force:
            return
        old, THEME, DARK_MODE = THEME, theme, theme == "dark"
        dark = DARK_MODE  # from here on, every colour given to Tk goes through theme_color()
        self.style_ttk()
        self.option_clear()  # defaults for widgets made from now on (menus, dialogs...)
        if dark:
            for pattern, value in (
                    ("*foreground", DARK_TEXT), ("*activeForeground", DARK_TEXT),
                    ("*disabledForeground", "#7a7a7a"), ("*selectColor", DARK_BOX),
                    ("*insertBackground", DARK_TEXT), ("*troughColor", "#262626"),
                    ("*highlightBackground", DARK_FACE), ("*background", DARK_FACE),
                    ("*Entry.background", DARK_BOX),  # text boxes: dark like the lists
                    ("*TCombobox*Listbox.background", DARK_BOX),
                    ("*TCombobox*Listbox.foreground", DARK_TEXT),
                    ("*TCombobox*Listbox.selectBackground", select_colors()[0]),
                    ("*TCombobox*Listbox.selectForeground", select_colors()[1])):
                self.option_add(pattern, value)
        names = {"disabledforeground": "*disabledForeground", "selectcolor": "*selectColor",
                 "selectbackground": "*selectBackground", "selectforeground": "*selectForeground",
                 "foreground": "*foreground", "activeforeground": "*activeForeground"}
        for opt, value in system_colors(theme).items():
            if opt in names and not (dark and opt not in ("selectbackground", "selectforeground")):
                self.option_add(names[opt], value)
        widgets = all_widgets(self)
        for w in widgets:  # everything that already exists
            retheme(w, old, theme)
            mono_boxes(w, theme == "mono")
        for w in widgets:  # the custom-drawn parts: redraw them in the new colours
            if isinstance(w, (FlatScrollbar, ToolButton)):
                w.draw()
            elif isinstance(w, ClassicProgress):
                w.draw(force=True)
            elif isinstance(w, RaisedEdge):
                w.redraw()
            elif isinstance(w, (EngravedLabel, TrackBar)):
                w.draw()
            elif w is getattr(self, "menu_line", None):  # the line under the menu bar: the
                # same as the group boxes' - in Black and White a plain 1 px black line (with
                # 1 px more space under it, so nothing below moves)
                mono = theme == "mono"
                w.configure(relief="flat" if mono else "groove", bd=0 if mono else 2,
                            height=1 if mono else 2)
                w.pack_configure(pady=(1, 1) if mono else (1, 0))
            elif isinstance(w, tk.LabelFrame):  # group boxes: Windows' grey etched line - in
                # Black and White a plain black one (1 px, and 1 px of face round it, so
                # nothing inside moves)
                mono = theme == "mono"
                w.configure(relief="solid" if mono else "groove", bd=1 if mono else 2,
                            highlightthickness=1 if mono else 0, highlightbackground=BG)
        self.chrome.border_colors()
        self.chrome.draw()
        self.color_dropdown_lists()
        if hasattr(self, "view"):  # the pages and thumbnails: drawn again in the new colours
            self.view.layout()
            self.sidebar.layout()
            self.sidebar.refresh_arrow()

    def color_dropdown_lists(self):
        """A dropdown makes its list the first time it opens and keeps its colours: colour
        every dropdown's list (making it now if needed) for the current theme."""
        for w in all_widgets(self):
            if isinstance(w, ttk.Combobox):
                popdown = self.tk.call("ttk::combobox::PopdownWindow", w)
                self.tk.call(f"{popdown}.f.l", "configure",
                             "-background", theme_color("#FFFFFF", "box"),
                             "-foreground", theme_color("#000000", "text"),
                             "-selectbackground", select_colors()[0],
                             "-selectforeground", select_colors()[1])

    def style_ttk(self):
        """The dropdowns are ttk widgets, coloured through styles. Every theme uses ttk's
        "alt" look - classic Windows shapes (raised arrow buttons) drawn by Tk, so unlike
        Windows' own native look they can be recoloured: the shapes stay exactly the same,
        only the colours change."""
        style = ttk.Style(self)
        style.theme_use("alt")
        # (styles don't go through the colour translator: translated here)
        face, box, text, trough, hot, grey = (
            theme_color(BG), theme_color("#FFFFFF", "box"), theme_color("#000000", "text"),
            theme_color("#F7F6F0"), theme_color("#F5F3E8"),
            "#7a7a7a" if DARK_MODE else theme_color("#999999"))
        sel, sel_text = select_colors()
        style.configure(".", background=face, foreground=text, fieldbackground=box,
                        troughcolor=trough, selectbackground=sel,
                        selectforeground=sel_text, arrowcolor=text, font=FONT)
        style.map(".", background=[("active", hot)])
        style.configure("Treeview", background=box, fieldbackground=box, foreground=text,
                        font=FONT, rowheight=18, borderwidth=0)
        style.map("Treeview", background=[("selected", sel)], foreground=[("selected", sel_text)])
        style.map("TCombobox",
                  fieldbackground=[("readonly", box), ("disabled", face)],
                  foreground=[("readonly", text), ("disabled", grey)],
                  selectbackground=[("readonly", box)],
                  selectforeground=[("readonly", text)],
                  background=[("readonly", face), ("active", hot)])

    # ---- What's This? ----
    MENU_NOTES = {
        "File": "Open, save and close PDFs; make a PDF from files; export the pages as "
                "pictures, split the PDF, or make a smaller copy of it.",
        "Edit": "Undo and redo, delete what's picked up, find words, and the settings.",
        "View": "Zoom, fit the pages to the window, and show or hide the page thumbnails.",
        "Pages": "Turn, add, take out and delete pages.",
        "Theme": "Changes the app's look: Windows 98, Ivory, Dark, or your own Custom theme.",
        "Help": "What's This?, updates, and About Master PDF.",
    }

    def whats_this_note(self, widget, x_root, y_root):
        """What's This? note for the part of the window that was clicked."""
        for name, btn in self.menubar.labels.items():
            if widget is btn:
                return self.MENU_NOTES[name]
        if isinstance(widget, ToolButton):
            return widget.note
        parts = {
            self.page_entry: "The page you're on. Type a page number and press Enter to go "
                             "there.",
            self.count_label: "How many pages the PDF has.",
            self.zoom_cb: "How big the pages are shown. Choose a size, or type one (like 130) "
                          "and press Enter. Fit page shows a whole page; Fit width fills the "
                          "window's width. Ctrl + mouse wheel zooms too.",
            self.size_cb: "The size of the text you add (in points) - or, with the Edit tool, of "
                          "the line picked up.",
            self.font_cb: "The font of the text you add - or, with the Edit tool, of the line "
                          "picked up.",
            self.find_entry: "Type words to find in the PDF and press Enter. Enter again "
                             "finds the next place.",
            self.view.canvas: "The PDF. Scroll with the mouse wheel or the scrollbars. What "
                              "the mouse does on the pages depends on the tool chosen in the "
                              "toolbar. Right-click a page or an annotation for more.",
            self.splitter: "Drag it to make the side panel wider or narrower.",
            self.sidebar.canvas: "The pages, small. Click one to go there; Ctrl or Shift + "
                                 "click to select several. Drag them to change their order, "
                                 "and right-click for the page commands.",
            self.msg_label: "What's happening, and what the chosen tool does.",
            self.page_label: "The page you're on, and how many pages there are.",
            self.size_label: "The size of the page you're on.",
            self.zoom_label: "How big the pages are shown.",
        }
        for part, note in parts.items():
            if widget is part:
                return note
        return None

    SETTINGS_NOTES = {  # What's This? in Settings: by a control's text, or its box's
        "App location": "The folder Master PDF is installed in. To move it, uninstall it and "
                        "install it again into another folder.",
        "Open folder": "Opens the folder Master PDF is installed in.",
        "Sound effects": "Plays a short chime when a job is done and a low tone when something "
                         "fails. Untick it for silence.",
        "App icon": "The picture shown at the left of the title bar and on the taskbar.",
        "Choose file...": "Use your own picture (.ico, .png, .jpg ...) as the app's icon.",
        "Reset to default": "Goes back to Master PDF's own icon.",
        "No icon in the title bar": "Hides the icon at the left of the title bar. The taskbar "
                                    "still shows it.",
        "Theme": "The app's colours. A change here is saved as the Custom theme, which you "
                 "can pick again any time from the Theme menu.",
        "Appearance": "The colours of the whole app: Light grey, Ivory, Dark grey... A change "
                      "is saved as the Custom theme.",
        "Color palette": "The colours of the title bar. Choose Custom... to pick any colour. A "
                         "change is saved as the Custom theme.",
        "strip": "Shows the title bar's colours.",
        "Window": "Whether Master PDF opens at its normal size or maximized (filling the "
                  "screen).",
        "Help": "Shows or hides the ? button in the title bar - the one you just used. It "
                "explains whatever you click next.",
        "Show the ? button in the title bar":
            "Shows or hides the ? button in the title bar - the one you just used. Untick it "
            "and it disappears from every window (turn it back on here).",
        "Updates": "Which version of Master PDF you have, and a button to look for a newer one.",
        "Check for updates": "Looks for a newer version now. If there is one, you can install "
                             "it straight away - the app restarts by itself.",
        "update_status": "What the last check for updates found.",
        "OK": "Closes Settings. (Changes are saved as soon as they're made.)",
    }

    def settings_note(self, widget, x_root, y_root):
        """What's This? note for the part of Settings that was clicked."""
        notes = self.SETTINGS_NOTES
        if widget is self.palette_strip:
            return notes["strip"]
        if widget is self.icon_preview:
            return notes["App icon"]
        if widget is self.version_label:
            return notes["Updates"]
        if widget is self.update_status:
            return notes["update_status"]
        if isinstance(widget, ttk.Combobox):
            var = str(widget.cget("textvariable"))
            return notes["Appearance" if var == str(self.appearance_var) else "Color palette"]
        try:
            text = str(widget.cget("text")).strip().rstrip(":")
        except tk.TclError:
            text = ""
        if text in notes:
            return notes[text]
        while widget is not None:  # anything else in a box: the box's note
            if isinstance(widget, tk.LabelFrame):
                return notes.get(str(widget.cget("text")).strip())
            widget = widget.master
        return None

    # ---- Settings (Edit > Settings), a dialog box ----
    def open_settings(self):
        if self.settings_win is not None:
            self.settings_win.lift()
            return
        win, chrome, body = new_dialog(self, "Settings")
        self.settings_win = win
        page = self.build_settings(body)
        page.pack(fill="both", expand=True)
        row = tk.Frame(body, bg=BG)
        row.pack(anchor="e", padx=12, pady=(0, 10))
        ok = xp_button(row, "OK", win.destroy)
        ok.config(width=10)
        ok.pack()
        WhatsThis(win, chrome, self.settings_note)
        win.bind("<Escape>", lambda e: win.destroy())
        win.bind("<Destroy>", lambda e: e.widget is win and setattr(self, "settings_win", None),
                 add="+")
        show_dialog(win, self, focus=ok)

    def build_settings(self, parent):
        page = tk.Frame(parent, bg=BG, padx=12, pady=10)

        # where the app is installed
        box = tk.LabelFrame(page, text=" App location ", bg=BG, font=FONT, padx=8, pady=8)
        box.pack(fill="x")
        tk.Label(box, text="Master PDF is installed in:", bg=BG, font=FONT,
                 anchor="w").pack(fill="x")
        row = tk.Frame(box, bg=BG)
        row.pack(fill="x", pady=(4, 0))
        folder = app_folder()
        xp_button(row, "Open folder", lambda: os.startfile(folder)).pack(side="right", padx=(6, 0))
        # shown like Windows 98's greyed-out text box - it can't be changed: sunken (shadow
        # and dark top / left, light and face bottom / right) on the face, its text engraved
        edge = tk.Frame(row, bg=EDGE_LIGHT)  # outer bottom / right
        edge.pack(side="left", fill="x", expand=True)
        inner = edge
        for bg, pad in ((EDGE_SHADOW, (0, 1)), (BG, (1, 0)), (EDGE_DARK, (0, 1)), (BG, (1, 0))):
            f = tk.Frame(inner, bg=bg)  # outer top / left, inner bottom / right, inner top /
            f.pack(fill="both", expand=True, padx=pad, pady=pad)  # left, then the inside
            inner = f
        EngravedLabel(inner, text=folder, anchor="w").pack(fill="x", pady=1)
        EngravedLabel(box, text="To move it, uninstall it and install it again into another "
                      "folder.", anchor="w").pack(fill="x", pady=(4, 0))

        # sound effects on / off
        box = tk.LabelFrame(page, text=" Sound effects ", bg=BG, font=FONT, padx=8, pady=6)
        box.pack(fill="x", pady=(6, 0))
        self.sounds_var = tk.BooleanVar(value=SOUNDS_ON)
        tk.Checkbutton(box, text="Play a sound when a job is done or something fails",
                       variable=self.sounds_var, command=self.toggle_sounds, bg=BG,
                       activebackground=BG, font=FONT).pack(anchor="w")

        # the app's icon
        box = tk.LabelFrame(page, text=" App icon ", bg=BG, font=FONT, padx=8, pady=8)
        box.pack(fill="x", pady=(6, 0))
        row = tk.Frame(box, bg=BG)
        row.pack(fill="x")
        self.icon_preview = tk.Label(row, bg="white", relief="sunken", bd=2)
        self.icon_preview.pack(side="left")
        col = tk.Frame(row, bg=BG)
        col.pack(side="left", padx=(10, 0))
        tk.Label(col, text="Shown in the title bar and on the taskbar.", bg=BG, font=FONT,
                 anchor="w").pack(anchor="w")
        btns = tk.Frame(col, bg=BG)
        btns.pack(anchor="w", pady=(6, 0))
        xp_button(btns, "Choose file...", self.choose_icon).pack(side="left")
        xp_button(btns, "Reset to default", self.reset_icon).pack(side="left", padx=(6, 0))
        self.no_icon_var = tk.BooleanVar(value=load_settings().get("no_icon", False))
        tk.Checkbutton(box, text="No icon in the title bar", variable=self.no_icon_var,
                       command=self.toggle_no_icon, bg=BG, activebackground=BG,
                       font=FONT).pack(anchor="w", pady=(6, 0))
        self.apply_icon()  # fills the preview

        # the theme: the app's colours (light grey, ivory, dark grey ...) and the title bar's,
        # side by side, with a strip showing the title bar (like Windows' Display settings);
        # a change here is saved as the Custom theme
        box = tk.LabelFrame(page, text=" Theme ", bg=BG, font=FONT, padx=8, pady=8)
        box.pack(fill="x", pady=(6, 0))
        row = tk.Frame(box, bg=BG)
        row.pack(fill="x")
        tk.Label(row, text="Appearance:", bg=BG, font=FONT).pack(side="left")
        self.appearance_var = tk.StringVar(value=APPEARANCE_NAMES[THEME])
        cb = ttk.Combobox(row, textvariable=self.appearance_var,
                          values=list(APPEARANCE_NAMES.values()), state="readonly", width=14)
        cb.pack(side="left", padx=(6, 0))
        cb.bind("<<ComboboxSelected>>", lambda e: self.set_appearance(self.appearance_var.get()))
        tk.Label(row, text="Color palette:", bg=BG, font=FONT).pack(side="left", padx=(16, 0))
        self.palette_var = tk.StringVar(value=self.palette_name)
        cb = ttk.Combobox(row, textvariable=self.palette_var,
                          values=list(TITLE_PALETTES) + [CUSTOM_PALETTE], state="readonly",
                          width=18)
        cb.pack(side="left", padx=(6, 0))
        cb.bind("<<ComboboxSelected>>", lambda e: self.set_palette(self.palette_var.get()))
        self.palette_strip = tk.Canvas(box, height=14, width=1, highlightthickness=0, bd=2,
                                       relief="sunken")
        self.palette_strip.pack(fill="x", pady=(8, 0))
        self.palette_strip.bind("<Configure>", lambda e: self.draw_palette_strip())
        EngravedLabel(box, text="Changes here are saved as the Custom theme.",
                      anchor="w").pack(fill="x", pady=(6, 0))
        self.color_dropdown_lists()

        # Window and Help side by side, as two equal boxes
        pair = tk.Frame(page, bg=BG)
        pair.pack(fill="x", pady=(6, 0))
        pair.columnconfigure((0, 1), weight=1, uniform="pair")
        pair.rowconfigure(0, weight=1)

        # how the window opens
        box = tk.LabelFrame(pair, text=" Window ", bg=BG, font=FONT, padx=8, pady=6)
        box.grid(row=0, column=0, sticky="nsew", padx=(0, 3))
        tk.Label(box, text="When the app starts:", bg=BG, font=FONT).pack(anchor="w")
        choices = tk.Frame(box, bg=BG)
        choices.pack(anchor="w", pady=(2, 0))
        self.start_var = tk.StringVar(
            value="max" if load_settings().get("start_maximized") else "normal")
        for text, val in (("Normal size", "normal"), ("Maximized", "max")):
            tk.Radiobutton(choices, text=text, variable=self.start_var, value=val, bg=BG,
                           activebackground=BG, font=FONT,
                           command=lambda: save_settings(
                               start_maximized=self.start_var.get() == "max")
                           ).pack(side="left", padx=(0, 10))

        # the ? (What's This?) buttons on or off
        box = tk.LabelFrame(pair, text=" Help ", bg=BG, font=FONT, padx=8, pady=6)
        box.grid(row=0, column=1, sticky="nsew", padx=(3, 0))
        self.help_var = tk.BooleanVar(value=SHOW_HELP)
        tk.Checkbutton(box, text="Show the ? button in the title bar",
                       variable=self.help_var, command=self.toggle_whats_this, bg=BG,
                       activebackground=BG, font=FONT).pack(anchor="w")
        EngravedLabel(box, text="(What's This? - it explains what you click)").pack(
            anchor="w", padx=(22, 0))

        # the version, and checking for a newer one
        box = tk.LabelFrame(page, text=" Updates ", bg=BG, font=FONT, padx=8, pady=8)
        box.pack(fill="x", pady=(6, 0))
        row = tk.Frame(box, bg=BG)
        row.pack(fill="x")
        running_source = parse_version(APP_VERSION) is None
        self.version_label = tk.Label(
            row, bg=BG, font=FONT, anchor="w",
            text="Version: " + ("dev (running from the source code)" if running_source
                                else APP_VERSION))
        self.version_label.pack(side="left")
        self.update_btn = xp_button(row, "Check for updates", self.check_updates_now)
        self.update_btn.pack(side="right")
        self.update_status = EngravedLabel(box, anchor="w")  # (errors: plain red)
        self.update_status.pack(fill="x", pady=(6, 0))
        # wraps only if the box is too narrow for its (one-line) messages
        self.update_status.bind("<Configure>", lambda e: self.update_status.config(
            wraplength=max(e.width - 4, 100)))
        return page

    def toggle_whats_this(self):  # Settings > Help
        global SHOW_HELP
        SHOW_HELP = self.help_var.get()
        save_settings(whats_this=SHOW_HELP)
        self.help_mode.stop()
        self.help_mode.close_note()
        self.chrome.on_help = self.help_mode.start if SHOW_HELP else None
        self.chrome.draw()

    def check_updates_now(self):
        """Settings > Updates > Check for updates: ask GitHub now (off the main thread) and
        say what was found; a newer version is offered straight away."""
        self.update_btn.config(state="disabled")
        self.update_status.config(text="Checking for updates...", fg="#666666")

        def found(latest):
            if self.settings_win is None:
                return  # (Settings was closed meanwhile)
            self.update_btn.config(state="normal")
            self.update_checked(latest)
        self.ask_github(found)

    def update_checked(self, latest):
        if latest is None:
            self.update_status.config(
                text="Couldn't reach GitHub - check the internet connection and try again.",
                fg="#C00000")
            return
        version, notes, url = latest
        mine = parse_version(APP_VERSION)
        if mine is None:
            self.update_status.config(
                text=f"Newest version: {version}. This copy runs from the source code, so it "
                     f"can't update itself.", fg="#666666")
        elif parse_version(version) > mine:
            self.update_status.config(text=f"Version {version} is available.", fg="#666666")
            choice = dialog("Update available",
                            f"Master PDF {version} is available.\n"
                            f"You have version {APP_VERSION}.",
                            ("Update now", "Not now"), parent=self.settings_win, sound="done")
            if choice == "Update now":
                self.install_update(version, notes, url)
            else:  # Not now: the update button in the title bar, for later
                self.show_update_button(version, notes, url)
        else:
            self.update_status.config(text=f"You have the newest version ({APP_VERSION}).",
                                      fg="#666666")

    def set_palette(self, name):
        """Settings > Color palette: recolour the title bar (and every message box's); saved
        as the Custom theme. Custom... asks for a colour (color_dialog) each time."""
        color = self.custom_color
        if name == CUSTOM_PALETTE:
            color = color_dialog(self.settings_win or self, "Edit Colors", CAPTION_ACTIVE[0],
                                 beside=True)
            if not color:  # cancelled: keep the palette there was
                self.palette_var.set(self.palette_name)
                return
            color = custom_palette(color)[0]
        self.save_custom(THEME, name, color)

    def draw_palette_strip(self):
        c = self.palette_strip
        w, h = max(c.winfo_width() - 4, 1), 14
        a, b = ([int(col[i:i + 2], 16) for i in (1, 3, 5)] for col in CAPTION_ACTIVE)
        ramp = Image.new("RGB", (256, 1))
        ramp.putdata([tuple(round(p + (q - p) * i / 255) for p, q in zip(a, b))
                      for i in range(256)])
        self._strip = ImageTk.PhotoImage(ramp.resize((w, h)))
        c.delete("all")
        c.create_image(2, 2, image=self._strip, anchor="nw")

    def toggle_no_icon(self):
        save_settings(no_icon=self.no_icon_var.get())
        self.apply_icon()

    def toggle_sounds(self):
        global SOUNDS_ON
        SOUNDS_ON = self.sounds_var.get()
        save_settings(sounds=SOUNDS_ON)
        play_sound("done")  # a sample when switched on (nothing when off)

    def apply_icon(self):
        """The app's icon - the custom one from Settings if there is one - on the window
        (taskbar, Alt+Tab), before the title, and in the Settings preview."""
        custom = os.path.exists(CUSTOM_ICON)
        path = CUSTOM_ICON if custom else resource_path("icon.png")
        try:
            im = Image.open(path).convert("RGBA")
        except Exception:
            return  # missing / broken icon file: keep what's there
        if custom:
            self._win_icons = [ImageTk.PhotoImage(im.resize((n, n), Image.LANCZOS))
                               for n in (48, 32, 16)]
            self.iconphoto(True, *self._win_icons)
        else:
            self.set_icon()
        if load_settings().get("no_icon"):  # Settings > No icon in the title bar
            self.chrome.icon = None
            self.chrome.draw()
        else:
            self.chrome.set_icon(path)
        if getattr(self, "settings_win", None) is not None and hasattr(self, "icon_preview"):
            self._preview_icon = ImageTk.PhotoImage(im.resize((32, 32), Image.LANCZOS))
            self.icon_preview.config(image=self._preview_icon)

    def choose_icon(self):
        path = file_dialog(
            "open", self.settings_win or self, title="Choose an icon",
            filetypes=[("Images", "*.ico *.png *.jpg *.jpeg *.bmp *.gif *.webp"),
                       ("All files", "*.*")])
        if not path:
            return
        try:
            im = Image.open(path)
            if im.format == "ICO":  # an icon holds several sizes: use the biggest
                im.size = max(im.info.get("sizes", [im.size]))
            im = im.convert("RGBA")
            side = max(im.size)  # not square: centre it on a transparent square
            square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
            square.paste(im, ((side - im.width) // 2, (side - im.height) // 2))
            os.makedirs(os.path.dirname(CUSTOM_ICON), exist_ok=True)
            square.resize((256, 256), Image.LANCZOS).save(CUSTOM_ICON)
        except Exception as e:
            dialog("Master PDF", f"Couldn't use that picture as the icon:\n{e}",
                   parent=self.settings_win, sound="error")
            return
        self.apply_icon()

    def reset_icon(self):
        try:
            os.remove(CUSTOM_ICON)
        except OSError:
            pass
        self.apply_icon()

    # ---- Help > About ----
    PROFILE_URL = "https://github.com/bocchhii"

    def about(self):
        """A Windows 98 About box: the logo beside the name, version and what the app is
        for, then (below an etched line) who made it, with a link to their GitHub."""
        win, chrome, body = new_dialog(self, "About Master PDF")
        top = tk.Frame(body, bg=BG)
        top.pack(padx=16, pady=(14, 0), anchor="w")
        try:
            self._about_logo = ImageTk.PhotoImage(
                Image.open(resource_path("icon.png")).convert("RGBA").resize((64, 64), Image.NEAREST))
            tk.Label(top, image=self._about_logo, bg=BG).pack(side="left", anchor="n")
        except Exception:
            pass
        col = tk.Frame(top, bg=BG)
        col.pack(side="left", padx=(14, 0), anchor="n")
        tk.Label(col, text="Master PDF", bg=BG, font=(FONT[0], 14, "bold"), anchor="w").pack(anchor="w")
        version = (f"Version {APP_VERSION}" if parse_version(APP_VERSION)
                   else "Version: dev (running from the source code)")
        tk.Label(col, text=version, bg=BG, font=FONT, anchor="w").pack(anchor="w", pady=(2, 8))
        tk.Label(col, text="Read, mark up and change your PDFs - without uploading them\n"
                           "anywhere. Everything happens on your own computer.",
                 bg=BG, font=FONT, justify="left").pack(anchor="w")
        etched_line(body, padx=16, pady=12)
        info = tk.Frame(body, bg=BG)
        info.pack(padx=16, anchor="w")
        tk.Label(info, text="Developer: bocchi the old", bg=BG, font=FONT).pack(anchor="w")
        tk.Label(info, text="For more tools, check out my profile:", bg=BG, font=FONT).pack(
            anchor="w", pady=(4, 0))
        link = tk.Label(info, text=self.PROFILE_URL, bg=BG, fg="#0000EE", cursor="hand2",
                        font=(FONT[0], FONT[1], "underline"))
        link.pack(anchor="w")
        link.bind("<ButtonRelease-1>", lambda e: __import__("webbrowser").open(self.PROFILE_URL))
        row = tk.Frame(body, bg=BG)
        row.pack(anchor="e", padx=12, pady=(10, 10))
        ok = xp_button(row, "OK", win.destroy)
        ok.config(width=10)
        ok.pack()
        tk.Frame(body, bg=BG, width=380, height=0).pack()
        win.bind("<Return>", lambda e: win.destroy())
        win.bind("<Escape>", lambda e: win.destroy())
        show_dialog(win, self, focus=ok)


if __name__ == "__main__":
    # Programs started from here (the update's installer, and through it the new version)
    # must start fresh: a PyInstaller app hands its children variables saying "your files
    # are unpacked in my temp folder" - a new Master PDF started by the installer would look
    # for them there after this app had closed and deleted them, and never start.
    os.environ["PYINSTALLER_RESET_ENVIRONMENT"] = "1"
    if sys.platform == "win32":
        try:  # lets Windows show our icon on the taskbar instead of Python's
            import ctypes
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("MasterPDF.App")
        except Exception:
            pass
    App().mainloop()
