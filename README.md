# epaper-calendar

ESP32 firmware that displays a **weekly calendar** on a Seeed XIAO e-paper board. It fetches events from Google Calendar via a Google Apps Script web app and renders them on an **800×480** e-paper screen — with proper **Thai text** support.

## Features

- 7-day week view (Mon–Sun) with date labels
- Time grid that auto-adjusts to your earliest/latest events
- Overlapping events laid out in up to 3 side-by-side lanes
- Today's column highlighted
- Thai grapheme-cluster text wrapping (vowels and tone marks render correctly)
- Auto-refresh at **00:01** every night via NTP
- Low power — ideal for always-on desk or wall display

## Hardware

| Component | Details |
|-----------|---------|
| Board | [Seeed XIAO e-paper driver board](https://wiki.seeedstudio.com/xiao_epaper/) |
| Display | 800×480 e-paper (`BOARD_SCREEN_COMBO 502` in `driver.h`) |
| MCU | ESP32 (built into the driver board) |

## Software Requirements

Install these libraries in the Arduino IDE (**Sketch → Include Library → Manage Libraries**):

| Library | Purpose |
|---------|---------|
| **TFT_eSPI** | Display driver (configured for the XIAO e-paper board) |
| **ArduinoJson** (v6+) | Parse calendar JSON |
| **LittleFS** | Font storage (built into ESP32 core) |

You also need the **Seeed XIAO e-paper board support package** and its `EPaper` driver (provides `driver.h` and `EPAPER_ENABLE`).

## Quick Start

### 1. Clone the repository

```bash
git clone https://github.com/aiiaor/epaper-calendar.git
cd epaper-calendar
```

### 2. Set up Google Apps Script

Create a [Google Apps Script](https://script.google.com/) project that reads your Google Calendar and returns a JSON array. Deploy it as a **Web app** (Execute as: Me, Access: Anyone).

The firmware expects a response like this:

```json
[
  {
    "title": "Team standup",
    "start": "2026-06-29T02:00:00",
    "end":   "2026-06-29T02:30:00"
  },
  {
    "title": "ประชุมทีม",
    "start": "2026-06-30T03:00:00",
    "end":   "2026-06-30T04:00:00"
  }
]
```

| Field | Format |
|-------|--------|
| `title` | Event name (Thai supported) |
| `start` | ISO 8601 datetime in **UTC** (`YYYY-MM-DDTHH:MM:SS`) |
| `end` | ISO 8601 datetime in **UTC** |

The firmware converts UTC to **GMT+7** (Thailand). Change `TZ_OFFSET_H` in the sketch if you use a different timezone.

Minimal Apps Script example:

```javascript
function doGet() {
  const cal = CalendarApp.getDefaultCalendar();
  const now = new Date();
  const start = new Date(now.getFullYear(), now.getMonth(), now.getDate() - now.getDay() + 1);
  const end   = new Date(start.getTime() + 7 * 24 * 60 * 60 * 1000);

  const events = cal.getEvents(start, end).map(ev => ({
    title: ev.getTitle(),
    start: ev.getStartTime().toISOString().slice(0, 19),
    end:   ev.getEndTime().toISOString().slice(0, 19),
  }));

  return ContentService
    .createTextOutput(JSON.stringify(events))
    .setMimeType(ContentService.MimeType.JSON);
}
```

Copy the deployed **Web app URL** — you will need it in the next step.

### 3. Configure the sketch

Open `calendar_epaper_copy_20260627235307.ino` and edit the three values at the top:

```cpp
const char* ssid      = "your-wifi-name";
const char* password  = "your-wifi-password";
const char* scriptUrl = "https://script.google.com/macros/s/YOUR_SCRIPT_ID/exec";
```

### 4. Upload

1. Select the **Seeed XIAO e-paper** board in the Arduino IDE.
2. Open the serial monitor at **9600 baud** to watch connection and fetch logs.
3. Upload the sketch.

On boot the display shows *Connecting…*, then *Fetching calendar…*, then the weekly grid.

## How It Works

```
Wi-Fi connect → NTP sync → Fetch JSON from Apps Script
       ↓
Parse events → Compute overlap layout → Draw grid → E-paper refresh
       ↓
Loop: check time every 30 s → refresh at 00:01 daily
```

### Display layout

```
┌──────┬────────┬────────┬────────┬────────┬────────┬────────┬────────┐
│      │  Mon   │  Tue   │  Wed   │  Thu   │  Fri   │  Sat   │  Sun   │
│      │ 29/06  │ 30/06  │ 01/07  │ 02/07  │ 03/07  │ 04/07  │ 05/07  │
├──────┼────────┴────────┴────────┴────────┴────────┴────────┴────────┤
│  9   │  ┌──────────┐                                                    │
│      │  │ Event A  │                                                    │
│ 10   │  └──────────┘  ┌─────┐ ┌─────┐                                  │
│      │                │  B  │ │  C  │  ← overlapping events            │
│ ...  │                └─────┘ └─────┘                                  │
└──────┴──────────────────────────────────────────────────────────────────┘
```

- **Hour range** is computed from events (clamped between 10:00–18:00 minimum span).
- **All-day events** (start at 00:00) are skipped in the time grid.
- Up to **30 events** per fetch (`MAX_EVENTS`).

### Thai text rendering

Standard character-by-character wrapping breaks Thai vowels and tone marks. This firmware groups characters into **grapheme clusters** (consonant + vowel + tone) and draws each cluster as one unit using the embedded **Sarabun** smooth font (`Sarabun7.h`).

## Project Structure

```
epaper-calendar/
├── calendar_epaper_copy_20260627235307.ino   # Main firmware
├── driver.h                                   # Board / screen selection
├── Sarabun7.h                                 # Embedded Thai font (PROGMEM)
├── .gitignore
└── README.md
```

## Configuration Reference

| Constant | Default | Description |
|----------|---------|-------------|
| `TZ_OFFSET_H` | `7` | Timezone offset from UTC (hours) |
| `MAX_EVENTS` | `30` | Maximum events per fetch |
| `MAX_LANES` | `3` | Max side-by-side columns for overlaps |
| `FONT_LINE_H` | `14` | Line height for Sarabun font |
| `HOUR_START` / `HOUR_END` | auto | Recalculated from event times |

## Troubleshooting

| Symptom | Check |
|---------|-------|
| Stuck on *Connecting…* | Wi-Fi SSID/password, 2.4 GHz network |
| *HTTP error* on screen | Apps Script URL, deployment access set to "Anyone" |
| *JSON parse error* in serial | Script returns valid JSON array, not HTML |
| Thai text looks broken | `Sarabun7.h` is included; LittleFS mounted OK |
| Wrong times | Events sent as UTC; adjust `TZ_OFFSET_H` if needed |
| Empty grid | Calendar has events this week; check script date range |

## License

MIT
