# Vitacompanion

Vitacompanion is a user module which makes developing homebrews for the PS Vita device easier. It does two things:
- Open a FTP server on port 1337
- Listen to commands on port 1338

# Build

This repository includes its hardened `libftpvita` fork in `libftpvita/`.
The plugin builds that source directly instead of relying on a globally
installed or separately checked-out library:

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

`FTPVITA_SOURCE_DIR` may select another compatible source directory for
development, but the default checkout is self-contained.
The command server reports
`protocol=3 hardening=8 ftp=LIST,REST_SAFE,BOUNDED_IO,TIMED_IO,SINGLE_FLIGHT,SAFE_REBOOT,RECOVERABLE_CLIENTS,RETR_EXTENT,RETR_ERRORS,STOR_SAFE,CMD_FRAMED`
through the `version`
command so automation can reject an old loaded plugin. `REST_SAFE` means FTP resume
offsets and all other built-in FTP commands use explicit bounded parsers; they
do not enter Vita PDCLib `scanf` from a shell plugin. `BOUNDED_IO` means all
eight admitted clients fit in the plugin pool and failed/partial sends abort a
transfer instead of tying up a shell thread. `TIMED_IO` bounds every accepted
control and data socket so an abandoned host transfer cannot occupy a shell
thread indefinitely.
`SINGLE_FLIGHT` permits only one LIST/RETR/STOR data transfer at a time so
development automation cannot saturate SceShell's reserved CPU3 with parallel
bulk workers. `SAFE_REBOOT` exposes `ftpstatus` and atomically refuses a reboot
while any FTP client or data transfer is live. A refused reboot leaves the Vita
running; automation must wait for `FTP clients=0 transfer=0` and try only once.
`RECOVERABLE_CLIENTS` makes FTP `QUIT` terminal on the server side and exposes
`ftpreset`, which aborts stranded FTP sockets without restarting SceShell or the
console. `ftpstatus` also reports socket-timeout configuration failures.

`RETR_EXTENT` bounds each download to the length of the opened file at the
start of RETR. Appended bytes wait for the next download; this is not an atomic
snapshot of a file being overwritten. A shortened file fails rather than
silently accepting a truncated download. The 64-bit end/restart seeks are
checked, and REST is consumed even on failure. `RETR_ERRORS` includes the
failing read/send stage, raw result, immediate thread-local network errno and
exact read/sent byte counts in a 426 reply. Partial sends retain their exact
progress; zero progress/errors abort without retrying indefinitely. Existing
five-second socket timeouts and single-flight admission are unchanged. These
changes address moving-EOF transfers and missing failure evidence, not a
proven diagnosis of every earlier FTP 426.

`STOR_SAFE` completes partial file writes and reports exact receive/write
progress on failure. Failed uploads retain their partial file for inspection
instead of deleting a pre-existing or resumed destination. `CMD_FRAMED`
accumulates a command through its newline across fragmented TCP receives,
bounds idle command connections, and verifies that the command listener has
successfully bound before reporting the service as available.

The tests under `libftpvita/tests/` exercise the actual bounded transfer
loop with growing files, REST/empty files, 64-bit extents, short reads/sends,
early EOF, errors and zero progress, as well as short file writes and upload
failure accounting. They pass host ASan/UBSan; the VitaSDK plugin build is
also part of release validation.

The hardened command server bounds requests and responses, validates all
socket/thread/listener creation, and has a single owner for Wi-Fi lifecycle
changes. It resamples network state after callback registration, closing the
state-query/registration race.
The screenshot command only asks SceShell to capture; host tooling owns gallery
discovery and never asks this system plugin to rename personal screenshots.

# Install

Run VitaShell on your PS Vita, press SELECT to activate the FTP server and copy `vitacompanion.suprx` to `ur0:/tai`. Finally, add the following line to `ur0:/tai/config.txt`:

```
*main
ur0:tai/vitacompanion.suprx
```

# Usage

## FTP server

You can upload stuff to your vita by running:
```
curl --ftp-method nocwd -T somefile.zip ftp://IP_TO_VITA:1337/ux0:/somedir/
```
Or you can use your regular FTP client.

## Command server

Send a command by opening a TCP connection to the port 1338 of your Vita.

For example, you can reboot your vita by running:
```
echo reboot | nc IP_TO_PSVITA 1338
```

Note that you need to append a newline character to the command that you send. `echo` already adds one, which is why it works here.

### Available commands

| Command   | Arguments     | Explanation                  |
| --------- | ------------- | ---------------------------- |
| `destroy` | none          | kill all running applications |
| `launch`  | `<TITLEID>`   | launch an application by id e.g. `launch VHBB00001` to launch the [Vita Homebrew Browser](https://github.com/devnoname120/vhbb) |
| `reboot`  | none          | reboot the console           |
| `ftpstatus` | none        | show active FTP clients/transfers |
| `ftpreset` | none         | abort stranded FTP clients without rebooting |
| `screen`  | `on` or `off` | turn screen on or off        |
 
 **Note**: Commands are defined in [`src/cmd_definitions.c`](https://github.com/robsdedude/vitacompanion/blob/master/src/cmd_definitions.c), you can add new commands there.
 
 # Integration in IDE's
 
 ## VSCode
 
 https://github.com/imcquee/vitacompanion-VSCODE
 
# Acknowledgements 

Thanks to xerpi for his [vita-ftploader](https://bitbucket.org/xerpi/vita-ftploader/src/87ef1d13a8aa/plugin/?at=master) plugin, I stole a lot of his code (with his permission). Thanks to cpasjuste for [PSP2SHELL](https://github.com/Cpasjuste/PSP2SHELL), it inspired me to create this tool.
