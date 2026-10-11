# Head Wi-Fi MAC / DHCP diagnostic

This change diagnoses the repeated association-without-IP failure. It does not
claim to repair the coprocessor's invalid MAC response or hotspot DHCP service.
No static IP, generated MAC, or extra reconnect loop is introduced.

## Changed files

- `components/wifi_manager/src/wifi_manager.c`: local netif MAC, IP, link-up and
  DHCP state at association and GOT_IP; disconnect reason; one additional remote
  MAC RPC on initial IP timeout (on the initialization task, not the event loop).
- `managed_components/espressif__esp_wifi_remote/esp_wifi_remote_net2.c`: zero
  initialization of the MAC result, reject zero/multicast MAC, check set_mac,
  and reject the connected/IP action when the local interface MAC is invalid.

The managed-component modification is local. Do not update/re-download that
dependency while testing; that can overwrite this patch.

## Verify

Build/flash **head only** using the existing IDF environment and serial port.
No erase-flash/fullclean is required. Both changed files passed ESP GCC syntax
checks using the existing compile database. Full build and on-device checks
are still required.

Capture startup through GOT_IP or the initial 30-second timeout. Key lines:

1. `MAC setup complete` means a valid remote MAC was installed; it is not proof
   that DHCP has succeeded.
2. `MAC setup rejected` means the remote getter returned OK with an invalid MAC.
   The network interface is deliberately not started with that address.
3. `NET diagnostic [associated]`: DHCP 0=INIT, 1=STARTED, 2=STOPPED;
   inspect return codes before interpreting values.
4. `NET diagnostic [initial IP timeout]` includes a second remote MAC read.
   Remote invalid too: investigate ESP-Hosted/RPC/coprocessor. Remote valid but
   local invalid: investigate the start-event/MAC setup sequence. Both valid,
   interface up and DHCP STARTED but IP zero: DHCP/data path remains suspect;
   this snapshot alone does not distinguish hotspot from transport failure.
5. `Got IP` is the network-ready milestone. Only after that can discovery,
   WebSocket server startup, camera delivery and algorithm results be assessed.

Hotspot credentials are never printed by these diagnostics. Controller,
SDK configuration, audio, wake engine and camera algorithms are unchanged.
