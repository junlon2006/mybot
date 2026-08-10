# AOSL patches

Patches in this directory are local changes kept on top of the upstream AOSL git
submodule at `third_party/aosl`. The submodule itself is never modified in place;
apply these patches explicitly when a build needs them.

## esp32-ipv6-tclass.patch

Sets the IPv6 traffic class (`IPV6_TCLASS`) when `LWIP_IPV6` is enabled instead of
always failing `aosl_hal_sk_set_dscp()` for `AOSL_AF_INET6` sockets on ESP-IDF.
This was a local fix in the previously vendored tree; consider upstreaming it to
https://github.com/AgoraIO-Community/aosl.

Apply from the AOSL checkout root:

```bash
git -C third_party/aosl apply ../patches/aosl/esp32-ipv6-tclass.patch
```
