# SPDX-License-Identifier: GPL-3.0-or-later
"""Headless phone and desktop checks for the project website, ``docs/index.html``.

The static checks need nothing but Python. They apply the Pages workflow's own reference rule (every local
``src``, ``srcset`` or ``href`` names a file the site publishes, and so does every absolute link to the site
itself, such as the share-card image), check that in-page anchors resolve, that the page loads nothing from
another host, that the two copies of the dark colour tokens agree, and that the page keeps the owner's content
decisions (``FORBIDDEN``).

The browser checks render the page in headless Chromium through Playwright, at three phone sizes and one desktop
size, in four theme states: Auto with a light system, Auto with a dark system, Light picked on a dark system and
Dark picked on a light system. For each they check that

- nothing overflows the viewport sideways;
- every section of ``main`` and the footer take a background of the active theme, and so does every large filled
  block inside them, apart from the deliberate exceptions in ``EXCEPTIONS``: the deep-blue top bar, which must
  stay deep blue in both themes, and the blue buttons;
- text meets WCAG AA contrast (4.5:1, or 3:1 for large text) against the colour behind it, the bar's text and the
  button text on the accent included;
- on phones, every control is at least 44 px tall, except links inside a sentence (the WCAG 2.5.8 inline
  exception);
- every image loads and the page requests nothing outside its own site.

Further tests drive the picker (it switches the whole page and is remembered), check that a remembered theme is in
place before the first paint, that blocked storage falls back to Auto without errors, that reduced motion
stops every animation and smooth scrolling, and that every keyboard focus stop draws a visible outline.

Without Playwright for Python or its Chromium, the browser tests SKIP; they never pass. The Pages workflow sets
``NK_SITE_TEST_REQUIRED=1``, which turns a missing tool into a failure.

Environment:

- ``NK_SITE_DIR``: the folder holding ``index.html`` and its images. Default ``docs/``, whose layout the
  workflow's ``_site/`` mirrors; the workflow points it at ``_site``.
- ``NK_SITE_TEST_CHROMIUM``: a Chromium or chrome-headless-shell executable to use instead of the one Playwright
  installs, for example an older ``%LOCALAPPDATA%\\ms-playwright\\chromium_headless_shell-*`` build.
- ``NK_SITE_SCREENSHOTS``: a folder to save a full-page screenshot of every state into. Unset, nothing is written.
- ``NK_SITE_TEST_REQUIRED``: ``1`` fails instead of skipping when the browser tooling is missing.
"""

from __future__ import annotations

import functools
import html.parser
import http.server
import os
import re
import threading
import unittest
from pathlib import Path
from urllib.parse import unquote, urlsplit

REPO_ROOT = Path(__file__).resolve().parent.parent
SITE_DIR = Path(os.environ.get("NK_SITE_DIR") or REPO_ROOT / "docs")
if not SITE_DIR.is_absolute():
    SITE_DIR = Path.cwd() / SITE_DIR
PAGE = SITE_DIR / "index.html"

# The Pages workflow's rule, unchanged: grep -oE '(src|srcset|href)="[^"#:]+"'. A value with ":" (an external
# link or a data: URI) or "#" (an in-page anchor) is not a local file and is skipped.
LOCAL_REF = re.compile(r'(src|srcset|href)="([^"#:]+)"')

