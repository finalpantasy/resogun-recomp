# Collab tournaments and how the tie-in outfits were earned

Research for two launcher modes: Classic (the original 2018 requirements) and Hardcore (stricter).
Static analysis of eboot 1.17 (`work/game-1.17-prepared/eboot.elf`, addresses are guest virtual
addresses, file offset = address + 0x4000), the bundled data in `work/game-1.17/xbdata`, the title
server in `server/`, and 2018 news. Nothing here was run in the guest unless it says so.

## Summary

- The national tournament (US "International Tournament", JP 全国大会) is driven by the server:
  the board list names the events and stages, a per-event archive downloaded from the server
  carries the course, conditions and **prizes**, the server ranks the scores, and a server notice
  makes the game build a result notice in the Notices box. The prizes in that notice come from the
  downloaded settings file, and the game grants them itself with the item-grant function 0xcd0250
  when the player opens the notice.
- So our server can define the FF30, Level-5 and Famitsu cups with their own courses, conditions and
  prize item ids, and deliver the prizes through the game's own Notices flow. The parts already
  working (board, archive, entry, first tee) are in `server/golf_server.py`. The missing server
  parts are the second (final) stage row, reward keys in the archive, result fields in the
  own-score reply, and a notice with a `<nid>` tag. None of the notice/prize path has run in the
  guest yet.
- Fallback without the notice path: the runtime already sees every title HTTP request, including
  `gameapi_registscorenational` with the event id and score, and already knows how to grant items
  (`runtime_game.c`, 0xcd0250). It can grant prizes when a qualifying score is submitted.
- 11 parts are unusable as the task said (textures or model missing from both 1.00 and 1.17), plus
  the Famitsu, Chocobo and three Level-5 carts (no cart model). The Famitsu ranger final prize
  therefore cannot be given; a substitute is needed.

## 1. How a national tournament works in 1.17

### 1.1 Where the list, rules and course come from

| Step | Source | Evidence |
|---|---|---|
| Event list | Server: `gameapi_getboardlistnational` (slot 0x2f), then GET of the `0x4a60` suffix (slot 0x30). Rows `0x4120..0x4130`: `0x4145`, `0x4146` (stage, becomes entry +0x271), group `0x4140`, id `0x4147`, optional `0x4142..0x4144`, name `0x4150`, start/end BCD `0x4177`/`0x4178`. | Row parser 0xb84b63-0xb851ca; the parsed 0x258-byte row has the id at +0x108 (lookup 0xb850b0). `protocol.md` "Local national tournament". |
| Rules, course, prizes | Server: `gameapi_getnationalgamedata` (slot 0x31) returns a suffix; the GET (slot 0x32) is an FB archive written to `/download0/` and opened by the manager worker 0xa4de30. Nothing local: no `data/sponsor` in `xbdata` of 1.00 or 1.17. | 0xa4de30 opens `data/sponsor/param/International_Tournament_settings.txt` and `..._messages.txt`. `server/tournament_archive.py` builds this archive today. |
| Conditions table | Local: `xbdata/param/course_condition/course_condition.xb`, rows 1000-1095 with `Cup_Use`, `Tee_Use`, `Wind_Use`, `Mirror_Use`. The settings key `p<n>_regulation` picks a row. | 1014 = (0,0,0,0) is what the server sends now (commit 941b970). |

Settings file keys (read by 0xa4de30, key names at 0x1203874-0x1203a1a). Global: `resetThreshold`
(entry +0x229), `tournamentId` (+0x04), `sponsored_US` (+0x228). Per stage `p1_` (qualifier) and
`p2_` (final), stored in a 0x90-byte block at entry `+0x78 + stage*0x90` (stage 1 block shown):

