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
.page {
  width: 100%;
  max-width: 440px;
  display: flex;
  flex-direction: column;
  gap: 16px;
}
.card {
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 16px;
  padding: 28px 24px;
  box-shadow: 0 1px 3px rgba(0,0,0,0.06);
}
h1 {
  margin: 0 0 20px;
  font-size: 2.0625rem;
  font-weight: 600;
  text-align: center;
}
.header-card {
  display: block;
  text-align: center;
  text-decoration: none;
  color: inherit;
  cursor: pointer;
  box-shadow: 0 2px 8px rgba(0,0,0,0.10);
  transition: transform 0.08s ease, box-shadow 0.08s ease;
}
/* Only for real pointers (mouse/trackpad) — a :hover rule on a touch
   device makes iOS/mobile Safari eat the first tap just to trigger
   :hover, needing a second tap to actually follow the link. */
@media (hover: hover) and (pointer: fine) {
  .header-card:hover { box-shadow: 0 4px 12px rgba(0,0,0,0.14); }
}
.header-card:active {
  transform: scale(0.98);
  box-shadow: 0 1px 3px rgba(0,0,0,0.08);
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
.status-dot {
  display: inline-block;
  width: 9px;
  height: 9px;
  border-radius: 50%;
  margin-right: 7px;
  vertical-align: middle;
}
.dot-online { background: var(--status-good); }
.dot-waiting { background: var(--status-warning); }
.dot-offline { background: var(--text-muted); }
.sat-card .sat-title {
  margin: 0 0 2px;
  font-weight: 600;
}
.sat-card .sat-meta {
  margin: 0 0 4px;
  font-size: 0.8125rem;
  color: var(--text-secondary);
}
.sat-card form {
  margin-top: 10px;
}
.sat-card .slot-row {
  display: flex;
  align-items: center;
  gap: 8px;
  margin: 6px 0;
  font-size: 0.9375rem;
}
input, button {
  font: inherit;
  color: inherit;
}
input[type="number"], input[type="time"] {
  background: var(--page-bg);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 4px 8px;
}
button {
  background: var(--accent);
  color: #fff;
  border: none;
  border-radius: 6px;
  padding: 6px 12px;
  cursor: pointer;
}
button:hover { opacity: 0.9; }
details summary {
  cursor: pointer;
  display: inline-block;
  font-size: 0.8125rem;
  color: var(--text-secondary);
  letter-spacing: 0.02em;
  margin-top: 14px;
  background: var(--page-bg);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 6px 12px;
  list-style: none;
}
details summary::-webkit-details-marker { display: none; }
details summary:hover { border-color: var(--accent); color: var(--accent); }
details table { margin-top: 8px; }
.button-row {
  display: flex;
  gap: 8px;
  margin-top: 14px;
}
.button-row details {
  flex: 1;
  min-width: 0;
}
.button-row details summary {
  width: 100%;
  text-align: center;
  margin-top: 0;
  box-sizing: border-box;
}
.action-row {
  display: flex;
  gap: 8px;
  flex-wrap: wrap;
  align-items: center;
  margin-top: 10px;
}
.action-row form { margin-top: 0; }
table {
  width: 100%;
  border-collapse: collapse;
  font-size: 0.875rem;
}
table th, table td {
  text-align: left;
  padding: 4px 6px;
  border-bottom: 1px solid var(--border);
}
table th {
  color: var(--text-secondary);
  font-weight: 600;
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
  html += F("</style></head><body><div class='page'>");
  html += bodyHtml;
  html += F("</div></body></html>");
  return html;
}