PHONES = ((360, 780), (390, 844), (430, 932))
DESKTOP = (1280, 800)
VIEWPORTS = PHONES + (DESKTOP,)
# (name, the system's colour scheme, the stored choice or None for Auto, the theme the page must show)
STATES = (
    ("auto-light", "light", None, "light"),
    ("auto-dark", "dark", None, "dark"),
    ("forced-light", "dark", "light", "light"),
    ("forced-dark", "light", "dark", "dark"),
)
STORAGE_KEY = "nk-theme"
MIN_TARGET_PX = 44
# The site's own address. An absolute link under it (the share-card image, the canonical URL) names a published file.
SITE_URL = "https://jstar269.github.io/nakagawa-recomp/"
# The deep-blue top bar stays deep blue in both themes; its luminance must stay at or under DARK_MAX_LUMINANCE.
BAR = "body > header.bar"
# Filled blocks that deliberately keep a blue fill in both themes: the top bar, the blue buttons, the picked theme
# button and the skip link. Their text is still checked for contrast against that fill.
EXCEPTIONS = (BAR, ".btn", '.theme button[aria-pressed="true"]', ".skip")
# The owner's content decisions for this page (2026-10-27): no hst-decomp mention, no "PSN download" wording, no
# Hot Shots Tennis screenshot, and no review-draft text.
FORBIDDEN = ("hst-decomp", "PSN", "hst-title-screen", "Draft for review")
# A region's background must be clearly of its theme (WCAG relative luminance).
LIGHT_MIN_LUMINANCE = 0.6
DARK_MAX_LUMINANCE = 0.05
# Blocks inside a region (cards, panels) get a little more room.
BLOCK_LIGHT_MIN_LUMINANCE = 0.5
BLOCK_DARK_MAX_LUMINANCE = 0.1
BLOCK_MIN_AREA_PX = 12000

REQUIRED = os.environ.get("NK_SITE_TEST_REQUIRED") == "1"


def _page_text() -> str:
    return PAGE.read_text(encoding="utf-8")