| Key | Entry offset (stage 1) | Meaning (from the code) |
|---|---|---|
| `course` | +0x108 (byte) | Course number (Eagle City = 1). The entry screen needs it nonzero (0xc4e088-0xc4e0b2). |
| `hole` | +0x109, +0x10a | 0 = 18 holes, 1 = OUT 9, 2 = IN 9 (constant 0x90912 in 0xa4de30); 3+ kept as a count. |
| `regulation` | +0x10c | `course_condition` row (cup, tee, wind, mirror). |
| `time` | +0x110 | Stored as value*600+600. Meaning not traced. |
| `weather` | +0x114 | Not traced. |
| `limitation_gear`, `limitation_stats`, `limitation_reentry`, `slotMode` | +0x118..+0x11b (bytes) | Gear limit, stat (growth) limit, re-entry limit, slot mode. Value semantics not traced. |
| `rule_1`, `rule_2` | +0x11c, +0x120 | Looked up by id in the 0x34-entry rule table at [0x2c6b630] (+0x1a). |
| `reg_reward_normal_1`, `_2` | +0x124, +0x128 | Entry prize item ids (non-sponsored event). |
| `reg_reward_sponcored_1`, `_2` | +0x12c, +0x130 | Entry prize item ids when `sponsored_US` is set. |
| `score_reward_flag`, `_score`, `_itemId`, `_itemCount` | +0x134..+0x140 | Prize for a score at or under `_score`. |
| `rank_reward_flag`, `_value`, `_itemId`, `_itemCount` | +0x144..+0x150 | Prize for a rank at or under `_value`. |

The stage-2 block is the same at +0x90. Name: `tournamentName_US_01_ENG` in the messages file
(the loader drops the first and last character, so it is quoted).

### 1.2 Qualifier and final

Each stage is its own board row: same event, different `0x4147` id, `0x4146` = 1 (qualifier) or 2
(final). The game keeps stage-1 and stage-2 entries in two vectors of 0x1198-byte entries
([0x2c81150]+0x8 and +0x11c0) and pairs them by entry +0x278 (0x6c11f5-0x6c1370 searches for a
stage-1 and a stage-2 entry with the same +0x278). Which board word becomes +0x278 is not traced;
`0x4145` (the only word shared by both rows; the server calls it "version" today) is the likely one.

Entry gates: `gameapi_noticejoinnational` returns a join count (`0x4962`); when it exceeds
`resetThreshold` the entry screen goes to the S-ticket re-entry path (0xc4b8d6-0xc4b8f9,
0xc4c794-0xc4c832). That is the game's own "limited attempts" mechanism. How the final checks that
the player qualified is not traced (see the 0x42d1 flag below).

### 1.3 Score submission

At round end the game POSTs `gameapi_registscorenational?account=..&n=..&s=1` with the event
fields (`0x4140`, `0x4147`, `0x4142..0x4144`), the name/date stamp, two signed 64-bit scores
(`0x4500`, `0x4501`) and 14 detail records (builder 0xb5ad2a-0xb5b96b). Our server stores the
lowest first score per date and event (`--national-tournament --score-db`). The first score is
probably strokes against par as in Open Course (`OneHole_*` rows stored 1 for a bogey), not verified
for the national route.

### 1.4 Results and the finalist cut

The cut is decided by the server, not the game. The own-score reply (`gameapi_getmyscorenational`)
carries the result; parser 0xb731d4 / 0xb77575-0xb776a8:

| Record | Parsed into | Use |
|---|---|---|
| `0x42d1` | 0 = keep, 1 = set bit 0x40 of the result flags, 2 = clear the top bits (0xb7323e-0xb7758a). The server sends 2 now. | `FUN_006c7ad0` copies entry +0x117c bit 0x40 to the notice (the debug label list has `IsPassed`). Most likely "qualified for the final" (inferred). |
| `0x4270` | rank (rsp+0x3790, entry +0x2d0) | Compared with `rank_reward_value`; rank/entrants percent picks the notice tier (5, 20, 50%; `FUN_006c7ad0`). |
| `0x4500` x up to 10 | scores (entry +0x2c0, +0x2c8) | First score compared with `score_reward_score`. |
| `0x4275` (optional) | rsp+0x4644 | Likely the entrant count (entry +0x2a0 is the divisor for the tier); not traced. |

