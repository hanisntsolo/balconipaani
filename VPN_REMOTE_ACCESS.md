# Remote Access via WireGuard VPN — Setup Reference

Covers how home LAN devices (starting with BalconiPaani) are made accessible from
anywhere via a WireGuard VPN hub on Oracle Cloud, using the home Linux mini-PC as
a gateway peer and Pi-hole for DNS resolution.

---

## Architecture

```
[Phone / Laptop]  ── WireGuard (AllowedIPs = 0.0.0.0/0) ──►  [Oracle Cloud VM]
                                                                  wg server  10.8.0.1
                                                                  Pi-hole    10.0.0.204
                                                                       │
                                                               WireGuard tunnel
                                                               (PersistentKeepalive)
                                                                       │
                                                            [Home Linux mini-PC]
                                                             VPN IP  : 10.8.0.2
                                                             LAN iface: eno1
                                                             ip_forward=1 + NAT
                                                                       │
                                                              Home LAN 192.168.29.0/24
                                                                       │
                                                           [ESP8266 BalconiPaani]
                                                            192.168.29.36 (static)
                                                            MAC: ec:fa:bc:ca:44:81
```

**Traffic flow (remote client → device):**

1. Client sends packet to `192.168.29.36` → enters WireGuard tunnel (full-tunnel config)
2. Oracle receives it, looks up routing → home mini-PC peer has `AllowedIPs` including `192.168.29.0/24`
3. Oracle forwards the packet to home mini-PC via WireGuard
4. Mini-PC FORWARD rules pass it out through `eno1`, NAT-MASQUERADE applied
5. Device responds; return path is the reverse

---

## Infrastructure Inventory

| Role | Host | WireGuard IP | LAN IP | Notes |
|---|---|---|---|---|
| VPN hub + DNS | Oracle Cloud VM | 10.8.0.1 | — | Pi-hole on 10.0.0.204 |
| Home LAN gateway | Home Linux mini-PC | 10.8.0.2 | 192.168.29.x | Always-on; `eno1` is LAN iface |
| Road-warrior clients | Phone / Laptop | 10.8.0.x | — | `AllowedIPs = 0.0.0.0/0` (full tunnel) |
| BalconiPaani ESP8266 | Home LAN | — | 192.168.29.36 | MAC `ec:fa:bc:ca:44:81`; DHCP reserved |

Home LAN subnet: `192.168.29.0/24`

---

## Configuration

### 1 — Home mini-PC `/etc/wireguard/wg0.conf` (already complete)

The config already contains all rules needed for home LAN gateway behaviour.
Key PostUp sections:

```ini
# VPN → Home LAN forwarding
PostUp = iptables -A FORWARD -i wg0 -o eno1 -j ACCEPT
PostUp = iptables -A FORWARD -i eno1 -o wg0 -j ACCEPT

# NAT VPN clients into home LAN
PostUp = iptables -t nat -A POSTROUTING -s 10.8.0.0/24 -d 192.168.29.0/24 -o eno1 -j MASQUERADE
```

Verify kernel IP forwarding is enabled and persisted:

```bash
sysctl net.ipv4.ip_forward          # must return net.ipv4.ip_forward = 1
# if 0, add this line to /etc/sysctl.conf then apply:
echo "net.ipv4.ip_forward = 1" | sudo tee -a /etc/sysctl.conf
sudo sysctl -p
```

Ensure the service starts on boot:

```bash
sudo systemctl enable wg-quick@wg0
```

Get the mini-PC public key (needed for Oracle peer entry):

```bash
sudo wg show wg0 public-key
```

### 2 — Oracle WireGuard server: home mini-PC peer entry

In Oracle's `/etc/wireguard/wg0.conf`, the `[Peer]` block for the home mini-PC
**must** include the home subnet in `AllowedIPs`:

```ini
[Peer]
# Home Linux mini-PC
PublicKey = <home-minipc-public-key>
AllowedIPs = 10.8.0.2/32, 192.168.29.0/24
```

> The `192.168.29.0/24` entry is what tells Oracle to route home LAN traffic
> through the mini-PC peer rather than dropping it.

