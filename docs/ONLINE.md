# Online play (psnr)

Titles that matchmake through sceNpMatching2 find each other through a
[psnr](https://github.com/sp00nznet/psnr) server. psnr is a small stand-in for
PSN's rooms and leaderboards. Once players are matched, the title's own
traffic goes directly between them over `sys_net` P2P sockets; the server does
not carry it.

Offline is still the default. Nothing below changes a title unless it is
set.

The runtime's command line takes two flags, anywhere after the program name:

| Flag | What it does |
|---|---|
| `--psnr host[:port]` | Go online with this server: the same as `PS3_NET_ONLINE=1` plus `PSNR_SERVER`. |
| `--username NAME` | The player's name; wins over `PS3_NP_ONLINE_ID`. |

The name follows PSN's online ID rules: 3-16 letters, digits, `-` or `_`,
starting with a letter. Other characters are dropped, and a name that still
doesn't fit is ignored with a message. With no name set, an online game uses
the OS login name. Offline with no name, nothing changes ("PS3Player").

| Variable | What it does |
|---|---|
| `PS3_NET_ONLINE=1` | Real host sockets (`libs/network/sysNet.c`) and cellHttp, and cellNetCtl reports a connection. Unset, empty or `0` is offline: cellHttp then connects nowhere. |
| `PS3_NET_HOSTS=name=addr&&name2=addr2` | The host map (RPCS3's IP/Hosts switch format) that `gethostbyname` and cellHttp resolve through. Names match whole and ignore case, `*` stands for any run of characters, and the first match wins. `addr` is an IPv4 address or a host name, which is then resolved normally. While it is set, a name it doesn't list doesn't resolve; the first refusal of each name is logged. Unset, names resolve through the OS. |
| `PSNR_SERVER=host[:port]` | The psnr server. Port 36100 by default. With `PS3_NET_ONLINE`, the NP manager reports ONLINE. |
| `PS3_NP_ONLINE_ID=name` | The player's name, if `--username` isn't given. The name also gives the instance its own console identity: the OpenPSID (`sys_ss_get_open_psid`) and MAC are derived from it. Titles tell consoles apart by these, so two instances with the same name count as one player. Offline with no name, both stay at their old fixed values, so offline saves are untouched. Online without `PSNR_SERVER`, a name alone signs NP in (ONLINE): there is no matching server, so Matching2 rooms stay unavailable, but a title's own servers (cellHttp) can be reached. The name is read on first use, so a title's options hook may set it. |
| `PS3_OPEN_PSID=<32 hex>` | Set the OpenPSID outright instead. |
| `PS3_NET_P2P_PORT=n` | The host port this instance's P2P sockets bind, whatever port the title asks for. Default 3658. psnr hands this port to peers. Two instances on one machine need two ports. |
| `PS3_NP_SIGNALING_DELAY_MS=n` | How long after a member joins that signaling reports them "established". Default 1000. |
| `PS3_NET_TRACE=1` | Log every packet sent, and every packet received (not the empty polls). |
| `PS3_NET_SIGNIN=host[:port]` | Sign in to this server before using it (see "Sign-in" below). Port 18000 by default. Needs `PS3_NET_KEY_FILE` too. |
| `PS3_NET_KEY_FILE=path` | Where the install's sign-in key is kept. Made at the first sign-in, and never written over: losing it loses the name on that server. |
| `PS3_NET_CLIENT_VERSION=x.y.z` | The build's version, for the server's version gate. Default 0.0.0. |
| `PS3_NET_NETWORK_VERSION=n` | Which builds play together. Default 1. |
| `PS3_NET_LAN=1` | Allow local addresses (this PC, the home network, link-local) for every server and peer. Without it, a local address is used only when a server this run was pointed at (`PS3_NET_HOSTS`, `PSNR_SERVER`, `PS3_NET_SIGNIN`) is on one itself, or when a co-op peer is on the same /24 as this machine; other local addresses are refused, and the first refusal of each is logged. |

`ps3_net_lock_offline()` (`include/ps3emu/ps3_net.h`) keeps a process offline for good, whatever the variables say later: cellNetCtl, NP, the sys_net host sockets and cellHttp all report no network. A port calls it for sessions that must never reach other players. Called before the title starts, the host sockets are never registered.

Peers: datagrams reach the title only from members of its current psnr room, at the addresses psnr gave them; datagrams that psnr relays are accepted only from the psnr server itself. Addresses the title is handed (by its servers, through DNS or by psnr) follow the local-address rule of `PS3_NET_LAN` above; an address in `PS3_NET_HOSTS` is always used as written.

## Sign-in

A server with a sign-in (the Demon's Souls Recomp server: its `docs/identity.md`)
knows a player by a key, not just a name. With `PS3_NET_SIGNIN` and
`PS3_NET_KEY_FILE` set (`libs/network/ps3_net_signin.c`):

- At the first sign-in the runtime makes an Ed25519 key (Monocypher,
  `third_party/monocypher`) and writes it to the key file. The first key to sign
  in with a name owns that name on the server.
- It asks the server for a challenge, signs it, and gets a session token back.
  A key the server has not seen also does a short proof of work (SHA-256).
- cellHttp adds `X-DSS-Session: <token>` to the title's requests to that
  server's address, and to no other. psnr's HELLO carries the token too; when
  psnr says it has ended, the runtime signs in once more.
- The session is renewed before it ends. After a failure there is no new try
  for a minute. A server that does not answer, or has no sign-in, leaves the
  title's requests as they were.
- A refusal's message (a ban, maintenance, an old build) is kept for the host to
  show the player: `ps3_net_signin_notice()` in `include/ps3emu/ps3_net_signin.h`.
  When psnr refuses co-op, the player is told so too, unless the sign-in itself
  failed: then its own message stays, not replaced by a second one.
- The server's banners (`GET /dss/v1/banners`, no session) are fetched every
  5 minutes by a thread of their own, from the host's first call of
  `ps3_net_signin_banner()`, which hands over each new banner once.
- The players met lately, blocks and reports (the server's `docs/social.md`)
  are for the host's own page: `ps3_net_players_refresh()` asks for the list,
  `ps3_net_player_block()` and `ps3_net_player_report()` send one change, and a
  thread of their own makes the calls, with the session, one at a time. The host
  polls `ps3_net_players()` for the list and `ps3_net_players_result()` for the
  line to show after a block or report. A session the server no longer takes
  signs in again, once.

## Two players on one machine

```
psnr                                    # server/ in the psnr repo; listens on :36100
PS3_NET_P2P_PORT=3658 ./build/simpsons <EBOOT.elf> --psnr 127.0.0.1 --username homer
PS3_NET_P2P_PORT=3659 ./build/simpsons <EBOOT.elf> --psnr 127.0.0.1 --username bart
```

Redirect stderr to a file and the runtime logs every wait (see
`runtime/ps3_log.h`). Two instances doing that crawl to about 1 fps once the
stage starts, and the title drops the peer as "not responding". Set
`PS3_VERBOSE=0` for play; at that setting both hold 40-50 fps.

## What is implemented

| Module | State |
|---|---|
| sceNpMatching2 | Contexts, one server and one world (both answered locally), and rooms on psnr: search, create/join, leave, kick, room data, room messages. Room, room-message and signaling callbacks are delivered from `cellSysutilCheckCallback`. Signaling reports a member as established as soon as psnr names them. |
| sceNpScore | Title and transaction contexts. Record score, and ranking by range and by NP ID, sync and async, all on psnr leaderboards. |
| sceNpLookup | Profiles are answered locally from the online ID. There is no avatar. |
| cellSysutilAvc2 | Voice chat loads, joins and streams, but carries no audio. |
| sceNp manager | Reports ONLINE when online with psnr, or online with a player name; fires the manager callback once with that status. |
| cellNetCtl | Connected only online. The net-start dialog reports `CELL_NET_CTL_ERROR_DIALOG_CANCELED` offline, 0 online. |
| Sign-in | The key, the challenge and session calls, the session on cellHttp's requests to that server and in psnr's HELLO (see "Sign-in"). |
| sys_net | P2P sockets map to UDP/TCP on `PS3_NET_P2P_PORT`. Datagram P2P sockets share the port by vport: each datagram carries an 8-byte vport header (`VP01`, destination and source vport), so both peers need a runtime with it. Stream P2P sockets share the port as plain TCP. |

## How it was verified

Simpsons Arcade (NPUB30563) was run as two instances on one machine:
- the host creates a match and the other player finds it with Quick Match;
- both appear in each other's online lobby and pick characters;
- the host starts the game, sends its game setup (58 KB, zlib-compressed by
  the title, over a TCP P2P socket), and both instances play Stage 1
  together; one player's moves show on both screens;
- the room goes through psnr, and game traffic runs peer to peer over UDP
  and TCP P2P sockets.

The setup transfer needs ps3recomp#202: the title's zlib is lifted code, and a
lifter bug left its compressor emitting a 10-byte stream.

Offline, with none of the variables set, Simpsons boots, reports NP offline,
shows its "sign in to post scores" notice, and plays Stage 1.

Tests: `libs/network/tests/test_np_matching2.c` covers response relocation,
attribute packing and the room structures a title reads. `test_sys_net.c`
covers sockets over loopback, including a P2P bind and receive, two datagram P2P
sockets on different vports sharing the port, and two P2P stream sockets
sharing the port with the peer seeing that port on accept.

What it took to get the lobby up, each of which a title will hit:
- **RoomMemberDataInternal layout.** userInfo is at +4 and memberId at +56,
  after the NP ID.
- **Signaling timing.** "Established" is deferred until the host has handled
  the join.
- **Update events for the writer.** The member who writes room internal data
  receives the update event too.
- **Distinct console identity.** Each player needs their own OpenPSID; the
  title hashes it into the key each player announces.
- **P2P sockets.** They bind all interfaces. Stream sockets connect to the
  port in sin_vport, and every stream socket shares the P2P port, so a
  connection comes from the port signaling reported. Accept reports that
  port in sin_vport, the way connect takes it: the title matches the
  connection to a room member by it and drops data from anyone else.
- **libnet errors.** A failing call returns `0x80010200 | errno` in-band.

## Why it is shaped this way

- **One thread.** Callbacks run on the title's polling thread, not on a
  Matching2 worker. psnr replies are pumped from `cellSysutilCheckCallback`,
  so every callback runs where titles already expect sysutil callbacks.
  Score's `PollAsync` pumps too, for titles that poll from a worker.
- **Responses are relocatable images.** An SDK response contains pointers into
  itself, and those only mean something once the title picks the buffer in
  `GetEventData`. Each response is built with its pointer fields recorded, then
  patched as it is copied into the title's buffer.
- **Attributes are opaque to the server.** Matching2 bin and int attributes are
  packed into blobs by the client. psnr stores and forwards bytes, and search
  filtering happens client-side on the rooms psnr lists.