### 1.5 Prize delivery through the Notices box

1. The notice list (`gameapi_getinfolist`, slot 0x05) items carry a `0x3030` string. A
   `<nid>N</nid>` tag stores N at item +0x343c of the notice list ([0x2c8f2e0]+0x10378, stride
   0x2b18, count +0x1b81c; parser 0xb68d42 and 0xb69437, 0xb6949b). (`<bid>` goes to +0x3440.)
2. The Notices box task (`FUN_006c07d0`, 0x6c11f5-0x6c1379) turns each such item into a national
   notice when it finds a stage-1 and a stage-2 entry whose +0x278 equals N.
3. `FUN_006c97a0` builds the prize list from that entry's stage block (stage byte +0x271):
   - `reg_reward_*_1/_2` (sponsored or normal by +0x228): an id > 0 adds that item, count 1;
     an id < 0 draws a random item from the shop pool (0x5edde0, as Lucky Chance does) and grants it
     at once (0x6c9b49, 0x6c9ce9).
   - `score_reward`: flag set and score (+0x2c0) <= `score_reward_score` adds `itemId` x `itemCount`.
   - `rank_reward`: flag set and rank (+0x2d0) <= `rank_reward_value` adds `itemId` x `itemCount`.
4. Opening the notice runs `FUN_006c8900`: for each listed prize, `0xcd0250(record+0x234, 1, count)`
   (call at 0x6c89cf), the same grant the runtime uses for the unlocked mode. An item set
   (category 1000, e.g. 9128 "Famitsu T-shirt & cap set") is expanded into its parts by 0x879ad0.
   The save remembers opened notices (ring of 128 ids at save +0xfa1008) and national prizes
   (ring of 40 x 0x3c at +0xfa1584, item ids at +0xfa15b0..+0xfa15b6), so a prize is given once.

The bonus list (`gameapi_getbonuslist`/`gameapi_getbonus`, notice box 0x6c0651 and 0x6c5e9e) is a
separate code-redemption list (16-byte code `0x7504`, text, a u16 `0x7506`, dates); it is not
needed for tournament prizes.

### 1.6 Can our server run these cups?

Yes, with these server additions (none run in the guest yet):

| Piece | What | Status |
|---|---|---|
| Event catalog | FF30, Level-5 cup 1, Level-5 cup 2, Famitsu: course, holes, regulation, limits, prizes per stage. | New. |
| Board | Two rows per event (stage 1 and 2, same `0x4145`, distinct ids), windows rebased to the player's date. | One row today (`national_tournament_board`). |
| Archive | One settings archive per id with the reward keys. | `tournament_archive.py` writes course/hole/regulation only. |
| Results | Own-score reply with rank, `0x42d1` = 1 when the qualifier score makes the cut, entrants `0x4275`. Single player: rank from the cut table, not from other players. | Rank 1 and `0x42d1` = 2 today. |
| Notice | `getinfolist` item with `<nid>N</nid>` and a date once the stage closes. | Empty list fixture only. |

Unknowns to settle in the first guest run: which board word is +0x278, whether the final row is
enterable without the 0x40 flag, the time gate (if any) for showing the result notice, and the
meaning of `limitation_*` values. GOLF_BREAK candidates: 0x6c1379 (national notice created),
0x6c97a0 (prize list built), 0x6c89cf (grant).

## 2. Fallback: runtime grant on a finished round

If the notice path fails, grant from the runtime:

- Detect: the runtime's HTTP layer (`src/runtime_net.c`, route `REGIST_PREFIX "national"`, 590-5,134
  bytes) sees the registration body. Parse `0x4147` (event/stage id), `0x4500` (score) and the
  account. Course and conditions are known from the event the server served for that id. For Open
  Course the per-hole `gameapi_registscore` body has the group `OneHole_<date>_<course>_<hole>`
  and the score.
