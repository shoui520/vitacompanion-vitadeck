## FTPVita
FTP Server for the PSVita.

## Compiling
Prerequisites:
* [vitasdk](https://vitasdk.org/)

Once all the prerequisites have been met, you can compile this library by typing:
```
	$ make
```

## Hardened transfer checks

`libftpvita/ftpvita_retr.h` is the bounded read/send loop used by
`ftpvita.c::send_file`. A RETR takes its initial extent from the opened file
with checked 64-bit `sceIoLseek`, and never follows subsequent appends. This
does not freeze bytes that another writer overwrites. Early EOF fails the
transfer. Error replies distinguish read, early EOF and send failures, preserve
the immediate libnet error, and report exact partial-transfer byte counts.
The SDK `kernel/iofilemgr.h`, libnet Reference (`sceNetSend`, `sce_net_errno`,
`SCE_NET_SO_SNDTIMEO`) and `api_teleport/teleport_client_test.cpp::sendAndRecv`
define the I/O and timeout contracts. Existing timeout durations are unchanged.

Host regression tests (use a disposable build directory):

```sh
retr_test_dir=$(mktemp -d /tmp/ftpvita-retr-tests.XXXXXX)
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  tests/test_ftpvita_retr.c -o "$retr_test_dir/retr"
"$retr_test_dir/retr"
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  tests/test_ftpvita_parse.c -o "$retr_test_dir/parse"
"$retr_test_dir/parse"
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  tests/test_ftpvita_store.c -o "$retr_test_dir/store"
"$retr_test_dir/store"
```

`libftpvita/ftpvita_store.h` similarly makes STOR/APPE complete short file
writes and fail on zero progress or I/O errors. Failed transfers report the
receive/write stage and exact byte counts without deleting the partial or
pre-existing destination.

## Credits
Thanks to yifanlu for Rejuvenate and UVLoader :D
Thanks to 173210 and everybody who contributed to psp2sdk.
Thanks to Cirne and everybody who contributed to vita-toolchain.
Also thanks to everybody who has helped me on #vitadev :P
