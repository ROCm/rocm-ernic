# devmem TCP TX verification pass: one line per case, 7.4.0-rc3-devmem-kasan (mainline series)

Output of `tests/devmem/scripts/tx-verify/vsum.py` over the evidence files
of the pass that `test-tx-verification.md` reports. `rx` and `tx` are the
bytes the receiver got and the sender had accepted; `DATA-OK` means the
receiver's SHA-256 equals that of the same number of bytes the sender
recorded, with no run of zeroes.

```text
A1-bind-tso-on                                             rx=67108864    tx=67108864    DATA-OK COMPLETE           unreadable_skbs=0       readable=0   maxfrags=-   lowbit=- drop=+0 txdrop=+0 retrans=+0 dmaOther=- splat=0/0
A2-tso-off-unaligned                                       rx=63998912    tx=63998912    DATA-OK COMPLETE           unreadable_skbs=44225   readable=3   maxfrags=2   lowbit=1:59832 drop=+0 txdrop=+0 retrans=+0 dmaOther=- splat=3/0
A3-tso-gso-off-unaligned                                   rx=63998912    tx=63998912    DATA-OK COMPLETE           unreadable_skbs=44233   readable=4   maxfrags=2   lowbit=1:59822 drop=+0 txdrop=+0 retrans=+0 dmaOther=- splat=0/0
B1-480x128B-gaps-tso-on                                    rx=36864000    tx=36864000    DATA-OK COMPLETE           unreadable_skbs=17996   readable=4   maxfrags=17  lowbit=1:296940 drop=+0 txdrop=+0 retrans=+1 dmaOther=- splat=0/0
B2-480x128B-gaps-tso-off                                   rx=36864000    tx=36864000    DATA-OK COMPLETE           unreadable_skbs=35399   readable=4   maxfrags=13  lowbit=1:314334 drop=+0 txdrop=+0 retrans=+2 dmaOther=- splat=0/0
B3-exactly-15-frags-nongso                                 rx=-           tx=-           NO RECEIVER RESULT         unreadable_skbs=32      readable=3   maxfrags=16  lowbit=1:492 drop=+12 txdrop=+0 retrans=+10 dmaOther=- splat=0/0
B4-1024x1400B-iovecs                                       rx=57344000    tx=57344000    DATA-OK COMPLETE           unreadable_skbs=3288    readable=4   maxfrags=17  lowbit=1:55001 drop=+0 txdrop=+0 retrans=+0 dmaOther=- splat=0/0
B5-odd-1byte-and-page-straddle                             rx=819800      tx=819800      DATA-OK COMPLETE           unreadable_skbs=600     readable=4   maxfrags=2   lowbit=1:800 drop=+0 txdrop=+0 retrans=+0 dmaOther=- splat=0/0
B3b-exactly-15-frags-no-straddle                           rx=288000      tx=288000      DATA-OK COMPLETE           unreadable_skbs=300     readable=4   maxfrags=15  lowbit=1:4500 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
B6-16-frags-one-over                                       rx=0           tx=1024        DATA-OK PARTIAL(prefix)    unreadable_skbs=7       readable=4   maxfrags=16  lowbit=1:112 drop=+7 txdrop=+0 retrans=+6 dmaOther=0 splat=0/0
B7-15-iovecs-one-straddles-a-page                          rx=960         tx=960         DATA-OK COMPLETE           unreadable_skbs=1       readable=4   maxfrags=15  lowbit=1:15 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
B8-1024x16B-tiny-iovecs                                    rx=0           tx=544         DATA-OK PARTIAL(prefix)    unreadable_skbs=8       readable=3   maxfrags=17  lowbit=1:136 drop=+9 txdrop=+0 retrans=+7 dmaOther=0 splat=0/0
B9-positive-straight-after                                 rx=33554432    tx=33554432    DATA-OK COMPLETE           unreadable_skbs=574     readable=3   maxfrags=17  lowbit=1:8767 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
D2-interleave-devmem-copy-userzc-one-socket                rx=140597600   tx=140597600   DATA-OK COMPLETE           unreadable_skbs=3874    readable=1776 maxfrags=17  lowbit=1:47929 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
D2b-interleave-nodelay                                     rx=2534000     tx=2534000     DATA-OK COMPLETE           unreadable_skbs=923     readable=997 maxfrags=3   lowbit=1:1600 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
D3a-iovec-crosses-end-of-dmabuf                            rx=8192        tx=8192        DATA-OK COMPLETE           unreadable_skbs=2       readable=4   maxfrags=2   lowbit=1:3 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
D3b-iovec-starts-at-end                                    rx=8192        tx=8192        DATA-OK COMPLETE           unreadable_skbs=2       readable=4   maxfrags=2   lowbit=1:3 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
D3c-iovec-far-beyond                                       rx=8192        tx=8192        DATA-OK COMPLETE           unreadable_skbs=2       readable=4   maxfrags=2   lowbit=1:3 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
D3d-second-iovec-out-of-range                              rx=8192        tx=8192        DATA-OK COMPLETE           unreadable_skbs=2       readable=4   maxfrags=2   lowbit=1:3 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
D3e-wraparound-many-passes                                 rx=786431700   tx=786431700   DATA-OK COMPLETE           unreadable_skbs=13260   readable=3   maxfrags=17  lowbit=1:204663 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
D4-zero-window-stall-resume                                rx=60000180    tx=60000180    DATA-OK COMPLETE           unreadable_skbs=2106    readable=13  maxfrags=12  lowbit=1:16789 drop=+0 txdrop=+0 retrans=+1 dmaOther=0 splat=0/0
D5-receiver-ring-64-retransmits                            rx=150000450   tx=150000450   DATA-OK COMPLETE           unreadable_skbs=6522    readable=3   maxfrags=17  lowbit=1:43301 drop=+0 txdrop=+0 retrans=+411 dmaOther=0 splat=0/0
D5b-receiver-ring-64-retransmits-small-iovecs              rx=18432000    tx=18432000    DATA-OK COMPLETE           unreadable_skbs=8996    readable=4   maxfrags=17  lowbit=1:148465 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
D6-receiver-link-flap-rto-retransmits                      rx=500530020   tx=503327309   DATA-OK PARTIAL(prefix)    unreadable_skbs=8058    readable=2   maxfrags=17  lowbit=1:130603 drop=+0 txdrop=+9 retrans=+4 dmaOther=0 splat=0/0
D6-rerun1-receiver-link-flap-capture-on-enp0s5             rx=471137525   tx=474066582   DATA-OK PARTIAL(prefix)    unreadable_skbs=7577    readable=2   maxfrags=17  lowbit=1:122722 drop=+0 txdrop=+3 retrans=+4 dmaOther=0 splat=0/0
D6-rerun2-receiver-link-flap-with-blackhole-fallback-route rx=1355004065  tx=1355004065  DATA-OK COMPLETE           unreadable_skbs=21750   readable=3   maxfrags=17  lowbit=1:352714 drop=+0 txdrop=+5 retrans=+363 dmaOther=0 splat=0/0
D7-receiver-rst-mid-transfer                               rx=30033394    tx=37000111    DATA-OK PARTIAL(prefix)    unreadable_skbs=543     readable=2   maxfrags=17  lowbit=1:8076 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
D7b-receiver-fin-mid-transfer                              rx=30000090    tx=37651711    DATA-OK PARTIAL(prefix)    unreadable_skbs=543     readable=2   maxfrags=17  lowbit=1:8015 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
D8-sigkill-sender-mid-stream                               rx=656979368   tx=-           DATA-OK PARTIAL(prefix)    unreadable_skbs=10585   readable=3   maxfrags=17  lowbit=1:170988 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
D8b-sigkill-sender-while-receiver-stalled                  rx=12390996    tx=-           DATA-OK PARTIAL(prefix)    unreadable_skbs=594     readable=9   maxfrags=11  lowbit=1:3624 drop=+0 txdrop=+0 retrans=+2 dmaOther=0 splat=0/0
D9-packet-tap-on-sender-netdev                             rx=63998912    tx=63998912    DATA-OK COMPLETE           unreadable_skbs=1050    readable=3   maxfrags=17  lowbit=1:16669 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
D10a-mtu9000-1MiB-sends                                    rx=63998912    tx=63998912    DATA-OK COMPLETE           unreadable_skbs=1083    readable=3   maxfrags=17  lowbit=1:16702 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
D10b-mtu9000-480x128B-iovecs                               rx=0           tx=10624       DATA-OK PARTIAL(prefix)    unreadable_skbs=12      readable=3   maxfrags=17  lowbit=1:204 drop=+12 txdrop=+0 retrans=+7 dmaOther=0 splat=0/0
D10c-mtu9000-120x512B-iovecs                               rx=0           tx=38400       DATA-OK PARTIAL(prefix)    unreadable_skbs=12      readable=3   maxfrags=17  lowbit=1:204 drop=+12 txdrop=+0 retrans=+7 dmaOther=0 splat=0/0
D10d-mtu9000-60x1024B-iovecs                               rx=6144000     tx=6144000     DATA-OK COMPLETE           unreadable_skbs=489     readable=3   maxfrags=17  lowbit=1:7576 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
D10e-mtu9000-4096B-unaligned-iovecs                        rx=24576000    tx=24576000    DATA-OK COMPLETE           unreadable_skbs=767     readable=3   maxfrags=17  lowbit=1:12023 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
E1-destination-routed-via-enp0s5                           rx=-           tx=-           -                          unreadable_skbs=0       readable=0   maxfrags=-   lowbit=- drop=+0 txdrop=+0 retrans=+0 dmaOther=- splat=
E2-route-moved-to-enp0s5-mid-stream                        rx=27300819    tx=27400822    DATA-OK PARTIAL(prefix)    unreadable_skbs=558     readable=2   maxfrags=17  lowbit=1:7223 drop=+0 txdrop=+0 retrans=+1 dmaOther=0 splat=0/0
E3-positive-after                                          rx=33554432    tx=33554432    DATA-OK COMPLETE           unreadable_skbs=605     readable=3   maxfrags=17  lowbit=1:8797 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
G-a-rmmod-with-orphan-holding-unacked-devmem-skbs          rx=-           tx=-           NO RECEIVER RESULT         unreadable_skbs=0       readable=0   maxfrags=-   lowbit=- drop=-28 txdrop=+0 retrans=+2 dmaOther=1 splat=5/0
G-a-positive-after-reload                                  rx=33554432    tx=33554432    DATA-OK COMPLETE           unreadable_skbs=581     readable=3   maxfrags=17  lowbit=1:8775 drop=+0 txdrop=+0 retrans=+0 dmaOther=0 splat=0/0
```
