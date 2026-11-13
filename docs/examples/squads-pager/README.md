# Squads pager: the badge blinks when a proposal waits for you

The badge watches one Squads multisig and compares its open proposals with the wallet you
linked at `/profile`. A proposal you can still vote on shows `SIGN`, one that is approved
and that you may execute shows `EXEC`, and either blinks the LEDs orange and toasts once.
Otherwise the LEDs show the newest active proposal: its approvals in green, out of the
threshold in grey. The badge only reads; you sign wherever your key is.

- `main.lua`, `app.json`: the app (permissions `rpc`, `profile`, `led`). No server and no
  hosts: every read goes through `rpc`.
- `test.lua`: runs the app on a computer against mocked RPC answers (`lua test.lua` in
  this folder, Lua 5.5): a small multisig, a big one, one too big, missing, unreadable and
  heavily voted proposals. It isn't part of the app; leave it out of what you load.
- It needs the `solana` module (`solana.pda`, Solana OS 2.1.0) and says
  so on a badge that doesn't have it.
- Squads v4 multisigs and smart accounts both work: the app tells them apart by the
  program that owns the account.

## Telling it which multisig

In this order:

1. **Typed on the badge.** With no address yet, SELECT opens the keyboard (RIGHT does at
   any time). An address is 32 to 44 characters, so this takes a minute, once: the app
   keeps it in its `storage` as soon as the account reads as a multisig. A typo doesn't get
   that far (it isn't an address, or there's no such account), and the vault's address,
   the usual mix-up, is answered with *Not a Squads multisig*.
2. **A bundled `multisig.txt`.** Put the address in a file of that name next to
   `main.lua` and load the folder with Run on badge: no typing. Anyone who installs that
   build watches that multisig, so this is for your own badge or your team's, not for a
   store listing. An address typed later wins over the file.

Use the multisig's own address (for a smart account, its settings account), not the vault
that holds the funds. LEFT switches between mainnet and devnet.

## How it reads

Every 20 seconds, three calls of the 30 a minute a badge gets. A big multisig takes more
(below), so the app counts its calls: a round starts only when one as long as the last
fits the minute, and polling leaves four calls for SELECT. A multisig of 120 members with
eight long proposals is read about once a minute instead of running into "too fast".

A list that doesn't fit one read is read in pieces, by separate calls, and a vote can land
between two of them. Pieces answered from one slot are one state of the account. Otherwise
the account's first bytes, which hold the counts the rest was read by, are read once more:
if they changed, the pieces are thrown away and the account is read again as it is now.

1. `getAccountInfo` on the multisig: the threshold, the members and their permissions, the
   number of its latest transaction and the one below which proposals are stale. The
   first read takes 1,500 bytes, about 40 members.
2. For the newest 8 transactions that aren't stale, the proposal's address:
   `solana.pda({prefix, multisig, "transaction", number, "proposal"}, program)`. An address
   costs 5,000 of a callback's instructions for each bump it tries (two on average), so
   the app works one out per tick and remembers it.
3. `getMultipleAccounts` on those, four at a time with a `dataSlice` of 420 bytes each
   (up to 10 approvals), so that the answer stays under 3.5 KB.

**Lists are read whole.** When the members, or a proposal's approvals or rejections, go
on past what the first read held, the app reads the rest of that account with
`getAccountInfo` and a `dataSlice` offset, 2,400 bytes a call and at most three more calls
(about 260 members, or as many votes). A multisig bigger than that shows *Too many members
to read*, and such a proposal *Proposal #n has too many votes to read*: the app never takes
part of a list for all of it, so it never says your wallet isn't a member, or asks for a
signature you already gave, because it stopped reading.

**Missing is not unreadable.** A transaction without a proposal account is skipped. A
proposal account that exists but isn't the program's, or doesn't parse, stops the round
with *Can't read proposal #n*, and the screen keeps what the last good round read (the
seconds since then count up). It is never shown as "No open proposals".

Account data comes back as base64, which the app decodes itself, and every field is
checked before use: an answer that doesn't parse is shown as an error, not trusted.

**Trust.** What the badge shows is what one RPC node says. Use it as a nudge to go and
look, and read the transaction where you sign it.

Reference: [`crypto` and `solana`](../../LUA_API.md#crypto-and-solana-always-allowed-solana-os-210),
[`rpc`](../../LUA_API.md#rpc-permission-rpc-solana-os-052).
