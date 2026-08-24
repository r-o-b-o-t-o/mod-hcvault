# mod-hcvault

The game-side half of the Hardcore Vault — a bank for the Hardcore community on ChromieCraft.

This module empties the vault character's mailbox into a table of its own, tells the website what the
vault is holding, and mails out the orders the website's operator has approved. It never decides what
to send: the website does that, and this module does it or reports why it could not.

## What it does

One cycle, every `HcVault.PollInterval` seconds:

1. **Collect.** Every deliverable mail in the vault character's mailbox is emptied: items are parked
   in the vault and merged into the stacks already there, money is added to the character's purse,
   anything written in the body is kept — along with a note of what came attached to it — and the
   mail row is deleted. COD mail is left
   alone — paying on the vault's behalf is not this module's decision.
2. **Push.** The complete stock and the character's purse go to the website. Always in full, never as
   a delta, so a push that is lost costs nothing but freshness.
3. **Forward.** Letters that came in on donations are sent on, then deleted locally once the website
   has them.
4. **Ask.** The website says which approved lines to mail and which recipients to describe.
5. **Do.** Items are pulled out of the vault — splitting stacks where the order asks for part of one,
   and merging what comes out back into full stacks — and put in the mail, under the subject
   `Hardcore Vault Order #237b142d`. Any replies the operator has written go out in the same pass.
   Recipients are looked up: name, class and level from the character cache, challenge mode from the
   challenge-modes table, and both from that table alone when the character has died.
6. **Report.** What came of it goes back, along with a fresh stock push if anything moved.

**The whole cycle is skipped while the vault character is online.** A logged-in player holds its mail
and its purse in memory and writes both back when it saves, which would undo anything done underneath
it. Nothing accumulates from a skipped cycle.

## Where the goods live

Not in bags, and not in the bank. A bank window runs out of room, which is what forced the vault onto
several characters before this module existed.

Instead the items stay as ordinary `item_instance` rows belonging to **nobody** — owner cleared, in no
container — and `mod_hcvault_stock` is the list of which rows those are. Counts and rolls are read
from the item itself, so the module's table and the truth cannot drift apart. The stock is read into
memory once at startup and written through on every change, so building the snapshot to push never
touches the database.

### Stacks are merged, twice

Donations arrive in whatever stacks the donor happened to have, so without help the vault fills up
with fives and sevens of things that stack to twenty. That matters: one mail holds twelve items, and
forty cloth held as eight fives is eight of them.

So it is packed in two places.

**On collection**, an item joins the stack already in the vault rather than sitting beside it. The
stack is repacked into the fewest rows the game allows — only `item_instance.count` is written, never
a whole row, so nothing else about an item can be disturbed by a merge. The vault therefore holds at
most one partial stack of anything, and the startup load repacks whatever it finds so an already
fragmented vault heals itself once.

**On delivery**, whatever a claim takes is merged again before it goes in the mail. A claim consumes
the smallest rows first, which keeps the odds and ends moving but can still split one stack across
two rows on the way out.

Between them, a line is refused for size only when the order is genuinely more than twelve full
stacks of the thing.

**What is never merged.** A merge keeps one row and destroys the other, so it only happens between
rows that cannot differ in anything but their count. The random-property roll is half the stack key,
so two rolls of the same item are already separate stacks and can never meet. Beyond that, anything
equippable is left alone — enchantments, durability and the name of whoever made it all belong to the
row, not the stack — with ammo the one exception, since it carries none of that. Timed items are left
alone too, rather than have a merge decide whose clock the survivor keeps.

On a stock 3.3.5 item set that admits 5,237 of the 5,340 stackable items, and the ones it turns down
are a handful of joke and test items that are somehow both stackable and equippable, plus conjured
food and water. Nothing the vault will ever be asked to hold in quantity.

## Tables

All in the characters database; see `data/sql/db-characters/`.

| Table | Holds |
| --- | --- |
| `mod_hcvault_stock` | One row per `item_instance` the vault is holding |
| `mod_hcvault_delivery` | Order lines already mailed. This is what makes delivery exactly-once |
| `mod_hcvault_letter` | Donation letters not yet accepted by the website |
| `mod_hcvault_letter_item` | What came attached to those letters. Cascaded from the letter |
| `mod_hcvault_reply` | Replies already mailed. Exactly-once, the same way deliveries are |

### Why the delivery table exists

The row is written **in the same transaction as the mail** — and so is the claim that took the goods
out of the vault. One transaction covers a whole order, so "the goods left the vault", "the mail
carrying them exists" and "we know they left" are one fact rather than three that a crash could
separate.

If the report back to the website is lost — a timeout, a restart, the site being down — the website
still has the line as approved and offers it again on the next poll. The module recognises it, skips
the vault entirely, and reports it delivered. The goods are never sent twice.

This is also why a line has to fit in one mail. Spread over two, a crash between them would leave
goods sent with no record, and the retry would send them again. A line needing more than 12 mail
slots is refused with a reason saying so instead.

### Why the letter table exists

The mail a letter came in is deleted the moment it is collected, so the letter has to outlive it. It
is buffered here and cleared only once the website has acknowledged it — otherwise a failed push
would be the thing that lost somebody's message.

The attachment list beside it is a record, not a claim on anything: the items themselves went into the
vault when the mail was collected. It is kept because a note is usually *about* the goods, and "the
axe is for whoever needs it, the rest is spares" is unreadable on its own.

## Installing

1. Clone into `modules/`, then re-run CMake and rebuild the worldserver.
2. Copy `conf/mod_hcvault.conf.dist` to your configuration directory as `mod_hcvault.conf` and fill
   it in.

