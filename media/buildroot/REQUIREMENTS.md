# Buildspot Console/Sync Design Requirements

## 1. Problem Statement

Current approach uses a background receiver on the same UART that is also used as the interactive console.

This creates a structural conflict:

- shell/getty and file transfer both consume the same byte stream
- binary transfer collides with prompt output, echo, banners, and control characters
- the receiver cannot distinguish ordinary console input from sync traffic
- a background service on the console UART interferes with normal operator work

As a result, the UART channel is unreliable for mixed console and sync traffic.

## 2. Goal

Replace the background UART receiver service with an explicit, host-driven sync mode that temporarily turns the console session into a file-transfer session and then returns cleanly back to console mode.

The target user experience is:

1. Open a serial console normally.
2. Work in shell mode as usual.
3. Press a local hotkey on the host.
4. Host switches the session into sync mode.
5. Files are synchronized with progress indication.
6. Session returns back to ordinary console mode.

## 3. Non-Goals

- No permanent background receiver on the console UART.
- No dependence on `minicom` internal patches for MVP.
- No use of `Ctrl+S` or `Ctrl+Q` as protocol triggers.
- No sharing of the same UART between shell and sync mode at the same time.

## 4. Key Constraints

- Device side uses a console UART that may be `ttyFIQ0`, `ttyS1`, or another board-specific tty.
- The same physical UART is needed for both interactive console access and offline file delivery.
- Device userland may be BusyBox-only and may miss utilities like `timeout`, `bash`, `python`, or `flock`.
- Host side can be richer and may use Python or another higher-level runtime.
- Sync source remains `output/out/oem` on the host.
- Sync target remains `/oem` on the device.

## 5. Root Cause of Current Failure

The current service model is flawed because the receiver tries to own the console tty from the background.

Concrete failure modes:

- console bytes are interpreted as transfer bytes
- transfer bytes are interpreted as terminal control/input
- the terminal stack may consume XON/XOFF and other control bytes
- shell output corrupts the data stream
- a daemon cannot safely multiplex with an interactive shell on the same tty without a well-defined mode switch

This is not a bug in one script. It is a protocol ownership problem.

## 6. Proposed Architecture

The new system has two components:

### 6.1 Host Component: `buildspot-console`

A custom host console program that opens the serial port and supports two modes:

- `console mode`: ordinary serial terminal pass-through
- `sync mode`: temporary framed file-transfer session with progress reporting

This tool becomes the primary operator entry point instead of `minicom` for sync-capable sessions.

### 6.2 Device Component: `buildspot-agent`

A foreground command launched from the current shell session when the host requests sync mode.

Important property:

- the agent must use current `stdin/stdout`
- the agent must not open `/dev/tty*` by itself
- the agent must not run in background
- when the agent exits, the shell resumes on the same console session

This removes tty ownership conflicts completely.

## 7. Console Mode Switching

### 7.1 Local Escape Sequence

Mode switching must be initiated by a host-local escape sequence that is intercepted before bytes are sent to the device.

Recommended candidates:

- `Ctrl-A` then `s`
- `Ctrl-]` then `s`
- `~~sync`

Not allowed:

- `Ctrl+S`
- `Ctrl+Q`

Reason: these are commonly bound to XON/XOFF flow control.

### 7.2 Entering Sync Mode

When the operator triggers local sync:

1. Host pauses normal terminal rendering.
2. Host switches local tty handling to raw mode.
3. Host sends a newline and a shell command to start the device agent.
4. Host waits for a clear readiness marker from the device.
5. If marker is received, host starts the sync protocol.
6. If marker is not received, host restores console mode.

### 7.3 Exiting Sync Mode

Sync mode ends when:

- agent sends `DONE`
- agent sends `ERROR`
- host cancels the transfer
- link failure occurs

On exit:

1. Host restores terminal rendering.
2. Host returns to console mode.
3. Device agent exits back into shell.

## 8. Launch Model on Device

The host must launch the agent by sending a normal shell command, for example:

```sh
/oem/usr/bin/buildspot-agent --stdio --root /oem
```

The agent must immediately print a machine-readable banner such as:

```text
BUILDSPOT/1 READY
```

This banner is the handshake marker that tells the host that the shell has been replaced by the sync agent.

## 9. Sync Protocol Requirements

Protocol must be simple, deterministic, and shell-safe after the initial agent start.

### 9.1 Transport

- single serial byte stream over the active console session
- after agent starts, only protocol traffic is allowed until sync ends
- protocol must not rely on terminal escape interpretation

### 9.2 Framing

Preferred MVP approach:

- line-based control records in ASCII
- payload chunks with explicit byte lengths
- SHA256 for content verification

Possible command set:

- `HELLO <version>`
- `GET_INDEX`
- `INDEX_BEGIN <bytes>`
- `INDEX_END`
- `PUT_FILE <path> <mode> <size> <sha256>`
- `DATA <bytes>` followed by raw bytes
- `FILE_OK <path>`
- `FILE_ERR <path> <reason>`
- `DELETE_FILE <path>` optional, not needed for MVP
- `COMMIT_INDEX <bytes>`
- `DONE`
- `ERROR <reason>`

### 9.2.1 MVP Wire Sequence