- Grant: queue the ids and grant them on the game thread with the existing `golf_collab_grant`
  pattern (`runtime_game.c`: item index 0x2c7cc70, save 0x2c6b648 + 0x3723e8 + id*0x44, grant
  0xcd0250). The HTTP send runs on the title-queue thread, so the grant should wait for a game-thread
  hook. The entitlement update 0xc74f90 (called at 0x3580fe and 0xb9f41a) is the only hooked site
  today; how often 0xb9f41a runs is not known.
- Offline rounds with no server request (Challenge, free play without Open Course) have no traced
  round-end hook. The result-screen machine 0xd8xxxx did not run in free play
  (`analysis/online/score-registration-trigger.md` 11.1). This would need a new trace.

## 3. Item ids by event and tier

Names are the JP item-index text (`Item_index_list.US_01.xb`). "Status" checks the model (.rem) and
`_df.gnf` texture in `chara_model.xb` / `cart.xb` of 1.17 and 1.00 (also `_nm`, `_sp`, tint rows).

| Event (2018) | Tier | Items | Status |
|---|---|---|---|
| FF30 Collab Cup, 02-26..03-12, Vortex Valley | Qualifier entry | Cactuar outer 8953; Cactuar caps 8929-8940 (sets 8962 = all 12, 8963 = 07-12) | OK |
| | Final entry | Moogle kigurumi 8958 | OK |
| | Everyone (free, not a prize) | FF30 cart 8956 | OK |
| | PS Store (540 yen) | Chocobo cart 8957 | No model (cart 11) |
| Level-5 cup 1: qualifier 06-05..06-11, final 06-11..06-18 | Qualifier | Toan, Xiao: set 9000 (8982, 8990, 8983, 8994, 8978, 8989, 8977, 8995) | OK |
| | Final | White Knight: set 9001 (8979, 8991) | OK |
| Level-5 cup 2: qualifier from 06-22, final from 06-29 | Qualifier | Evan, Shaati, Layton: set 9002 (8976, 8987, 8980, 8993, 8988, 8992, 8981, 8986) | 8980 Evan's pants: no model or texture |
| | Final | Jibanyan 8984, Komasan 8985: set 9003 | OK |
| | Free store label (floor 18, `EN...29`) | Level-5 cart set 8999 (carts 8996-8998) | No cart models (12-14) |
| Atelier Lydie & Suelle, shop 04-19..05-17 | Shop | 8965, 8966, 8969-8974 (set 8975) | OK |
| Famitsu Collab Cup: qualifier 08-31..09-07, final 09-07..09-14 on Akigase Keikoku | Qualifier | T-shirt & cap set 9128: caps 9108-9119, T-shirts 9124-9127 | 9125-9127 no texture |
| | Final | Ranger set 9129: 9120-9123, 9130-9132 | All 7: no texture (`kigurumi_006.rem` exists) |
| | Everyone | Famitsu cart 9106; kuttsuki Necky 8785 | 9106 no model (cart 15); 8785 OK |
| 1st anniversary, 08-31..09-28 | Shop, 500 coins/part | Already in the shop sheet (floor 16, window 2018-08-31..09-28): BEAMS 8882-8890, FF30 8929-8940, 8953, 8958, Atelier 8965/8966/8969-8974 | OK |

Unusable, confirmed: 8980, 9120-9123, 9125-9127, 9130-9132 (11 parts, the list in the task minus
9124 which is fine), plus carts 8957, 8996-8998, 9106. `kigurumi_006.rem` and `tops_058.rem` are
present, so a texture-pack texture could make the ranger suits and letter shirts wearable, but that
would be new art, not the original.

## 4. Coins

Static data (`xbdata/param/serverFile/info.US_01.xb`):

| Source | Sheet | Values |
|---|---|---|
| Turf War / online rooms | 16 (`Fightingroom_bonus_coin_settings`) | join_stroke 300, join_real 400, join_match 50, join_ctf 10; placement +200/+100/+50 |
| Open Course pickups | 6 (`item_drop`) and 5 (per cart) | pattern 0: 40 coins per coin drop; cart weights 60-150 |
| Hole Bonus side bet | 3, 4, 14 | 30 coins per tee; birdie/eagle/albatross pay 30 (+70 in sheet 14); jackpot 1,000 |
| Lucky Chance | `event_mission_schedule` | Items, not coins |

