# Building quests: the HH GtS quest wiki

How to write quests for *The Hitchhiker's Guide to Solana*, the game on the Solana
BP26 badge: what a quest is, every field and step type, how the engine behaves,
patterns that work, how to test, and how to ship one. It's written for organisers,
QuestMakers and the AI agents they hand it to. Everything here matches the server's quest
engine and validator; where this guide and the server's API contract (`server/API.md`,
"Quest definition") disagree, the contract wins.

It lives in the Solana OS repo next to the Lua app API (`docs/LUA_API.md`), and the badge
site shows it at `/apps/docs/quests` (raw Markdown: `/docs/quests.md`). Paths like
`/admin/quests` are pages of the badge site; `server/...` files are in the badge server's
repo (solana-bp26-badges, private), and this guide says what you need from them.

**Who builds quests:** organisers (the site's admins) and **QuestMakers**, people an admin
gave that role in `/admin/admins` (Team). A QuestMaker gets the quest tools in `/admin`
(Quests, Characters, Beacons, the beacon simulator and their own quests' activity) and
nothing else. They change, delete and test only the quests they made; everyone else's
open read-only. Admins can change any quest.

**For AI agents:** you can't save quests yourself (the admin API needs an organiser's or
QuestMaker's signed-in session). Your job is to produce one **QuestDef JSON object** per
quest that they paste into `/admin/quests`. Write new quests with `"active": false`, so
they can be tested before anyone else sees them. Read "Rules that the validator doesn't
check" and "Checklist for AI agents" at the end before you answer.

## How quests work

- Quests are **data on the server**. Nothing quest-specific ships on the badge, so a new
  quest goes live the moment it's saved with `"active": true`, with no firmware update.
  Saved with `"active": false` it isn't live: only the people testing it can play it
  (see "Testing a quest").
- A quest is given by a **character** (Pip Thumbwell, Dr. Hexa Strain, ...). Characters
  live on **beacons**: always-on ESP32 boards around the venue, or badges running their
  Beacon app. One character can be on several beacons; a beacon plays one character at a time.
- A player opens **Quests** on their badge near a beacon. The badge talks to the beacon
  over ESP-NOW (short-range radio), and the beacon relays every request to the server.
  The badge's Wi-Fi can be off: the beacon is its link.
- The server answers with dialog: the character's line and up to 4 options. Picking an
  option can accept a quest, complete a step, give items, pay !LAMPs and so on.
- Progress belongs to the **player's account**, not the badge. Rewards (!LAMPs, Guide
  pages, packs, items) land in the account at once and show on the web too
  (`/quests`, `/wallet`, `/inventory`, `/guide`). The !LAMPs, pages and packs go only to
  players who linked X (see "Rewards").
- The web emulator can play quests too: a screen beacon or a badge beacon shows a QR code, and
  scanning it puts the emulated badge next to that beacon for 3 minutes.

What players see:

- On the badge: the dialog screen (who's talking, the quest title, the text, the options),
  toasts like `Got: Pip's towel` and `+50 !LAMPs`, LED flashes, and a quest log with each
  active quest's current step and any countdown.
- On a beacon with a screen and speaker: the character's animated sprite, and the
  dialog line spoken aloud in the character's voice.
- On the web, `/quests`: every active quest (signed out too), the player's progress,
  rewards and deadlines.

## Anatomy of a quest

A quest is one JSON object, a `QuestDef` (the comments mark what goes there; real JSON has none):

```jsonc
{
  "id": "towel-run",
  "title": "The Towel Run",
  "summary": "Carry a fresh towel from Pip to Captain Slot before the clock runs out.",
  "giver": "pip",
  "requires": [],
  "repeatable": false,
  "active": true,
  "intro": [ /* the offer conversation: DialogNode[] */ ],
  "steps": [ /* what the player does, in order: Step[] */ ],
  "reward": { "lamps": 50, "pages": ["always-bring-a-towel"] }
}
```

| Field | Required | Rules | What it does |
|---|---|---|---|
| `id` | yes | 3-32 chars, `a-z 0-9 -`, starts with a letter or digit, unique | Permanent key. Progress is stored under it; never rename a live quest |
| `title` | yes | ≤ 48 chars | Quest log and dialog header |
| `summary` | yes | ≤ 140 chars | One line on `/quests` and in the badge's quest list |
| `giver` | yes | a character id | The quest is offered at that character's beacons |
| `requires` | no | quest ids (not itself) | Quests the player must have completed once first. Until then the quest is `locked` |
| `repeatable` | no | default `false` | Can be started again after completing (after its `cooldown`); the reward pays every run |
| `cooldown` | no | seconds, 300-604,800, default 3,600 | For a repeatable quest: how long after each run before it's offered again. `/quests` shows when |
| `active` | no | default `true` | Live for everyone. `false`: nobody new can start it, except the people testing it. Write new quests with `false`, test, then set `true` (see "Testing a quest" and "Editing live quests") |
| `intro` | yes | 1-32 dialog nodes, at least one option with `"action": "accept"` | The offer conversation |
| `steps` | yes | 1-20 steps, unique step ids | Done in order, one at a time |
| `reward` | no | see "Rewards" | Paid when the last step completes |

## Dialog

Dialog is a small graph of nodes. The conversation starts at the **first node** of the
list and moves along options.

```json
{
  "id": "start",
  "text": "Oh, hello! Captain Slot lost his towel again. Could you run this one over?",
  "options": [
    { "label": "Hand it over", "action": "accept" },
    { "label": "Why a towel?", "next": "why" },
    { "label": "Not now", "action": "decline" }
  ]
}
```

| Field | Rules |
|---|---|
| `id` | `a-z 0-9 -`, ≤ 32 chars, unique within this dialog |
| `text` | ≤ 400 chars. Aim for 120-250: the badge screen is 320 x 240 and scrolls, and beacons speak it aloud |
| `options` | 0-4 options. A node with no options ends the conversation (the badge shows the text and "SELECT close") |
| `options[].label` | ≤ 32 chars. Keep it short: it's a button on a small screen |
| `options[].next` | Node id to go to (must exist in the same dialog) |
| `options[].action` | `accept`, `decline`, `close` in an `intro`; `complete`, `close` in a step's dialog |

What an option does:

| Option | Effect |
|---|---|
| `next` only | Shows that node. Use it for questions, lore and branches |
| `"action": "accept"` (intro) | Starts the quest at step 1 |
| `"action": "complete"` (step dialog) | Completes the current step (a `deliver` step first takes the item, a `pay` step first charges the !LAMPs) |
| `"action": "decline"` or `"close"` | Ends the conversation with the character's idle line. Nothing is lost |
| neither `next` nor `action` | Same as close |
| an action **and** `next` | Does the action, then shows that node. Use it for a closing line after accepting or completing |

After `accept` or `complete` without `next`, the character carries straight on with
anything else the player now has to do **at this same character** (a `talk`, `deliver`
or `pay` step here shows its dialog, a `visit` here completes), or else says what's next:
`Next: Bring the towel to Captain Slot (Captain Slot)` or `Quest complete: The Towel Run`.
That's why the Towel Run's first step is a `talk` at the giver: accepting flows straight
into "here's the towel".

Within one conversation only the first `accept` or `complete` counts; later ones in the
same conversation are ignored. A conversation also ends when the player walks away: the
session lapses 30 s after the badge's last request (see "Checkpoints and failure").

## Steps

Every step has:

| Field | Required | Rules |
|---|---|---|
| `id` | yes | `a-z 0-9 -`, ≤ 32 chars, unique in the quest. Shows in the admin funnel |
| `title` | yes | ≤ 48 chars. The quest log line: write it as an instruction ("Bring the towel to Captain Slot") |
| `type` | yes | one of the seven below |
| `at` | for `talk`, `visit`, `deliver`, `pay` | A character id: the step happens at that character's beacons |
| `checkpoint` | no | `true`: reaching this step saves progress here |
| `timeLimit` | no | Seconds, 10 to 2,592,000 (30 days). Miss it and the player goes back to the last checkpoint |
| `onStart` | no | ≤ 8 actions, run when the step becomes current |
| `onComplete` | no | ≤ 8 actions, run when the step completes |

### The seven step types

| Type | Completes when | Needs |
|---|---|---|
| `talk` | The player picks a `complete` option in the step's dialog at `at` | `at`, `dialog` (with a `complete` option) |
| `visit` | The player opens Quests at a beacon of `at`. No dialog: arriving is enough | `at` |
| `deliver` | The player picks `complete` at `at` **and** has an item of `take.kind` that this quest gave; it's taken | `at`, `dialog`, `take: {kind}` |
| `pay` | The player picks `complete` at `at` and has `amount` !LAMPs; they're charged | `at`, `dialog`, `amount` (1-1,000,000) |
| `collect` | The player swaps X Dex cards with `count` different people after the step started | `collect: {count, filter?}` |
| `item` | The player owns an item of `match.kind` whose `data` has the `match.where` values | `match: {kind, where?}` |
| `app` | A quest app calls `quest.report(event)` with this step's `event` | `event` (≤ 32 chars) |

Details that matter:

- **talk**: the step's dialog shows whenever the player opens Quests at a beacon of `at`
  while this step is current. A `close` option leaves it open for later.
- **visit**: completes the moment the player talks to that character, before any other
  dialog. Several visit steps in a row at the same character complete together.
- **deliver**: the item must come from this quest (`give` in an earlier action of this
  quest; the validator checks that some earlier action gives that kind). A shop-bought
  or traded copy doesn't count. Without it the character says "You don't have it yet.
  Come back when you do." and nothing changes.
- **pay**: without enough !LAMPs the character says "You need 20 !LAMPs." and nothing
  changes. The charge shows in the player's wallet under the quest's title. Say the
  price in the option label ("Pay 20 !LAMPs"): people should know before they pick.
- **collect**: counts different people (badge owners: one friend's several badges count
  once) whose X Dex swap with the player reaches the server while the step is current (swaps sync when either badge is online). Only swaps with
  people who linked X count (anti-farming), unless the server runs without X. `count` is
  1-100. A `filter` narrows which badges count:
  - `prefix`: badge ids starting with these characters (badge ids are 6 Crockford
    characters, uppercase; emulator badges start with a lowercase `e`).
  - `range`: `["A00000", "MZZZZZ"]`, inclusive, by badge id.
  - `hashMod`: `[m, lo, hi]`: the first byte of sha256(badge id), mod `m`, is in
    `lo..hi`. Splits all badges into `m` stable groups: `[4, 0, 0]` is a quarter of them.
- **item**: checked when the step starts and every time the server looks at the player's
  quests (any beacon conversation, the Quests app or `/quests` loading). Only items from
  quests, the shop, admins or **this quest's own apps** count, never ones an unrelated
  store app made. `where` values are strings, numbers, booleans or null (≤ 512 bytes);
  every key must match exactly. If the player already owns a match when the step starts,
  it completes at once.
- **app**: only the apps this quest grants (`grantApp` in its actions) may complete it.
  A quest that grants no app accepts any app with status `quest`.

## Actions

`onStart` and `onComplete` are lists of actions. One action may hold several keys; it
needs at least one.

| Key | Value | Effect |
|---|---|---|
| `give` | ItemSpec | Puts an item in the player's inventory (toast `Got: <name>`) |
| `take` | `{ "kind": "towel" }` | Removes one item of that kind (this quest's own first) |
| `lamps` | 1-100,000 | Pays !LAMPs (once per run, see below) |
| `page` | a Guide page id | Adds a copy of that page (once per run) |
| `grantApp` | `{ "id": "virus-lab", "ttl": 7200 }` | Gives the player a temporary quest app for `ttl` seconds (60 to 30 days) |
| `led` | 6 hex digits, `"14F195"` | Flashes the badge's LEDs in that colour |

An **ItemSpec**:

```json
{ "kind": "towel", "name": "Pip's towel", "data": { "color": "ACFF9B" }, "tradable": false }
```

| Field | Rules |
|---|---|
| `kind` | ≤ 32 chars of letters, digits, `_ . : -`. What steps match on |
| `name` | ≤ 40 chars. What players see |
| `data` | optional JSON object, ≤ 1 KB. What `item` steps match and apps read |
| `tradable` | Default **false** for items a quest gives (actions and rewards), so they can't be bought off someone else. Set `true` for keepsakes meant to be passed on |

Notes:

- `lamps` and `page` in actions pay once per run of the quest, even if a failure sends
  the player back over the same step. `give` and `grantApp` run again on a replay (the
  items were removed by the rollback); a replayed `onStart` doesn't `take` again.
- A badge answer carries one LED colour: the last `led` of the answer wins.
- Toasts on the badge are cut to 64 characters in all, so keep item names short.

## Rewards

Paid once when the last step completes, every run for a repeatable quest.

```json
"reward": {
  "lamps": 75,
  "pages": ["gulf-stream"],
  "packs": ["pack-tech"],
  "items": [{ "kind": "bracelet", "name": "Friendship bracelet", "tradable": true }]
}
```

| Field | Rules |
|---|---|
| `lamps` | 0-1,000,000 |
| `pages` | ≤ 10 Guide page ids |
| `packs` | ≤ 5 pack type skus (`starter`, `guide`, `pack-<category>`, or any an admin made). Packs arrive sealed; the player opens them in the Guide |
| `items` | ≤ 5 ItemSpecs. Default `tradable: false` |

When a quest completes, every item it gave becomes the player's for good: no later run
or rollback removes it.

With X linking on, only players who linked X get a quest's !LAMPs, pages and packs (from
the reward and from `lamps` and `page` actions); everyone gets the items, and the badge
says "Link X to earn quest rewards". And quests pay a player at most 1,000 !LAMPs a UTC
day, all quests together: the rest of a payout over that is dropped ("Daily quest !LAMPs
limit reached").

Sizing rewards: the welcome is 100 !LAMPs, a Guide Pack costs 100, a category pack 150,
and X Dex bounties are 10-100 per new person. The seeded quests pay 15-75 !LAMPs, plus a
page, pack or item. A quest worth 30 minutes of walking is worth about a pack. Pages are
the collectible: a rare page is a strong reward for a short quest. !LAMPs are a bounty
(highest balance at the end wins a prize), so keep big payouts for hard quests, and keep
repeatable quests small.

## Checkpoints and failure

Progress since the last checkpoint is **lost** when:

1. a step's `timeLimit` passes before it completes, or
2. the player walks away in the middle of a `talk` step's dialog (they opened it, didn't
   finish or close it, and 30 s passed without the badge talking to that beacon).

Losing progress means: back to the last checkpoint step (the first step if none was
reached), every item the quest gave from that step on is removed, and the checkpoint
step starts again (its `onStart` runs again). !LAMPs and pages already paid stay paid.

How to use them:

- Put `checkpoint: true` on a step the player shouldn't have to repeat, typically right
  after something hard or far away.
- Time limits count from the moment the step became current. The badge and `/quests`
  show the deadline. Tell the player the time in the dialog ("You'll have 15 minutes").
- Pair a time limit with a checkpoint on the step that starts the clock, so a miss means
  "go get another towel", not "start the quest over".
- Walking away mid-`talk` is a failure only for `talk` steps. Give every `talk` dialog
  a "Not yet" option with `"action": "close"` so players can leave cleanly.
- Failures are checked when the server next looks at the player (any conversation, the
  Quests app, `/quests`), not on a timer, but the outcome is the same.

## Quest apps

A quest can lend the player a Lua app for a while with `grantApp` (Patient Zero lends
Virus Lab for two hours). The badge downloads it through the beacon, so it works with
Wi-Fi off, and it appears in the player's Apps with its time left.

Rules:

- Only apps with status `quest`, or apps by the built-in Solana OS author, can be
  granted: never an ordinary user's store app.
- The app must be a single `main.lua` (no bundled files): beacons carry only that.
- Badges need Solana OS 0.5.0 or newer to install apps.
- While the quest is active, a grant that runs out is renewed for another `ttl`.
- The app uses the `quest` permission: `quest.report(event, data, cb)` completes `app`
  steps whose `event` matches, and `quest.active(cb)` lists the player's active quests.
  With the `items` permission it can update the items its quest gave (Virus Lab
  rewrites the virus's `strain`), which is how `item` steps get completed by play.
  The full API is the Lua app reference, `/apps/docs` on the site (`docs/LUA_API.md` here, sections `quest` and `items`).

Getting a quest app onto the server: either add it to the seed content
(`server/content/apps/<id>/` with `"status": "quest"`, see `server/content/README.md`),
or have its author submit it at `/apps/submit` and an organiser set its status to
`quest` in `/admin/apps`. A `quest` app is never listed in the App Store.

## Characters and beacons

- Characters are made in `/admin/characters`: id, name, title, bio, colour (6 hex),
  an **idle line** (what they say when there's no quest business), and optionally a
  Text-to-Speech voice, a chat persona and a sprite sheet.
- Every character a quest uses (`giver` and every step's `at`) needs at least one
  adopted beacon (`/admin/beacons`), or players can't do those steps.
- Moving a character to another beacon is an edit in `/admin/beacons`; it takes effect
  within a minute. Quests don't change.
- A character offers one quest at a time: the first one available to this player, in
  the server's quest order (seeded quests in file order, quests made in the admin page
  after them; a quest you're testing comes first). Others show up once it's done or
  locked. Spread
  quests across characters so the venue has many doors.
- Chat with a character (hold a screen beacon) is flavour only. It never moves quests.
- **Any badge can be a beacon**, with nothing erased: plug it in at
  `/admin/beacons/badge` (Solana OS 0.5.6 or newer), which joins it to Wi-Fi, pairs it
  and lets you adopt it with a character. Its launcher then has **Beacon**: while that
  app is open the badge plays the character (radio, quest relays, a scan code for the
  emulator); CANCEL stops it, and it reopens by itself after a restart. Its network must
  be on the mesh channel, like a beacon's. For an emulated badge, `/emulator` has the
  same panel for QuestMakers. Deleting it in `/admin/beacons` takes the app away again;
  the badge's owner keeps everything either way.

Seeded characters (check `/admin/characters` for the current list):

| id | Name | Title | Colour |
|---|---|---|---|
| `pip` | Pip Thumbwell | Hitchhiker at large | `ACFF9B` |
| `hexa` | Dr. Hexa Strain | Mesh virologist | `EB54BC` |
| `slot` | Captain Slot | Leader-schedule skipper | `64A8F2` |
| `lampwick` | Bo Lampwick | Keeper of the !LAMPs | `E8F180` |
| `whisper` | Madame Whisper | Rumor broker | `FF754A` |

## Things a quest can reference

The server checks every reference when you save. These are the seeded ids; organisers
may have added more (`/admin/pages`, `/admin/packs`, `/admin/apps`).

Pack skus: `starter` (5 pages, one rare or better), `guide` (5 pages, any category),
`pack-events`, `pack-culture`, `pack-tech`, `pack-releases`, `pack-ecosystem`,
`pack-lore`, `pack-history`, `pack-people` (5 pages of that category).

Quest apps: `virus-lab`.

Guide page ids by category (C common, R rare, E epic, L legendary):

| Category | Pages |
|---|---|
| events | breakpoint (L), online-hackathons, hacker-houses, local-meetups, side-events, first-breakpoint (R), hackathon-to-mainnet (R), hackable-badges (E) |
| culture | gm, open-source, chewing-glass, devnet-first, simds (R), purple-to-green (R), stack-exchange (E), ibrl (L) |
| tech | accounts-model, programs, transactions, slots-and-epochs, rent-exemption (R), priority-fees (R), cpi (E), pdas (L) |
| releases | versioned-transactions, solana-pay, wallet-standard, sign-in-with-solana, token-extensions (R), state-compression (R), agave (E), firedancer (L) |
| ecosystem | token-program, anchor, wallets, block-explorers, actions-and-blinks (R), solana-mobile (R), depin (E), one-global-state (L) |
| lore | the-guide, forty-two (L), lamps, your-badge, x-dex, beacons (R), always-bring-a-towel (R), dont-panic (E) |
| history | proof-of-history (L), tower-bft (E), turbine (R), gulf-stream (R), sealevel, loom-to-solana, mainnet-beta, tour-de-sol |
| people | validators, rpc-providers, core-contributors, stakers, superteam (R), hackathon-builders (R), solana-foundation (E), founders (L) |

Unmarked pages are common. Legendary pages are about 0.7 % of pack draws: a quest that gives
one is a big deal.

## Rules that the validator doesn't check

The server catches structure, lengths and references. These it doesn't, and they
matter:

- **Printable ASCII only** in everything a badge shows: titles, summaries, dialog text,
  option labels, step titles, item names. The badge fonts have no other glyphs, so no
  curly quotes (`'` and `"` only), no em or en dashes (use a comma, colon or `-`), no
  ellipsis character (`...`), no accents, no emoji.
- **Original characters and writing.** The game nods to the Hitchhiker's Guide (towels,
  42, don't panic) but uses no Douglas Adams characters, names or quotes.
- **Facts about Solana must be true.** If a line teaches something, check it against
  solana.com/docs. When in doubt, leave it out.
- **Say "!LAMPs"** (with the `!`) in player-facing text, and amounts like `1,250 !LAMPs`.
  The buttons are SELECT, CANCEL and the D-pad; never say "Back".
- **Be kind.** No dark patterns: say the cost before a `pay`, the time before a time
  limit, and what the player risks by walking away.
- **Reachable steps.** Every character in the quest needs a beacon, and every step must
  be possible at the event (a `collect` filter that matches no real badge is a dead end).

## Patterns

**Fetch and deliver (timed).** `talk` at A gives an item (checkpoint) > `deliver` at B
with a `timeLimit`. Missing the time sends the player back to A for a new item. See
`towel-run` below.

**Scavenger hunt.** A chain of `visit` steps at different characters, each a checkpoint
with a small `lamps` action and an `led` in that character's colour. Order matters:
steps happen one at a time. See `proof-of-visit`.

**Buy a secret.** One `pay` step whose reward is a rare page. The dialog sells it. See
`rumor-mill`.

**Go meet people.** `collect` with a `count` > `talk` back at the giver. Use a
`hashMod` filter to make players find a specific crowd ("someone from the blue team").

**Play a minigame.** `talk` gives an item and `grantApp` (checkpoint) > `app` step for
the first milestone > `item` step on the final state of the item (checkpoint) > `talk`
to hand it back with a `take` action. See `patient-zero`.

**Story arcs.** Chain quests with `requires`: part two shows as `locked` on `/quests`
until part one is done, which teases it. Give part two to a different character to
move players around the venue.

**Repeatable errand.** `repeatable: true` with a small reward. The player can run it
again an hour after finishing (or after its `cooldown`), so keep the reward small or the
steps long.

**Branching talk.** Lore nodes that loop back (`next` to an earlier node) are fine;
make sure every path can reach the `accept` or `complete` option or an exit.

## A full example

A new quest for Madame Whisper: hear a rumour, fetch a sealed note from Bo Lampwick
within 20 minutes, bring it back unopened. It shows branching, `close`, a checkpoint
with a time limit, `deliver`, actions and a mixed reward. Like every new quest it's
saved with `"active": false`: test it, then set it to `true`.

```json
{
  "id": "sealed-note",
  "title": "The Sealed Note",
  "summary": "Fetch a sealed note from Bo Lampwick for Madame Whisper. Twenty minutes, and don't peek.",
  "giver": "whisper",
  "requires": ["rumor-mill"],
  "active": false,
  "intro": [
    {
      "id": "start",
      "text": "You again. Good. Bo Lampwick is holding a note for me, sealed with wax. Fetch it and bring it back unopened. You'll have 20 minutes once he hands it over.",
      "options": [
        { "label": "I'll fetch it", "action": "accept" },
        { "label": "What's in it?", "next": "what" },
        { "label": "Not now", "action": "decline" }
      ]
    },
    {
      "id": "what",
      "text": "If I told you, it wouldn't need a seal. Let's say it's about which validators are next in the leader schedule. Allegedly.",
      "options": [
        { "label": "Fine, I'll go", "action": "accept" },
        { "label": "Too shady for me", "action": "decline" }
      ]
    }
  ],
  "steps": [
    {
      "id": "get-note",
      "title": "Collect the note from Bo Lampwick",
      "type": "talk",
      "at": "lampwick",
      "checkpoint": true,
      "dialog": [
        {
          "id": "hand",
          "text": "Whisper sent you? Of course she did. Here, one note, sealed. The clock starts now: 20 minutes to get it to her.",
          "options": [
            { "label": "Take the note", "action": "complete" },
            { "label": "Give me a minute", "action": "close" }
          ]
        }
      ],
      "onComplete": [
        { "give": { "kind": "sealed-note", "name": "Sealed note", "data": { "seal": "wax" } }, "led": "E8F180" }
      ]
    },
    {
      "id": "return-note",
      "title": "Bring the note to Madame Whisper",
      "type": "deliver",
      "at": "whisper",
      "timeLimit": 1200,
      "take": { "kind": "sealed-note" },
      "dialog": [
        {
          "id": "check",
          "text": "Seal intact. You have more self-control than most. Hand it over.",
          "options": [
            { "label": "Here it is", "action": "complete", "next": "thanks" },
            { "label": "Not yet", "action": "close" }
          ]
        },
        {
          "id": "thanks",
          "text": "Discretion is a currency too. Take this, and keep your ears open.",
          "options": []
        }
      ],
      "onComplete": [
        { "led": "FF754A" }
      ]
    }
  ],
  "reward": { "lamps": 40, "pages": ["turbine"] }
}
```

How it plays, once it's live: Whisper offers it only after `rumor-mill`. Accepting doesn't start a
conversation at Lampwick, so the badge says `Next: Collect the note from Bo Lampwick`.
At Lampwick's beacon the `get-note` dialog shows; taking the note saves a checkpoint and
starts step 2's 20-minute clock. Missing it removes the note and sends the player back
to Lampwick for a new one. At Whisper, `Here it is` takes the note, completes the
quest (40 !LAMPs and the Turbine page) and then shows the `thanks` node.

## Saving and shipping

Two ways to get a quest onto the server:

1. **Admin page**: `/admin/quests` > New quest (organisers and QuestMakers). The editor
   is the JSON, with a readable preview beside it; a new quest starts with
   `"active": false`. Save checks every field and reference and points at the failing
   path, e.g. `steps[1].dialog[0].options[0].next: unknown node 'thanks'`. Test it (next
   section), then set `"active": true` and save again: it's live at once. The quest is
   yours: the list shows who made each quest, and a QuestMaker can't change anyone
   else's.
2. **Seed content** (in the repo, for quests that should exist on every server): add
   it to `server/content/quests.json`, run `bun server/content/check.ts` (it also checks
   ASCII and that items a quest takes are given earlier), and open a pull request. The
   server upserts seeds on start, but skips any quest an admin has since edited.

Editing a seeded quest in the admin page detaches it from the seed: later deploys
won't overwrite your edit.

## Testing a quest

A quest that isn't live (`"active": false`) can be played by the people testing it, and
nobody else: it isn't on `/quests`, and a beacon playing its giver doesn't offer it to
anyone else. Admins can test any quest, QuestMakers their own. Tests play the saved
version, on your own badge or an emulator signed in as you.

What's different in a test:

- The quest is offered to you even if its `requires` aren't done, and at its giver it
  comes before that character's live quests, so you can test with a seeded character.
  Its title reads `Test: <title>` on the badge, in the quest log and on `/quests`.
- A test run **pays no !LAMPs** (actions or reward), **pages or packs**, and `pay` steps
  charge nothing. Completing it toasts `Test complete`.
- Items and quest apps work as usual, so every step can be played. Items a test gives are
  marked as test items: they can't be traded, and they count only for test runs (a live
  quest's `item`, `take` or `deliver` step ignores them).
- A test completion doesn't unlock other quests' `requires`.
- Its events show in the activity log marked `test`; the dashboard, leaderboards and
  funnels don't count them. Every payout or charge the test skipped gets its own line,
  `Withheld`: "500 !LAMPs, page towel not paid: test run", so a missing reward there is
  the test, not a broken quest.

Step by step:

1. Make sure the quest's characters exist (`/admin/characters`). For a test beacon,
   `/admin/beacons` > *New beacon with a key*, with the giver as its character, gives
   you a key.
2. Open `/admin/beacon-sim`, paste the key and start it: this browser tab is now a
   beacon. (Or make an emulated badge a beacon in another tab: `/emulator?badge=3`, the
   *Quest beacon* panel, then leave its Beacon app open.)
3. Open `/emulator` in another tab of the same browser, turn its Wi-Fi on once to get an
   id, claim it with the account you test with (the one that will press *Test it*), then
   turn Wi-Fi off. Keep both tabs visible. A real badge near a beacon playing the
   character works too (Solana OS 0.5.0 or newer for quest apps).
4. In `/admin/quests`, open the quest and press **Test it**.
5. On the emulated badge open Quests: the character is nearby and offers `Test: <title>`.
   Talk to start it.
6. For steps at another character, change the beacon's character in `/admin/beacons`
   (the sim picks it up at its next hello) or run a second sim tab with another key.
   For `collect` steps, `/emulator?badge=2` is a second badge (X Dex between the two;
   both owners need X linked unless the server runs without X).
7. Watch `/quests` (signed in as the tester) for progress and deadlines, and the quest's
   **Activity** (the button in the editor, or `/admin/activity?quest=<id>`) for every
   start, step, failure and completion.
8. Changed the quest? Save, then **Start over**: your test run and the items it gave
   are removed and the quest is offered again from the beginning. Do the same after a
   test completion to play it again.
9. When it works, set `"active": true` and save. That ends every test of it (test runs
   and test items are removed), and everyone, testers included, plays it from the start,
   for real. **Stop testing** ends your test without going live.

You can test several of your quests at once, for example both parts of a story arc:
items one test gives count for your other tests.

## Editing live quests

- Progress is stored as the **index** of the player's current step. Don't reorder,
  insert or remove steps in a quest people are playing: they'd jump to a different step.
  Fixing text, dialog, rewards and limits is safe; adding steps at the end is safe.
- `active: false` stops new players from starting it. Players mid-quest can still
  finish, and players who finished still see it as completed. Set it to test a big
  change first: only testers are offered it until it's live again.
- Deleting a quest is refused while another quest `requires` it. Prefer `active: false`:
  deleting takes it out of everyone's history.
- QuestMakers can change and delete only the quests they made. An admin can change
  anyone's; the quest keeps its maker.
- Rewards changed later don't touch players who already completed it.

## Watching how it does

- `/admin` dashboard (admins): the quests leaderboard (starts, completions, failures,
  players, completion rate, average time) for any time range. Open a quest for its
  **funnel**: how many players reached, finished and failed each step. A step where many
  fail or stall is too hard, too far, or badly explained. Test runs don't count.
- `/admin/activity`: filter by quest for the live log of `quest.start`, `quest.step`,
  `quest.fail` (with the reason: `time` or `left`), `quest.complete` and `quest.withheld`
  (what a run didn't pay or charge, and why: a test run, a player without X, or the daily
  quest !LAMPs limit); test runs are marked `test`. QuestMakers see the activity of their own quests here (players' names,
  never their emails).
- Beacon liveness is on `/admin/beacons`: a quest whose beacon is offline can't move.

## Checklist for AI agents

When asked to write a quest:

1. Ask for, or use from this guide, the character ids, page ids, pack skus and app ids
   that exist. Use only those, or say clearly which new character, page or app the
   organiser must create first.
2. Output **one JSON object per quest**, valid JSON (no comments, no trailing commas),
   with keys in this order: `id`, `title`, `summary`, `giver`, `requires`, `repeatable`,
   `cooldown`, `active`, `intro`, `steps`, `reward`. Omit optional keys you don't use, but set
   `"active": false` on a new quest so it can be tested before it goes live.
3. Check the limits: title and step titles ≤ 48, summary ≤ 140, dialog text ≤ 400
   (aim for 250), labels ≤ 32, item names ≤ 40, ≤ 4 options per node, ≤ 20 steps,
   ≤ 8 actions per list, ids `a-z 0-9 -`.
4. Every `intro` has an `accept` option; every `talk`, `deliver` and `pay` step has a
   `dialog` with a `complete` option, and should have a `close` option ("Not yet").
   Every `next` names a node in the same dialog.
5. `talk`, `visit`, `deliver` and `pay` steps have `at`; `deliver` has `take.kind` given
   by an earlier action of the same quest; `pay` has `amount` and says the price in its
   label; `collect`, `item` and `app` have `collect`, `match` and `event`.
6. Every string is printable ASCII: straight quotes, `-` not dashes, `...` not an
   ellipsis character, no emoji or accents. Say `!LAMPs`.
7. Original writing in the HH GtS tone: playful, a little absurd, kind to the player,
   and accurate about Solana.
8. After the JSON, list what the organiser or QuestMaker must check: beacons for each
   character, anything that has to be created first, and how to test it (*Test it* in
   `/admin/quests`, the beacon simulator and the emulator), then set `"active": true`.