The implemented MVP sequence is:

1. Host opens serial port in raw mode.
2. Host sends shell command:
   `/oem/usr/bin/buildspot-agent.sh --stdio --root /oem`
3. Device replies:
   `BUILDSPOT/1 READY`
4. Host sends:
   `HELLO 1`
5. Device replies:
   `OK HELLO 1`
6. Host sends:
   `GET_INDEX`
7. Device replies:
   `SEND INDEX`
8. Device sends current index file using `sz`.
9. Host receives index using `rz`.
10. Host computes delta.
11. For each changed file host sends:
	`PUT <mode> <sha256> <size> <relative-path>`
12. Device replies:
	`READY PUT <relative-path>`
13. Host sends file contents using `sz`.
14. Device receives file using `rz`, verifies hash and size, and replies:
	`OK FILE <relative-path>`
15. After all files host sends:
	`PUT_INDEX <sha256> <size>`
16. Device replies:
	`READY INDEX`
17. Host sends new index using `sz`.
18. Device commits the index and replies:
	`OK INDEX_UPDATE`
19. Host sends:
	`DONE`
20. Device replies:
	`BYE`
21. Agent exits and control returns to the shell.

All protocol failures must be reported as `ERR ...` on the control channel.

### 9.3 Index Format

Keep existing conceptual model:

- host computes index from `output/out/oem`
- device stores index under `/oem/.buildspot.index`
- index is text-based and hash-driven

Each entry should contain at least:

- relative path
- sha256
- size
- mode
- mtime optional

### 9.4 Atomicity

Device must write incoming files to temporary names first:

- `/oem/.buildspot-tmp/<path>.part`

Then atomically rename into place after hash verification.

Index update must also be atomic:

- write temp index
- verify
- rename to final `.buildspot.index`

## 10. Host Responsibilities

`buildspot-console` must:

- open serial port in raw mode
- disable local XON/XOFF handling
- support local escape command for sync mode
- maintain ordinary console behavior when not syncing
- compute local OEM index
- compare with device index
- transfer only changed files
- render transfer progress per file and total
- recover to console mode after failure
- save a session log for diagnostics

Recommended host log outputs:

- serial port and speed
- mode transitions
- handshake success/failure
- file count and byte count
- current file progress
- retry/error details

## 11. Device Responsibilities

`buildspot-agent` must:

- operate strictly in foreground over stdio
- avoid dependencies on `bash` and optional userland tools
- validate all incoming paths
- reject absolute paths and `..` traversal
- create missing directories under `/oem`
- verify hashes before commit
- update index only after successful transfers
- emit structured status lines that host can parse

Recommended device log/stderr messages:

- agent started
- root directory in use
- index loaded count
- file receive started
- file verify success/failure
- commit success/failure
- protocol error reason

## 12. Why Not Patch `minicom`

Patching `minicom` is possible but not preferred.

Reasons:

- ongoing maintenance burden
- harder to distribute and reproduce across development hosts
- still requires device-side mode switch design
- custom transfer/progress integration is easier in a dedicated tool

Conclusion:

- keep `minicom` as an optional plain console tool
- build a project-specific `buildspot-console` for sync-aware sessions

## 13. Recommended MVP Implementation

### 13.1 Host Side

Use Python with `pyserial` for MVP because it is fast to implement and easy to iterate.

Suggested command layout:

```text
buildspot-console console --port /dev/ttyUSB0 --baud 115200
buildspot-console sync --port /dev/ttyUSB0 --baud 115200
buildspot-console auto --port /dev/ttyUSB0 --baud 115200
```

Where:

- `console` = plain terminal mode only
- `sync` = one-shot non-interactive sync command
- `auto` = interactive console with local escape into sync mode

### 13.2 Device Side

Use POSIX shell for bootstrap and either:

- a compact shell implementation for MVP, or
- a tiny C helper for more reliable framing and binary IO

Recommendation:

- MVP can start as shell if protocol stays simple
- final stable agent should likely be a small C binary for deterministic binary reads/writes

## 14. Phased Plan

### Phase 1: Safe Ownership Model

- remove permanent background sync service from boot path
- introduce foreground `buildspot-agent --stdio`
- add one-shot host sync command

### Phase 2: Unified Console Tool

- implement host interactive console
- add local hotkey to enter sync mode
- add progress UI and session logging

### Phase 3: Robustness

- resumable transfers optional
- delete propagation optional
- chunk retry optional
- protocol versioning

## 15. Acceptance Criteria

The design is acceptable when all of the following are true:

1. Ordinary console usage works with no background sync daemon attached to the tty.
2. Sync can be started explicitly from the host without rebooting the device.
3. Sync does not corrupt shell prompts or typed commands.
4. Files transferred to `/oem` are hash-verified before commit.
5. Host returns to console mode after sync completes or fails.
6. Operator gets explicit progress and error reporting.
7. No dependence on `Ctrl+S` or tty flow-control sequences.

## 16. Decision Summary

Recommended direction:

- do not keep `buildspot-recv` as a background init service on the console UART
- do not patch `minicom` for MVP
- build a custom host console client
- run a foreground device agent over stdio only when sync is requested

This is the simplest design that matches how a single-UART console system actually behaves.