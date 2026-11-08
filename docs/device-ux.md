# CosmoLibre — on-device UX

How the clock behaves when you touch it. Anything that needs typing (Wi‑Fi
password, city, page layouts) happens in the web UI instead; the clock only
offers what works well with three touch pads.

**Inputs:** three touch pads on top (Left · Middle · Right). Each can be
*tapped* or *held* (about 0.8 s).
**Output:** 280×480 e‑paper panel and a small speaker.

## Principles

- **The face is the product.** The menu is for occasional changes.
- **Middle carries the important actions** (select, menu, stop). Left and Right
  are read through a shared analogue input and are less reliable, so they only
  move or page — never anything destructive.
- **Destructive actions** (Forget Wi‑Fi) live in the menu, behind a confirm
  screen.
- **Gentle on the panel:** moving the cursor uses partial refreshes; entering or
  leaving the menu, and once an hour, does a full refresh to clear ghosting.
- **Gentle on the battery:** in power saver (the default), Wi‑Fi is off between
  syncs, and the radio and the panel are never busy at the same time.

## Clock face

| Gesture | Action |
|---|---|
| Tap Left / Right | Previous / next page (up to 4); restarts the rotation timer |
| Tap Middle | Stop or snooze a ringing alarm or timer; otherwise nothing |
| Hold Middle | Open the menu |

**Status line (bottom left):** empty when all is well. It appears only when
something needs attention — joining Wi‑Fi, no connection (`wifi fail r<code>`,
where the code is the Wi‑Fi disconnect reason), waiting for the time — or while
a timer or snooze is counting down. *Show IP address* (web UI › Settings) keeps
the IP there permanently. Page dots appear bottom right when there are two or
more pages.

**Pages** are designed in the web UI (each page holds up to 3 widgets). Their
order there is the order the clock flips and rotates through. Optional
**rotation** moves to the next page every 1, 2, 5, 10, 15, 30 or 60 minutes.

## Alarm or timer ringing

| Gesture | Action |
|---|---|
| Tap any pad | Stop |
| Hold Middle | Snooze 9 minutes (alarms only) |

Ringing stops by itself after 5 minutes. The screen shows a large ALARM or
TIMER card.

## Menu

| Gesture | Action |
|---|---|
| Tap Left / Right | Move up / down (wraps round) |
| Tap Middle | Select, or change the value |
| ↰ in the header + tap Middle | Back (from the top level: close the menu) |
| Hold Middle | Back — optional, *Settings › Hold to back* (on by default) |
| No input for 30 s | Close the menu |

The ↰ is the first position, just above the first item: from the first item,
tap Left once to reach it.

**Entering numbers** (alarm time, timer, volume, night hours): Left / Right =
−/+ 1, hold = −/+ 10, Middle = next field or confirm, hold Middle = cancel.

### Items

The top level follows the original Cosmo II menu — the **cosmolibre** wordmark
in the header, a dot cursor, line icons, letter‑spaced capitals:

| Item | What's inside |
|---|---|
| **Apps** | Rotate on/off · rotate every N minutes · jump to a page |
| **Alarm** | 3 alarms (6:30 / 7:30 / 8:30, off by default): on/off · time · days (every day / weekdays / weekends) |
| **Timer** | 5 / 10 / 15 / 30 minutes or custom; the countdown shows on the face |
| **Settings** | See below |
| **Sync clock** | Fetch the time and weather now (wakes Wi‑Fi if it's asleep) |

**Settings**

| Item | What's inside |
|---|---|
| **Sound** | Volume · touch beep · sync beep · test sound |
| **Night mode** | Quiet hours: no sounds except alarms |
| **Wi‑Fi** | See below |
| **Hold to back** | Whether holding Middle goes back in menus |
| **Info** | Firmware version, uptime, battery, IP, and a QR code for the web UI |
| **Clean screen** | Full refresh to clear ghosting |

**Wi‑Fi**

| Item | What it does |
|---|---|
| Network · IP · signal | Status (shows *Asleep (saver)* while the radio is off) |
| **Mode** | **Saver** (default): Wi‑Fi switches off between syncs. **Always**: stays connected |
| **Connect** | Turns Wi‑Fi on for 15 minutes and opens the **Web UI** screen: *Connecting…*, then the address and a QR code |
| **Setup hotspot** | Opens the *CosmoLibre* hotspot to choose a network (confirm screen) |
| **Web UI** | On / off (see below) |
| **Forget Wi‑Fi** | Erases the saved network and restarts (confirm screen) |

## Wi‑Fi power saver

- After joining and syncing, the radio switches **off**. The clock keeps time on
  its own.
- About every **45 minutes** it wakes, just after a minute change (so it doesn't
  coincide with a screen refresh), re-syncs the time and weather, and switches
  off again — usually within 10–15 seconds.
- If a wake can't connect, it tries again after 10 minutes and the status line
  shows `wifi fail r<code>`.
- While the web UI is open in a browser, the clock stays connected.
- *Mode › Always* keeps the old always-connected behaviour.

## Web UI on / off

- **On by default.** The setting survives restarts and firmware updates.
- **Off** stops the web server and withdraws the `cosmolibre.local`
  announcement, so nothing listens on your network. Time and weather still sync.
- It can be switched **off** from the clock or from the web UI (which asks for
  confirmation and explains the next point).
- It can only be switched back **on** from the clock (*Settings › Wi‑Fi › Web
  UI*), so you can't lock yourself out remotely.
- The setup hotspot is separate and always available.

## Wi‑Fi setup page

Joining the *CosmoLibre* hotspot opens a single page: a dropdown of nearby
networks (strongest first, with signal and a lock for secured ones, plus
*Other network…* for hidden ones), a password field with Show/Hide, and
Connect. If the last attempt failed it says why (for example, wrong password).
After saving, the clock restarts and joins.

## Not built yet

- **Alarm chimes:** alarms play a beep pattern for now.
- **Night mode** fewer refreshes overnight (quiet hours already silence sounds).
- **Custom greeting text** (needs text storage).
- **Alarms and timer in the web UI** (currently only on the clock).
