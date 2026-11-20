# devmem TCP TX verification pass: one line per case, 7.2.15-devmem (backport series)

Output of `tests/devmem/scripts/tx-verify/vsum.py` over the evidence files
of the pass that `test-tx-verification.md` reports. `rx` and `tx` are the
bytes the receiver got and the sender had accepted; `DATA-OK` means the
receiver's SHA-256 equals that of the same number of bytes the sender
recorded, with no run of zeroes.

```text
F-A1-bind-tso-on                                           rx=67108864    tx=67108864    DATA-OK COMPLETE           unreadable_skbs=1107    readable=3   maxfrags=17  lowbit=1:17494 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
F-A2-tso-off-unaligned                                     rx=63998912    tx=63998912    DATA-OK COMPLETE           unreadable_skbs=44225   readable=3   maxfrags=2   lowbit=1:59829 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
F-B1-480x128B-gaps-tso-on                                  rx=36864000    tx=36864000    DATA-OK COMPLETE           unreadable_skbs=17830   readable=4   maxfrags=17  lowbit=1:297036 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
F-B3b-exactly-15-frags                                     rx=288000      tx=288000      DATA-OK COMPLETE           unreadable_skbs=300     readable=4   maxfrags=15  lowbit=1:4500 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
F-D2-interleave-devmem-copy-userzc                         rx=140597600   tx=140597600   DATA-OK COMPLETE           unreadable_skbs=3608    readable=1606 maxfrags=17  lowbit=1:47538 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
F-B6-16-frags-one-over                                     rx=0           tx=1024        DATA-OK PARTIAL(prefix)    unreadable_skbs=7       readable=4   maxfrags=16  lowbit=1:112 drop=+7 txdrop=+0 retrans=+6 dmaOther=0 splat=0/0
F-B8-1024x16B-tiny-iovecs                                  rx=0           tx=544         DATA-OK PARTIAL(prefix)    unreadable_skbs=8       readable=3   maxfrags=17  lowbit=1:136 drop=+9 txdrop=+0 retrans=+7 dmaOther=0 splat=0/0
F-C-before-connect-tx-off                                  rx=0           tx=78192       DATA-OK PARTIAL(prefix)    unreadable_skbs=0       readable=3   maxfrags=-   lowbit=- drop=+0 txdrop=+9 retrans=+6 dmaOther=0 splat=0/0
F-C-mid-stream-tx-off                                      rx=31300939    tx=31466102    DATA-OK PARTIAL(prefix)    unreadable_skbs=633     readable=3   maxfrags=17  lowbit=1:8274 drop=+0 txdrop=+10 retrans=+7 dmaOther=0 splat=0/0
F-C-before-connect-sg-off                                  rx=0           tx=78192       DATA-OK PARTIAL(prefix)    unreadable_skbs=0       readable=3   maxfrags=-   lowbit=- drop=+0 txdrop=+9 retrans=+6 dmaOther=0 splat=0/0
F-C-mid-stream-sg-off                                      rx=26800804    tx=26965967    DATA-OK PARTIAL(prefix)    unreadable_skbs=543     readable=3   maxfrags=17  lowbit=1:7085 drop=+0 txdrop=+10 retrans=+7 dmaOther=0 splat=0/0
F-C-before-connect-tx-off-sg-off                           rx=0           tx=78192       DATA-OK PARTIAL(prefix)    unreadable_skbs=10      readable=3   maxfrags=0   lowbit=- drop=+10 txdrop=+7 retrans=+6 dmaOther=0 splat=0/0
F-C-mid-stream-tx-off-sg-off                               rx=29100873    tx=29266036    DATA-OK PARTIAL(prefix)    unreadable_skbs=704     readable=3   maxfrags=17  lowbit=1:7695 drop=+115 txdrop=+7 retrans=+7 dmaOther=0 splat=0/0
F-D10c-mtu9000-120x512B-iovecs                             rx=0           tx=38400       DATA-OK PARTIAL(prefix)    unreadable_skbs=12      readable=3   maxfrags=17  lowbit=1:204 drop=+12 txdrop=+0 retrans=+7 dmaOther=0 splat=0/0
F-D8-sigkill-sender-mid-stream                             rx=635001905   tx=-           DATA-OK PARTIAL(prefix)    unreadable_skbs=10273   readable=3   maxfrags=17  lowbit=1:165294 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
F-D4-zero-window-stall-resume                              rx=60000180    tx=60000180    DATA-OK COMPLETE           unreadable_skbs=2648    readable=13  maxfrags=11  lowbit=1:17329 drop=+0 txdrop=+0 retrans=+2 dmaOther=0 splat=0/0
F-positive-last                                            rx=33554432    tx=33554432    DATA-OK COMPLETE           unreadable_skbs=564     readable=3   maxfrags=17  lowbit=1:8758 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
E1-destination-routed-via-enp0s5                           rx=-           tx=-           -                          unreadable_skbs=0       readable=0   maxfrags=-   lowbit=- drop=+0 txdrop=+0 retrans=+0 dmaOther=- splat=
E2-route-moved-to-enp0s5-mid-stream                        rx=29000870    tx=29100873    DATA-OK PARTIAL(prefix)    unreadable_skbs=588     readable=2   maxfrags=17  lowbit=1:7669 drop=+0 txdrop=+0 retrans=+2 dmaOther=0 splat=0/0
E3-positive-after                                          rx=33554432    tx=33554432    DATA-OK COMPLETE           unreadable_skbs=585     readable=3   maxfrags=17  lowbit=1:8779 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
```