Apply live without dropping existing tunnels:

```bash
sudo wg set wg0 peer <home-minipc-pubkey> allowed-ips 10.8.0.2/32,192.168.29.0/24
# or reload the full config:
sudo systemctl reload wg-quick@wg0
```

Verify it took effect:

```bash
sudo wg show wg0
# → home mini-PC peer should list 192.168.29.0/24 under "allowed ips"
```

### 3 — Home router: DHCP reservation for each device

Assign fixed IPs by MAC so device addresses never drift.

| Device | MAC | Reserved IP |
|---|---|---|
| BalconiPaani ESP8266 | `ec:fa:bc:ca:44:81` | `192.168.29.36` |

Configure in router admin UI (usually `http://192.168.29.1`).

### 4 — Pi-hole: custom DNS records

Pi-hole admin → **Local DNS** → **DNS Records**:

| Domain | IP |
|---|---|
| `balconipaani` | `192.168.29.36` |

> Do not use `.local` here — mDNS (`.local`) is link-local only and does not
> cross subnet boundaries. The plain `balconipaani` hostname resolved by Pi-hole
> works over VPN. The `.local` name continues to work when physically on the home WiFi.

VPN clients already use Pi-hole as their DNS server (`DNS = 10.0.0.204` in their
`[Interface]` block), so no client-side change is needed.

---

## Verification Checklist

Run these from a phone or laptop **with the VPN enabled**:

```bash
# 1. Confirm Oracle sees the home subnet route
ssh oracle-vm "sudo wg show wg0"
# → home mini-PC peer should list 192.168.29.0/24

# 2. Raw IP reachability — bypasses DNS entirely
ping 192.168.29.36

# 3. BalconiPaani HTTP via raw IP
curl http://192.168.29.36/api/status

# 4. DNS resolution via Pi-hole
curl http://balconipaani/api/status

# 5. Full browser UI test
# Open:  http://balconipaani
```

---

## Adding a New Home LAN Device

To expose any additional device on `192.168.29.0/24` over VPN:

**Step 1** — DHCP reservation (router admin): MAC → fixed IP

**Step 2** — Pi-hole DNS record: domain → IP
(Pi-hole admin → Local DNS → DNS Records)

**Nothing else changes:**
- Home mini-PC `wg0.conf` already advertises the full `192.168.29.0/24` subnet
- Oracle peer entry already covers the whole subnet
- Phone/laptop configs already full-tunnel (`0.0.0.0/0`)

### Quick-add table (extend as you go)

| Device | MAC | Reserved IP | Pi-hole hostname |
|---|---|---|---|
| BalconiPaani ESP8266 | `ec:fa:bc:ca:44:81` | `192.168.29.36` | `balconipaani` |
| _(next device)_ | | | |

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `ping 192.168.29.x` times out from VPN | Oracle peer missing `192.168.29.0/24` in `AllowedIPs` | Section 2 above |
| Ping works but hostname fails | Pi-hole record missing or wrong IP | Section 4 above |
| Everything fails when VPN is on | `ip_forward` not enabled on mini-PC | `sysctl net.ipv4.ip_forward` must be `1` |
| Works from one client, not another | That client's `AllowedIPs` excludes `192.168.29.0/24` | Add subnet to that peer's client config |
| Access lost after mini-PC reboot | `wg-quick@wg0` not enabled for autostart | `systemctl enable wg-quick@wg0` |
| Access lost after Oracle reboot | Oracle WireGuard service not enabled | `systemctl enable wg-quick@wg0` on Oracle VM |

---

## Security Notes

- Traffic from your device to the home mini-PC travels over the home LAN unencrypted
  (HTTP). This is acceptable for a local-only IoT device on a private subnet.
- The WireGuard tunnel (client → Oracle → mini-PC) is fully encrypted end-to-end.
- The BalconiPaani OTA firmware update page (`/update`) is separately password-protected.
- Do **not** expose port 80 of the ESP8266 directly to the public internet.
- Oracle OCI Security Lists must allow UDP 51820 inbound for WireGuard to function.