No offline (Challenge) coin reward was found in the data; `challenge_param` has no coin column. A
typical Turf War round pays 300-600 coins; an Open Course round pays its pickups. The one-round
measurement with the pad replay harness was not done (a full round is over the 10-minute run limit).
Measure it by reading the save's coin count before and after one Open Course 9-hole round.

## 5. Design: Classic and Hardcore

### 5.1 Archived cut lines

Only one was found: the FF30 qualifier pass line was +3 (18 holes, Vortex Valley, big cup, low
character growth); the final used champion tees, strong wind and medium growth (player blog,
2018-03). No cut, rank tables or entrant counts were found for the Level-5 or Famitsu cups.

### 5.2 Modes

| | Classic | Hardcore |
|---|---|---|
| Qualifier entry prize | Finish the qualifier round once (`reg_reward_normal_1/_2` = the qualifier set) | Finish the qualifier at or under par (`score_reward`) |
| Finalist prize | Qualifier score at or under the cut (FF30: +3; others: +3 as the only archive) unlocks the final; finishing the final gives the final prize | Cut at -3 (or 6 strokes under Classic), final on champion tee + strong wind, prize only at or under par in the final |
| Course and rules | FF30: Vortex Valley, big cup (row 1013), final champion tee + strong wind (row 1034, inferred from the table); Level-5: course to pick (not archived); Famitsu final: Akigase Keikoku | Every final on Akigase Keikoku (the anniversary's hardest course) |
| Attempts | Unlimited (`resetThreshold` high) | 3 per stage per day via the join count and `resetThreshold`; extra tries cost S tickets (game's own path) |
| Shop | 500 coins/part: reopen the anniversary rows (drop the 2018 window) and add the Level-5 parts at 500 | 1,500-2,500 (current `collab_items=earn` prices) |
| Unusable prizes | Famitsu ranger final: give the Famitsu cap set and Necky instead, or a texture-pack suit marked as new art | Same |

Score targets are set by the server per event in the catalog; Hardcore just uses tighter numbers.
Rank prizes are not useful for a single player; use `score_reward` for skill targets.

### 5.3 Work and estimates

| Piece | Where | Estimate |
|---|---|---|
| Event catalog with Classic/Hardcore numbers | server (separate worktree; main checkout has another chat's edits) | 0.5 day |
| Two-stage board, per-id archives with reward keys | server | 1 day |
| Own-score results (`0x42d1`, rank, entrants) and cut logic | server | 1 day |
| Result notice with `<nid>`, dates | server | 0.5 day |
| Guest runs: entry to final, notice, prize grant, save/restart check | runtime + server | 2-3 days (main risk: the unknowns in 1.6) |
| Launcher mode switch (Classic/Hardcore/off), no `unlocked` grant while active, shop prices | launcher + `runtime_collab.c` | 1 day |
| Fallback runtime grant (if the notice path fails) | `runtime_net.c`, `runtime_game.c` | 2 days |

Total about 6-7 days via the notice path, or 4-5 days via the runtime fallback.

## Sources

- [PlayStation Blog JP, FF30 collab (2018-02-08)](https://blog.ja.playstation.com/2018/02/08/20180208-newmingol/)
- [Player blog, 4th national tournament (FF30 cup)](https://ameblo.jp/ousyun2010/entry-12359019233.html)
- [PlayStation Blog JP, Level-5 collab (2018-05-24)](https://blog.ja.playstation.com/2018/05/24/20180524-newmingol/)
- [GAME Watch, Level-5 collab](https://game.watch.impress.co.jp/docs/news/1123617.html)
- [GAME Watch, 1st anniversary and Famitsu cup](https://game.watch.impress.co.jp/docs/news/1139022.html)