The two settings with no sensible default are `HcVault.VaultCharacterGuid` and
`HcVault.Website.Passkey`. The module refuses to run without either, and says which one is missing.

`HcVault.Website.Url` wants `https://`: the passkey travels on every request and is the whole of the
authentication.

### Testing against a local HTTP site

A debug website with no TLS in front of it needs both of these:

```
HcVault.Website.Url = "http://127.0.0.1:5006"
HcVault.Website.AllowInsecureHttp = 1
```

Two settings rather than one, so plain HTTP is never something a mistyped URL can turn on by itself.
The module warns on every startup while it is enabled — it is the setting most likely to be switched
on for an afternoon and left on.

## Commands

| Command | Does |
| --- | --- |
| `.hcvault status` | What is configured, what is held, and what it will serve |
| `.hcvault sync` | Runs a cycle now instead of waiting for the timer |

Both require administrator and work from the console.

## Logging

Logs under `module.hcvault`, every line prefixed `[HCVault]` so it can be picked out of a console the
core does not otherwise label. Add the logger to your configuration:

```
Logger.module.hcvault = 4,Console Server
```

Levels are the core's own: 0 disabled, 1 fatal, 2 error, 3 warning, 4 info, 5 debug, 6 trace.

**4 (Info)** is the one to run with — what was collected, what was delivered, and anything refused.
**5 (Debug)** adds a line per cycle and per push. The stock `Appender.Console` is itself capped at
Info, so Debug lines reach `Server.log` and not the console until that is raised too.

## Finding a character that has died

A hardcore death **deletes the character**. The realm forgets it, the character cache stops answering
to its name, and eventually the `characters` row is purged outright — so asking the realm about a
dead donor or recipient gets the same answer as asking about a name nobody ever used.

That is not good enough for a card that has to tell "this person died" apart from "you typed the name
wrong", so the module falls back to the challenge-modes table, which keeps a row per character with
its name, class, level and how it ended, long after the realm has let go.

That lookup is `WHERE name = ? ORDER BY guid DESC LIMIT 1`, and it only runs when the realm has no
character by that name — a death, or a typo. It leans on the index challenge-modes keeps on `name`;
without one the query walks most of the table, which on a realm with 23k rows is milliseconds rather
than microseconds, on the world thread.

**The living character always wins.** Names are reused heavily, so a name whose previous holder died
and whose current holder is alive must resolve to the living one. The character cache is asked first,
and only when the realm has no such character does the challenge table answer, with the most recent
record. Nothing is ever delivered to a character the realm no longer has, whatever its record says:
the mail would go to a box nobody can open.

## Refusals

Approving a line on the website is not the last word. This module declines to deliver when:

- no character by that name exists on the realm;
- the character runs no challenge mode at all;
- its `challenge` mask does not satisfy `HcVault.AllowedChallengeMask` — exact match by default,
  because the server only lets characters trade with others running the identical set of challenges,
  so a character running *more* cannot receive the mail either;
- the character is dead and `HcVault.RefuseDeadRecipients` is on;
- the vault no longer holds enough of the stack, or is carrying less gold than was asked for;
- the line is more than twelve full stacks, which cannot fit in one mail.

Each comes back as a reason the website shows on the order card. The website also asks this module to
describe an order's recipient *before* anything is approved, which is what stops most of these ever
being approved in the first place.

## Threading

Anything touching the game — mail, items, the character cache — runs on the world thread, driven from
`WorldScript::OnUpdate`. Anything talking to the website runs on the worldserver's own io_context,
taken from `ServerScript::OnNetworkStart` and wrapped in a strand of its own.

The only thing crossing between them is the website's answer to step 4, which is parked under a mutex
and picked up on the next world tick. The stock snapshot is built on the world thread and handed over
as a finished string, so the in-memory vault is never read from a network thread.

The website client is taken once when a cycle starts and carried through every hop of it, rather than
read from a member at each one. `.reload config` replaces that member on the world thread, and every
hop after the first runs on a network thread — reading it there would be a race, and finding it null
a crash. A cycle finishes on the client it began with.

Reads on the world thread are batched and bounded. Emptying the mailbox used to load each attached
item with a query of its own, which is a synchronous round trip per item inside one tick; every
attachment in the box now arrives in one. The scan also takes at most a hundred mails per cycle, so a
box that piled up while the module was off drains over a few minutes instead of stalling a tick.

Database reads happen on the world thread only. `CharacterDatabase.Query` spins looking for a free
synchronous connection before it blocks on MySQL, which is no way to treat an io_context thread, so
the donation letters are read where the stock snapshot is and handed over as a finished string.
Clearing them afterwards is a queued `Execute`, which costs the calling thread nothing.

A cycle that somehow never reports is abandoned after a few times the HTTP timeout. `_busy` gates
everything, so without that the module would go quiet — no mail collected, no orders sent — and say
nothing but "a cycle is already running" until the next restart.

That handover is also where compression sits. Bodies over 1 KB are gzipped — the stock push is the
whole vault every cycle, repetitive JSON that goes out at about a ninth of its size — and it happens
on the network thread, after the snapshot has been handed over. A world tick never spends a moment on
it. Roughly 0.2 ms per push at zlib level 6, once a minute.

Database writes go through the core's async queue, because the statements involved — `item_instance`,
`mail`, `mail_items` — are registered `CONNECTION_ASYNC` and are only ever prepared on those
connections. Committing one directly takes a synchronous connection, finds the statement unprepared,
and asserts.

Which means a cycle can never read back what it has just written. It does not have to: the vault is
held in memory and written through, the character's purse is read once at the start of a cycle and
then counted by hand, and item counts are taken from the vault rather than from the row that was just
loaded. Each transaction is still atomic; it simply lands a moment later.
