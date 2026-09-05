# PS5 LocalSend

PS5 LocalSend is a local-network file receiver for jailbroken PlayStation 5
systems. The ELF starts a web server on the console. Open its address from a
phone or computer to send files directly to a configured PS5 storage folder.

No companion application or cloud service is required. Transfers remain on
the local network.

## Features

- Browser interface for phones and computers
- Direct streaming to PS5 storage
- Optional six-digit PIN authorization
- English and Russian interface
- Configurable destination dropdown with up to eight folders
- Internal, USB, M.2, and other mounted storage paths
- Upload progress, current speed, cancellation, and integrity verification
- Automatic conflict-safe file naming

## Installation

1. Copy `ps5localsend.elf` to the PS5.
2. Run the ELF through your payload loader.
3. Read the server address from the PS5 notification.
4. Open that address in a browser on a device connected to the same network.
5. Confirm the PIN if PIN authorization is enabled, choose a destination, and
   select or drop files.

The receiver creates `/data/ps5localsend/config.ini` automatically on first
start.

## Configuration

Edit `/data/ps5localsend/config.ini` while the receiver is stopped, then start
the ELF again.

```ini
port=53317
auth_mode=pin
language=en
pin_ttl_seconds=120
session_ttl_seconds=900
destination=/data/ps5localsend/inbox
storage_path=internal|Internal storage|/data/ps5localsend/inbox
storage_path=usb|USB drive|/mnt/usb0/ps5localsend/inbox
max_file_bytes=21474836480
max_files_per_session=100
```

### Interface language

```ini
language=en
```

or:

```ini
language=ru
```

English is the default. Language selection is available only in the
configuration file.

### Authorization

PIN authorization is enabled by default:

```ini
auth_mode=pin
```

It can be disabled on a trusted local network:

```ini
auth_mode=none
```

With authorization disabled, every device that can reach the server can send
files.

### Storage destinations

Add up to eight entries using this format:

```ini
storage_path=unique_id|Browser label|Absolute path
```

Example:

```ini
storage_path=internal|Internal storage|/data/ps5localsend/inbox
storage_path=m2|M.2 SSD|/mnt/ext0/game-images
storage_path=usb_games|USB games|/mnt/usb0/Games
```

`destination` contains the currently selected path. The browser dropdown
updates it automatically. For paths below `/mnt`, the mount point must already
exist before it can be selected.

Save `config.ini` as UTF-8. UTF-8 BOM and CRLF line endings are supported.

## Building

The project uses `ps5-payload-dev/sdk` v0.43 and builds libmicrohttpd from the
vendored source archive.

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make ps5
```

Create the release archive:

```sh
make release
```

Run host-side tests:

```sh
make test
```

## Network security

The receiver uses unencrypted HTTP and is intended only for a trusted local
network. Do not expose its ports to the internet or use it on public Wi-Fi.

## License

See [LICENSE](LICENSE) and [third-party notices](LICENSES/THIRD_PARTY.md).