class _Collector(html.parser.HTMLParser):
    """Collects ids, tag attributes and the text of <style> and <script> elements."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.ids: set[str] = set()
        self.tags: list[tuple[str, dict[str, str | None]]] = []
        self.styles: list[str] = []
        self.scripts: list[str] = []
        self._capture: str | None = None
        self.head_order: list[str] = []
        self._in_head = False

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attributes = dict(attrs)
        self.tags.append((tag, attributes))
        if attributes.get("id"):
            self.ids.add(str(attributes["id"]))
        if tag == "head":
            self._in_head = True
        elif tag == "body":
            self._in_head = False
        if self._in_head and tag in ("script", "style", "link"):
            self.head_order.append(tag)
        if tag in ("style", "script"):
            self._capture = tag
            (self.styles if tag == "style" else self.scripts).append("")

    def handle_endtag(self, tag: str) -> None:
        if tag == "head":
            self._in_head = False
        if tag == self._capture:
            self._capture = None

    def handle_data(self, data: str) -> None:
        if self._capture == "style":
            self.styles[-1] += data
        elif self._capture == "script":
            self.scripts[-1] += data


def _parsed() -> _Collector:
    collector = _Collector()
    collector.feed(_page_text())
    collector.close()
    return collector


def _declarations(block: str) -> dict[str, str]:
    return {
        name: " ".join(value.split()) for name, value in re.findall(r"(--[\w-]+|color-scheme)\s*:\s*([^;]+);", block)
    }


class SiteStaticTests(unittest.TestCase):
    """Checks that need no browser."""

    def test_every_local_reference_is_published(self) -> None:
        refs = {unquote(value) for _attr, value in LOCAL_REF.findall(_page_text())}
        for _tag, attrs in _parsed().tags:
            for key in ("src", "srcset", "href", "content"):
                value = str(attrs.get(key) or "")
                if value.startswith(SITE_URL):
                    refs.add(unquote(urlsplit(value).path.removeprefix(urlsplit(SITE_URL).path)) or "index.html")
        self.assertTrue(refs, "the page names no file of its own; the reference rule has nothing to check")
        missing = sorted(ref for ref in refs if not (SITE_DIR / ref).is_file())
        self.assertEqual(missing, [], f"referenced but not present under {SITE_DIR}")

    def test_owner_content_decisions_hold(self) -> None:
        text = _page_text()
        self.assertEqual([word for word in FORBIDDEN if word in text], [])

    def test_in_page_anchors_resolve(self) -> None:
        collector = _parsed()
        anchors = {
            str(attrs["href"])[1:]
            for tag, attrs in collector.tags
            if tag == "a" and str(attrs.get("href") or "").startswith("#")
        }
        self.assertTrue(anchors)
        self.assertEqual(sorted(anchors - collector.ids), [], "in-page links with no matching id")

    def test_page_loads_nothing_from_another_host(self) -> None:
        collector = _parsed()
        problems = []
        for tag, attrs in collector.tags:
            if tag == "script" and attrs.get("src"):
                problems.append(f"external script {attrs['src']}")
            if tag == "link":
                rel = str(attrs.get("rel") or "")
                href = str(attrs.get("href") or "")
                if rel == "icon" and href.startswith("data:"):
                    continue
                if rel == "canonical":
                    continue
                problems.append(f"<link rel={rel!r} href={href!r}>")
            if tag in ("img", "source", "iframe", "video", "audio", "embed", "object"):
                for key in ("src", "srcset", "data"):
                    value = str(attrs.get(key) or "")
                    if "://" in value or value.startswith("//"):
                        problems.append(f"<{tag} {key}={value!r}>")
        css = "\n".join(collector.styles)
        if "@import" in css or "@font-face" in css:
            problems.append("CSS @import or @font-face")
        problems += [f"CSS url({url})" for url in re.findall(r"url\(\s*['\"]?([^)'\"]+)", css) if "://" in url]
        self.assertEqual(problems, [])

    def test_theme_script_runs_before_any_style(self) -> None:
        collector = _parsed()
        self.assertTrue(collector.head_order, "no script or style in <head>")
        self.assertEqual(collector.head_order[0], "script", "the theme script must run before the first stylesheet")
        first = collector.scripts[0]
        self.assertIn(STORAGE_KEY, first)
        self.assertIn("data-theme", first)
        self.assertRegex(first, r"try\s*\{", "storage access must be wrapped in try/catch")

    def test_dark_tokens_match_in_both_places(self) -> None:
        css = "\n".join(_parsed().styles)
        light = re.search(r":root\s*\{(.*?)\}", css, re.S)
        auto_dark = re.search(
            r"@media\s*\(prefers-color-scheme:\s*dark\)\s*\{\s*:root:not\(\[data-theme=\"light\"\]\)\s*\{(.*?)\}",
            css,
            re.S,
        )
        forced_dark = re.search(r":root\[data-theme=\"dark\"\]\s*\{(.*?)\}", css, re.S)
        self.assertIsNotNone(light, "no :root token block")
        self.assertIsNotNone(auto_dark, "no dark token block under prefers-color-scheme: dark")
        self.assertIsNotNone(forced_dark, 'no :root[data-theme="dark"] token block')
        assert light and auto_dark and forced_dark
        auto_tokens = _declarations(auto_dark.group(1))
        self.assertEqual(auto_tokens, _declarations(forced_dark.group(1)), "the two dark token sets differ")
        self.assertEqual(auto_tokens.get("color-scheme"), "dark")
        missing = sorted(set(auto_tokens) - set(_declarations(light.group(1))))
        self.assertEqual(missing, [], "dark tokens with no light value")


# ---- Browser checks ----

CHECK_PAGE_JS = r"""
(opts) => {
  const out = { overflow: [], regions: [], blocks: [], contrast: [], unverifiable: [], targets: [] };
  const vw = document.documentElement.clientWidth;
  const desc = (el) => {
    let s = el.tagName.toLowerCase();
    if (el.id) s += '#' + el.id;
    if (el.classList.length) s += '.' + [...el.classList].join('.');
    const text = (el.textContent || '').trim().replace(/\s+/g, ' ').slice(0, 40);
    return text ? `${s} "${text}"` : s;
  };
  const visible = (el) => el.checkVisibility({ checkOpacity: true, checkVisibilityCSS: true });
  const parse = (c) => {
    const m = c.match(/rgba?\(([^)]+)\)/);
    if (!m) return null;
    const p = m[1].split(/[ ,\/]+/).filter(Boolean).map(Number);
    return { r: p[0], g: p[1], b: p[2], a: p.length > 3 ? p[3] : 1 };
  };
  const over = (top, bottom) => ({
    r: top.r * top.a + bottom.r * (1 - top.a),
    g: top.g * top.a + bottom.g * (1 - top.a),
    b: top.b * top.a + bottom.b * (1 - top.a),
    a: 1,
  });
  const lum = (c) => {
    const f = (v) => { v /= 255; return v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * f(c.r) + 0.7152 * f(c.g) + 0.0722 * f(c.b);
  };
  const ratio = (a, b) => { const x = lum(a), y = lum(b); return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05); };
  // The colour behind an element: its own background and its ancestors', composited from the first opaque one.
  const behind = (el) => {
    const layers = [];
    for (let n = el; n; n = n.parentElement) {
      const cs = getComputedStyle(n);
      if (cs.backgroundImage !== 'none') return { image: n };
      const c = parse(cs.backgroundColor);
      if (c && c.a > 0) { layers.push(c); if (c.a >= 1) break; }
    }
    let colour = { r: 255, g: 255, b: 255, a: 1 };
    for (let i = layers.length - 1; i >= 0; i--) colour = over(layers[i], colour);
    return { colour };
  };
  const ofTheme = (c, min, max) => opts.theme === 'light' ? lum(c) >= min : lum(c) <= max;
  const hex = (c) => '#' + [c.r, c.g, c.b].map((v) => Math.round(v).toString(16).padStart(2, '0')).join('');
  const excepted = (el) => opts.exceptions.some((sel) => el.closest(sel));

  // 1. Nothing wider than the viewport.
  const page = document.scrollingElement;
  if (page.scrollWidth > vw) out.overflow.push(`page scrolls sideways: ${page.scrollWidth}px content in ${vw}px`);
  const clipped = (el) => {
    for (let n = el.parentElement; n && n !== document.body; n = n.parentElement) {
      if (getComputedStyle(n).overflowX !== 'visible') return true;
    }
    return false;
  };
  for (const el of document.body.querySelectorAll('*')) {
    if (!visible(el)) continue;
    const r = el.getBoundingClientRect();
    if (r.width <= 1 && r.height <= 1) continue;
    if ((r.right > vw + 0.5 || r.left < -0.5) && !clipped(el)) {
      out.overflow.push(`${desc(el)} spans ${Math.round(r.left)}..${Math.round(r.right)} in ${vw}px`);
    }
  }

  // 2. Every top-level region, and the canvas, takes a background of the active theme; the top bar stays deep blue.
  const bar = document.querySelector(opts.bar);
  if (!bar) out.regions.push('missing top bar');
  else {
    const b = behind(bar);
    if (b.image || lum(b.colour) > opts.darkMax) out.regions.push(`${desc(bar)} is not deep blue in the ${opts.theme} theme`);
  }
  const regions = [document.documentElement, document.body, ...document.querySelectorAll('main > *'),
    document.querySelector('body > footer')];
  for (const el of regions) {
    if (!el) { out.regions.push('missing region'); continue; }
    const b = behind(el);
    if (b.image) { out.regions.push(`${desc(el)} has a background image`); continue; }
    if (!ofTheme(b.colour, opts.lightMin, opts.darkMax)) {
      out.regions.push(`${desc(el)} is ${hex(b.colour)} (luminance ${lum(b.colour).toFixed(3)}) in the ${opts.theme} theme`);
    }
  }
  // ... and so does every large filled block inside them.
  for (const el of document.body.querySelectorAll('*')) {
    if (!visible(el) || ['IMG', 'PICTURE', 'VIDEO', 'svg', 'CANVAS'].includes(el.tagName)) continue;
    const cs = getComputedStyle(el);
    const own = parse(cs.backgroundColor);
    const hasImage = cs.backgroundImage !== 'none';
    if (!hasImage && !(own && own.a > 0)) continue;
    const r = el.getBoundingClientRect();
    if (r.width * r.height < opts.blockArea || excepted(el)) continue;
    const b = behind(el);
    if (b.image) { out.blocks.push(`${desc(el)} has a background image`); continue; }
    if (!ofTheme(b.colour, opts.blockLightMin, opts.blockDarkMax)) {
      out.blocks.push(`${desc(el)} is ${hex(b.colour)} (luminance ${lum(b.colour).toFixed(3)}) in the ${opts.theme} theme`);
    }
  }

  // 3. Text contrast, WCAG AA.
  for (const el of document.body.querySelectorAll('*')) {
    const own = [...el.childNodes].some((n) => n.nodeType === 3 && n.textContent.trim());
    if (!own || !visible(el)) continue;
    const r = el.getBoundingClientRect();
    if (r.width <= 1 || r.height <= 1) continue;
    const cs = getComputedStyle(el);
    const b = behind(el);
    if (b.image) { out.unverifiable.push(`${desc(el)} sits on a background image`); continue; }
    let fg = parse(cs.color);
    if (fg.a < 1) fg = over(fg, b.colour);
    const size = parseFloat(cs.fontSize), weight = parseInt(cs.fontWeight, 10);
    const large = size >= 24 || (size >= 18.66 && weight >= 700);
    const need = large ? 3 : 4.5;
    const got = ratio(fg, b.colour);
    if (got < need) out.contrast.push(`${desc(el)}: ${hex(fg)} on ${hex(b.colour)} is ${got.toFixed(2)}:1, needs ${need}:1`);
  }

  // 4. Tap targets on phones.
  if (opts.phone) {
    const blockOf = (el) => { let n = el.parentElement; while (n && getComputedStyle(n).display === 'inline') n = n.parentElement; return n; };
    const selector = 'a[href], button, summary, label[for], input:not([type="hidden"]), select, textarea, [tabindex]';
    for (const el of document.querySelectorAll(selector)) {
      if (!visible(el)) continue;
      if (el.tagName === 'A' && getComputedStyle(el).display === 'inline') {
        const block = blockOf(el);
        if (block && block.textContent.trim().length > el.textContent.trim().length) continue;  // a link in a sentence
      }
      const r = el.getBoundingClientRect();
      if (r.height < opts.minTarget) out.targets.push(`${desc(el)} is ${r.height.toFixed(1)}px tall`);
    }
  }
  return out;
}
"""

LOAD_IMAGES_JS = r"""
async () => {
  for (const img of document.images) img.loading = 'eager';
  await Promise.all([...document.images].map((img) => img.complete ? null : new Promise((done) => {
    img.addEventListener('load', done, { once: true });
    img.addEventListener('error', done, { once: true });
  })));
  return [...document.images].filter((img) => !img.naturalWidth).map((img) => img.currentSrc || img.src);
}
"""

REGION_THEMES_JS = r"""
() => {
  const lum = (c) => {
    const p = c.match(/rgba?\(([^)]+)\)/)[1].split(/[ ,\/]+/).filter(Boolean).map(Number);
    const f = (v) => { v /= 255; return v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * f(p[0]) + 0.7152 * f(p[1]) + 0.0722 * f(p[2]);
  };
  const regions = [document.body, ...document.querySelectorAll('main > *'), document.querySelector('body > footer')];
  return regions.map((el) => {
    let n = el;
    while (n && getComputedStyle(n).backgroundColor === 'rgba(0, 0, 0, 0)') n = n.parentElement;
    return lum(getComputedStyle(n || document.documentElement).backgroundColor);
  });
}
"""


class _QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format: str, *args: object) -> None:  # noqa: A002 - the base class's name
        pass


class SiteBrowserTests(unittest.TestCase):
    """Renders the page headless and checks it at phone and desktop widths in every theme."""

    @classmethod
    def setUpClass(cls) -> None:
        try:
            from playwright.sync_api import sync_playwright
        except ImportError as exc:
            cls._missing(f"Playwright for Python is not installed ({exc})")
        cls._playwright = sync_playwright().start()
        executable = os.environ.get("NK_SITE_TEST_CHROMIUM") or None
        try:
            cls.browser = cls._playwright.chromium.launch(executable_path=executable)
        except Exception as exc:  # Playwright raises its own Error type for a missing browser.
            cls._playwright.stop()
            if "Executable doesn't exist" in str(exc) or "executable doesn't exist" in str(exc).lower():
                cls._missing(
                    "Playwright's Chromium is not installed: run `python -m playwright install chromium`, "
                    "or set NK_SITE_TEST_CHROMIUM to a Chromium executable"
                )
            raise
        handler = functools.partial(_QuietHandler, directory=str(SITE_DIR))
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.origin = f"http://127.0.0.1:{cls.server.server_address[1]}"
        cls.url = cls.origin + "/index.html"
        shots = os.environ.get("NK_SITE_SCREENSHOTS")
        cls.shots = Path(shots) if shots else None
        if cls.shots:
            cls.shots.mkdir(parents=True, exist_ok=True)

    @classmethod
    def _missing(cls, reason: str) -> None:
        if REQUIRED:
            raise RuntimeError(f"NK_SITE_TEST_REQUIRED=1 but {reason}")
        raise unittest.SkipTest(reason)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.shutdown()
        cls.server.server_close()
        cls.browser.close()
        cls._playwright.stop()

    # -- helpers --

    def _open(
        self,
        viewport: tuple[int, int],
        scheme: str,
        stored: str | None = None,
        *,
        reduced_motion: str = "no-preference",
        init: str = "",
    ):
        phone = viewport in PHONES
        context = self.browser.new_context(
            viewport={"width": viewport[0], "height": viewport[1]},
            color_scheme=scheme,
            reduced_motion=reduced_motion,
            is_mobile=phone,
            has_touch=phone,
            device_scale_factor=1,
        )
        self.addCleanup(context.close)
        foreign: list[str] = []
        errors: list[str] = []

        def guard(route) -> None:
            if route.request.url.startswith(self.origin + "/"):
                route.continue_()
            else:
                foreign.append(route.request.url)
                route.abort()

        context.route("**/*", guard)
        if stored is not None:
            context.add_init_script(f"try {{ localStorage.setItem({STORAGE_KEY!r}, {stored!r}); }} catch (e) {{}}")
        if init:
            context.add_init_script(init)
        page = context.new_page()
        page.on("pageerror", lambda exc: errors.append(f"page error: {exc}"))
        page.on(
            "console", lambda msg: errors.append(f"console {msg.type}: {msg.text}") if msg.type == "error" else None
        )
        page.goto(self.url, wait_until="load")
        broken = page.evaluate(LOAD_IMAGES_JS)
        self.assertEqual(broken, [], "images that did not load")
        return page, foreign, errors

    def _check(self, page, theme: str, phone: bool) -> dict[str, list[str]]:
        return page.evaluate(
            CHECK_PAGE_JS,
            {
                "theme": theme,
                "phone": phone,
                "bar": BAR,
                "exceptions": list(EXCEPTIONS),
                "lightMin": LIGHT_MIN_LUMINANCE,
                "darkMax": DARK_MAX_LUMINANCE,
                "blockLightMin": BLOCK_LIGHT_MIN_LUMINANCE,
                "blockDarkMax": BLOCK_DARK_MAX_LUMINANCE,
                "blockArea": BLOCK_MIN_AREA_PX,
                "minTarget": MIN_TARGET_PX,
            },
        )

    def _assert_clean(self, findings: dict[str, list[str]], where: str) -> None:
        problems = [f"{kind}: {item}" for kind, items in findings.items() for item in items]
        self.assertEqual(problems, [], f"{where}:\n  " + "\n  ".join(problems))

    def _theme_of(self, page) -> str:
        lums = page.evaluate(REGION_THEMES_JS)
        if all(value >= LIGHT_MIN_LUMINANCE for value in lums):
            return "light"
        if all(value <= DARK_MAX_LUMINANCE for value in lums):
            return "dark"
        return "mixed " + ", ".join(f"{value:.3f}" for value in lums)

    def _pick(self, page, label: str) -> None:
        button = page.locator(f".theme button:text-is('{label}')")
        button.click()
        self.assertEqual(button.get_attribute("aria-pressed"), "true")
        self.assertEqual(page.locator('.theme button[aria-pressed="true"]').count(), 1)

    def _pressed(self, page) -> str:
        return page.locator('.theme button[aria-pressed="true"]').inner_text()

    def _shot(self, page, name: str, *, full_page: bool = True) -> None:
        if self.shots:
            page.screenshot(path=str(self.shots / f"{name}.png"), full_page=full_page)

    # -- tests --

    def test_every_width_and_theme(self) -> None:
        for viewport in VIEWPORTS:
            phone = viewport in PHONES
            for name, scheme, stored, theme in STATES:
                where = f"{viewport[0]}x{viewport[1]} {name}"
                with self.subTest(where):
                    page, foreign, errors = self._open(viewport, scheme, stored)
                    self.assertEqual(
                        page.evaluate("document.documentElement.dataset.theme || 'auto'"), stored or "auto"
                    )
                    self.assertEqual(page.evaluate("getComputedStyle(document.documentElement).colorScheme"), theme)
                    self._assert_clean(self._check(page, theme, phone), where)
                    self.assertEqual(self._pressed(page), {None: "Auto", "light": "Light", "dark": "Dark"}[stored])
                    self._shot(page, f"{viewport[0]}x{viewport[1]}-{name}")
                    self.assertEqual(foreign, [], "requests outside the site")
                    self.assertEqual(errors, [])

    def test_picker_switches_the_whole_page_and_is_remembered(self) -> None:
        theme_colours = "[...document.querySelectorAll('meta[name=theme-color]')].map(m => m.content)"
        for viewport in (PHONES[1], DESKTOP):
            phone = viewport in PHONES
            with self.subTest(f"{viewport[0]}x{viewport[1]}"):
                page, foreign, errors = self._open(viewport, "light")
                self.assertEqual(self._theme_of(page), "light")
                self.assertEqual(self._pressed(page), "Auto")
                self.assertEqual(page.evaluate(theme_colours), ["#1c3d6e", "#152c50"])
                self._pick(page, "Dark")
                self.assertEqual(self._theme_of(page), "dark")
                self.assertEqual(page.evaluate(f"localStorage.getItem({STORAGE_KEY!r})"), "dark")
                self.assertEqual(page.evaluate(theme_colours), ["#152c50", "#152c50"])
                self._assert_clean(self._check(page, "dark", phone), "after picking Dark")
                page.reload(wait_until="load")
                self.assertEqual(self._theme_of(page), "dark", "the choice was not remembered")
                self.assertEqual(self._pressed(page), "Dark")
                self._pick(page, "Light")
                self.assertEqual(self._theme_of(page), "light")
                self.assertEqual(page.evaluate(f"localStorage.getItem({STORAGE_KEY!r})"), "light")
                self.assertEqual(page.evaluate(theme_colours), ["#1c3d6e", "#1c3d6e"])
                self._assert_clean(self._check(page, "light", phone), "after picking Light")
                self._pick(page, "Auto")
                self.assertIsNone(page.evaluate(f"localStorage.getItem({STORAGE_KEY!r})"))
                self.assertIsNone(page.evaluate("document.documentElement.getAttribute('data-theme')"))
                page.emulate_media(color_scheme="dark")
                self.assertEqual(self._theme_of(page), "dark", "Auto does not follow the system")
                page.emulate_media(color_scheme="light")
                self.assertEqual(self._theme_of(page), "light", "Auto does not follow the system")
                self.assertEqual(foreign, [])
                self.assertEqual(errors, [])

    def test_remembered_theme_is_in_place_before_the_first_paint(self) -> None:
        # Record the theme at the moment <body> is created: before that, nothing can paint.
        watch = r"""
          new MutationObserver((records, observer) => {
            if (!document.body) return;
            window.__atBody = {
              theme: document.documentElement.getAttribute('data-theme'),
              background: getComputedStyle(document.documentElement).backgroundColor,
            };
            observer.disconnect();
          }).observe(document, { childList: true, subtree: true });
        """
        for stored, scheme, background in (
            ("dark", "light", "rgb(20, 24, 30)"),
            ("light", "dark", "rgb(255, 255, 255)"),
        ):
            with self.subTest(stored=stored):
                page, _foreign, errors = self._open(PHONES[0], scheme, stored, init=watch)
                self.assertEqual(page.evaluate("window.__atBody"), {"theme": stored, "background": background})
                self.assertEqual(errors, [])

    def test_blocked_storage_falls_back_to_auto(self) -> None:
        block = r"""
          Object.defineProperty(window, 'localStorage', { configurable: true, get() { throw new DOMException('blocked', 'SecurityError'); } });
        """
        page, foreign, errors = self._open(PHONES[1], "dark", init=block)
        self.assertEqual(self._theme_of(page), "dark")
        self.assertEqual(self._pressed(page), "Auto")
        self._pick(page, "Light")
        self.assertEqual(self._theme_of(page), "light", "the picker stopped working without storage")
        self.assertEqual(errors, [])
        self.assertEqual(foreign, [])

    def test_reduced_motion_stops_animation(self) -> None:
        page, _foreign, errors = self._open(DESKTOP, "light", reduced_motion="reduce")
        self.assertEqual(page.evaluate("document.getAnimations().length"), 0)
        self.assertEqual(page.evaluate("getComputedStyle(document.documentElement).scrollBehavior"), "auto")
        self.assertEqual(errors, [])

    def test_local_targets_resolve_in_the_browser(self) -> None:
        page, foreign, errors = self._open(DESKTOP, "light")
        statuses = page.evaluate(
            r"""
            async (site) => {
              const urls = new Set();
              for (const el of document.querySelectorAll('[src], [srcset], [href], meta[content]')) {
                for (const attr of ['src', 'srcset', 'href', 'content']) {
                  let value = el.getAttribute(attr);
                  if (value && value.startsWith(site)) value = value.slice(site.length) || 'index.html';
                  else if (attr === 'content' || !value || value.startsWith('#') || value.includes(':')) continue;
                  for (const part of value.split(',')) urls.add(new URL(part.trim().split(/\s+/)[0], location.href).href);
                }
              }
              const out = {};
              for (const url of urls) out[url] = (await fetch(url, { method: 'HEAD' })).status;
              return out;
            }
            """,
            SITE_URL,
        )
        self.assertIn(self.origin + "/images/brand/og-card.png", statuses, "the share card is not checked")
        self.assertEqual({url: code for url, code in statuses.items() if code != 200}, {})
        for url in statuses:
            path = unquote(urlsplit(url).path).lstrip("/")
            self.assertTrue((SITE_DIR / path).is_file(), path)
        self.assertEqual(foreign, [])
        self.assertEqual(errors, [])

    def test_keyboard_focus_is_visible(self) -> None:
        # Tab through the whole page in both themes; every stop must draw an outline of at least 2 px.
        outline = r"""
          () => {
            let el = document.activeElement;
            if (!el || el === document.body) return null;
            const name = el.tagName.toLowerCase() + (el.id ? '#' + el.id : '') + ' "' + (el.textContent || '').trim().slice(0, 30) + '"';
            const cs = getComputedStyle(el);
            return { name, style: cs.outlineStyle, width: parseFloat(cs.outlineWidth) };
          }
        """
        for viewport, scheme in ((PHONES[1], "dark"), (DESKTOP, "light")):
            with self.subTest(f"{viewport[0]}x{viewport[1]} {scheme}"):
                page, _foreign, errors = self._open(viewport, scheme)
                seen, problems = 0, []
                for _ in range(120):
                    page.keyboard.press("Tab")
                    stop = page.evaluate(outline)
                    if stop is None:
                        break
                    seen += 1
                    if stop["style"] == "none" or stop["width"] < 2:
                        problems.append(f"{stop['name']}: outline {stop['style']} {stop['width']}px")
                self.assertGreater(seen, 10, "too few focus stops: is the page reachable by keyboard?")
                self.assertEqual(problems, [])
                self.assertEqual(errors, [])


if __name__ == "__main__":
    unittest.main()
