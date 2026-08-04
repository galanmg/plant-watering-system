// Shared page shell + design tokens for every hub web page (status,
// schedule, etc. as they get built). Keep this the single source of visual
// style so new pages stay consistent — don't hand-roll CSS per page.
//
// System font stack only (no external fonts/CDN) — the hub must render
// reliably on a bare local network with no internet dependency for its UI.

#pragma once

#include <Arduino.h>

const char PAGE_CSS[] PROGMEM = R"rawliteral(
:root {
  color-scheme: light;
  --page-bg: #f9f9f7;
  --surface: #fcfcfb;
  --text-primary: #0b0b0b;
  --text-secondary: #52514e;
  --text-muted: #898781;
  --border: rgba(11,11,11,0.10);
  --accent: #2a78d6;
  --status-good: #0ca30c;
  --status-warning: #fab219;
  --status-serious: #ec835a;
  --status-critical: #d03b3b;
}
@media (prefers-color-scheme: dark) {
  :root {
    color-scheme: dark;
    --page-bg: #0d0d0d;
    --surface: #1a1a19;
    --text-primary: #ffffff;
    --text-secondary: #c3c2b7;
    --text-muted: #898781;
    --border: rgba(255,255,255,0.10);
    --accent: #3987e5;
  }
}
* { box-sizing: border-box; }
body {
  margin: 0;
  min-height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 24px;
  background: var(--page-bg);
  color: var(--text-primary);
  font-family: system-ui, -apple-system, "Segoe UI", sans-serif;
}
.card {
  width: 100%;
  max-width: 420px;
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 16px;
  padding: 32px 28px;
  box-shadow: 0 1px 3px rgba(0,0,0,0.06);
}
h1 {
  margin: 0 0 24px;
  font-size: 1.375rem;
  font-weight: 600;
}
.stat-label {
  margin: 0 0 4px;
  font-size: 0.8125rem;
  color: var(--text-secondary);
  text-transform: uppercase;
  letter-spacing: 0.04em;
}
.stat-value {
  margin: 0;
  font-size: 2.75rem;
  font-weight: 600;
  font-variant-numeric: tabular-nums;
  color: var(--accent);
}
)rawliteral";

String pageShell(const String &title, const String &bodyHtml) {
  String html;
  html.reserve(600 + bodyHtml.length());
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width, initial-scale=1'>"
             "<title>");
  html += title;
  html += F("</title><style>");
  html += FPSTR(PAGE_CSS);
  html += F("</style></head><body><div class='card'>");
  html += bodyHtml;
  html += F("</div></body></html>");
  return html;
}
