# 05 — File workflow

## 1. The problem

Files are prepared on a PC, often when the machine is off and often away from the workshop.
Today, getting a file onto FluidNC's SD card needs the PC and the machine on at the same
time. The goal (G9): save the file somewhere while preparing it, and later pick it up from
the display in the workshop with no PC present.

## 2. Model

The display, not FluidNC, is the place a file lands first:

```
inbox ──fetch──> display SD (/lrt/work) ──preview / transform──> upload ──> FluidNC SD ──> run
FluidNC SD ──fetch──┘                                    (only this step needs the machine on)
display SD ─────────┘
```

A **file source** is anything that can list files and fetch one. Three are planned:

| Source | List | Fetch | Needs |
|--------|------|-------|-------|
| Controller SD | `$Files/ListGcode` (as now) | HTTP GET `http://<address>/sd/<path>` (works, S1; not yet tried during a job) | Machine on |
| Display SD | Local | Local | Card carried from the PC |
| Inbox | See §3 | See §3 | See §3 |

## 3. Inbox options

### A. PC folder server

A small program on a PC serves one folder, read-only, over HTTP on the home network:
`GET /list` returns names, sizes and dates as JSON; `GET /file/<name>` returns a file. The
display lists and pulls on demand, so there is no sync delay: what is in the folder when you
tap Refresh is what you see. If the folder is inside OneDrive, files saved from any PC
appear there once OneDrive has synced that PC.

- Work: a script of well under 100 lines (Python or PowerShell) started at logon, a
  firewall rule, and a fixed name or address for the PC. About 150 lines on the display.
- Limit: the PC must be on, awake and at home when you are in the workshop. That suits an
  always-on desktop. It does not suit a laptop that is asleep or elsewhere.

Plain Windows folder sharing (SMB) is not proposed: it needs an SMB client on the ESP32,
and I don't know of a maintained one (not researched in depth).

### B. OneDrive directly

The display signs in to OneDrive once (a code and QR shown on screen, approved on your
phone), then lists and downloads from one chosen OneDrive folder over HTTPS.

- Meets the goal with no PC on at all.
- Work: a one-time app registration with Microsoft, token storage and refresh on the
  device, HTTPS downloads. Needs internet in the workshop.
- Not yet verified on this hardware; spike S5 decides whether it is practical.

### C. Display SD card

Works from the first build, no network. Copy files to the card at the PC.

## 4. Decision

OneDrive directly (option B) is the preferred inbox (D17). An always-on PC that syncs
OneDrive exists, so option A is the fallback if spike S5 shows B is not practical.

1. M3: file-source interface with Controller SD and Display SD.
2. M1: spike S5, so the answer is known early.
3. M8: build B, or A if S5 fails. The source interface is the same either way.

## 5. On the controller

- Generated and uploaded jobs go to one folder on FluidNC's SD (**Q7**: `/sd/lrt/` or the
  existing `/sd/fluidtouch/uploads/`).
- Uploading a name that exists overwrites it after a confirm.
- **Q6** Should the display offer to delete old uploaded jobs, e.g. keep the newest 20?

## 6. PC side

Independent of the inbox choice, the G-code Editor can gain "Save to CNC inbox" (save a
copy into the inbox folder), so rotated or checked files land in the right place. **Q4**
