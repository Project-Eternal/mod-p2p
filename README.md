# mod-p2p

An AzerothCore module (3.3.5a) that lets the servers of different players meet. Each player's server (in [WoW Eternal](https://github.com/Project-Eternal/Client): EternalCore, the
WebAssembly build embedded in the page) connects to a relay node, a general purpose MQTT broker, finds the servers of the players it should see, opens encrypted links to
them (WebRTC data channels in the browser, the node's inbox topics when a direct link cannot be made) and keeps their worlds in agreement. Nobody hosts a game server.

What is shared: the other players (as mirrors: characters that move, fight and cast as on their own server, never saved), chat and channels, friends, parties with shared
experience and loot, duels, quests, mail, trade, instances, creatures (each decided by one server at a time), and the changes game masters make to the world of a realm.

## Parts

- `src/core/` — portable code, only OpenSSL 3: identity and certificates, signed envelopes, private rooms, encrypted links, the node (rooms, peers, clock), the
  peer host interface, the shared-memory ring buffers, the authority table. Tested natively: `test/` (`cmake -S test -B build && cmake --build build && ctest --test-dir build`).
- `src/` — the glue with the server: `P2PService` (thread, queues), `RingHost` (the ring buffers to the page's JavaScript, `web/p2p.js` of WoW Eternal), `P2PStore`
  (the module's tables), and one file per shared feature.
- `conf/mod_p2p.conf.dist` — `P2P.Enable`.

## What the core must provide

The module needs seams in the server core, all inert when the module is not there; [EternalCore](https://github.com/Project-Eternal/EternalCore) has them. Among them: `Creature::SetPuppet` (a creature decided elsewhere), `Player::IsMirror`,
`GameTime::SetClockCorrection`, the options `Social.AccountWide`, `Characters.NameCheckIgnoreAccount` and `World.CustomIdStart`, and hooks for chat, channels,
spells, auras and packets. The world changes of a realm are read from the database layer on SQLite of WoW Eternal.

## Configuration of the server

`P2P.MirrorAccountId` (default 2147483000) and `Characters.NameCheckIgnoreAccount` must be the same number: the account under which the characters of other players are
kept. `Social.AccountWide = 1` makes the friends and ignore lists belong to the account. `P2P.ShareWorldChanges.Realms` lists the realms whose world changes are shared
(default `2`). Players meet only players of the same realm (`RealmID`).
