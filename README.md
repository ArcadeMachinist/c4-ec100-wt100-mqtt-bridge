# Control4 EC-100/WT-100 SmartEnergy Thermostat → Home Assistant MQTT Bridge

Bridges the Control4 EC-100 director's local TCP API to Home Assistant via MQTT.
Exposes a WT100 Zigbee thermostat (device 84) as a native HA `climate` entity with full
two-way control: HVAC mode, fan mode, hold (preset), heat/cool setpoints, current
temperature, and battery level.

Check out PoC video here: https://www.youtube.com/watch?v=OaH2poMRqtE

Control4 EC-100 is an "energy controller" with a companion WT-100 thermostat.

Originally it was supposed to connect to your electric company via HTTP, and to your smart meter via Zigbee SEC. At the same time it uses propietary Zigbee HA profile to talk with the companion thermostat.

EC-100 is based on Ti DM365 DaVinci Digital Media Processor, has 1Gb NAND storage, 256Mb RAM and runs Linux 2.6.32-rc2.44

This is EXPERIMENTAL solution.
It is not "production" ready, there are still things to figure out/clarify.
I do not include EC-100 root password here, it is unique for each individual device, but can be guessed, based on MAC and Zigbee SEC "Install Code"

## Build

Requires [Zig](https://ziglang.org/) 0.12+ for cross-compilation to the EC-100's ARMv5TEJL CPU.

```sh
zig cc -target arm-linux-musleabi -mcpu=arm926ej_s -O2 -static \
       c4_mqtt_bridge.c arm5_atomics.S -o c4_mqtt_bridge
```

`arm5_atomics.S` provides `__sync_*` atomic shims required because ARMv5TEJ has no
hardware atomic instructions and musl's CAS intrinsics don't compile for it cleanly.

## Deploy (temporary — lost on reboot)

```sh
# Copy to EC-100 (old SSH server — needs legacy options)
scp -O \
    -o KexAlgorithms=+diffie-hellman-group1-sha1 \
    -o HostKeyAlgorithms=+ssh-rsa \
    c4_mqtt_bridge root@<EC100_IP>:/tmp/c4_mqtt_bridge

# Run (survives SSH logout, but not reboot)
ssh root@<EC100_IP> \
    "chmod +x /tmp/c4_mqtt_bridge; /tmp/c4_mqtt_bridge > /tmp/bridge.log 2>&1 &"

# Watch logs
ssh root@<EC100_IP> "tail -f /tmp/bridge.log"
```

Optional: override MQTT host/port at runtime:
```sh
/tmp/c4_mqtt_bridge <MQTT_BROKER_IP> 1883
```

## Persistent Install (survives reboots)

The EC-100 runs BusyBox SysV init. Startup scripts in `/etc/rc.d/` are executed
alphabetically by `rc.sysinit2`. `/etc` and `/mnt/internal` survive reboots.

```sh
SSH="ssh -o KexAlgorithms=+diffie-hellman-group1-sha1 -o HostKeyAlgorithms=+ssh-rsa"
SCP="scp -O -o KexAlgorithms=+diffie-hellman-group1-sha1 -o HostKeyAlgorithms=+ssh-rsa"

# 1. Install binary to persistent storage
$SCP c4_mqtt_bridge root@<EC100_IP>:/mnt/internal/c4_mqtt_bridge

# 2. Create init.d service script
$SSH root@<EC100_IP> 'cat > /etc/init.d/c4bridge << '"'"'SCRIPT'"'"'
#!/bin/sh
BIN=/mnt/internal/c4_mqtt_bridge
LOG=/mnt/ram/var/log/c4bridge.log
case $1 in
start)   chmod +x $BIN; $BIN > $LOG 2>&1 & ;;
stop)    kill $(ps | grep c4_mqtt_bridge | grep -v grep | awk "{print \$1}") 2>/dev/null ;;
restart) $0 stop; sleep 1; $0 start ;;
getdisplayname) echo "C4 MQTT Bridge" ;;
esac
exit 0
SCRIPT
chmod +x /etc/init.d/c4bridge'

# 3. Create rc.d entry (runs after director, slot 90)
$SSH root@<EC100_IP> 'cat > /etc/rc.d/90c4bridge << '"'"'SCRIPT'"'"'
#!/bin/sh
case $1 in
start) (sleep 20; /etc/init.d/c4bridge start) & ;;
stop)  /etc/init.d/c4bridge stop ;;
getdisplayname) echo "C4 MQTT Bridge" ;;
esac
exit 0
SCRIPT
chmod +x /etc/rc.d/90c4bridge'
```

The 20-second delay in the startup script lets the director and Zigbee stack fully
initialize before the bridge connects. The bridge log after boot is at
`/mnt/ram/var/log/c4bridge.log` (RAM-based, so cleared each reboot — check it early).

To start/stop manually without rebooting:
```sh
$SSH root@<EC100_IP> "/etc/init.d/c4bridge start"
$SSH root@<EC100_IP> "/etc/init.d/c4bridge stop"
```

## Remove from EC-100

Stop the running bridge, remove startup hooks, remove binary, then clear the retained
HA discovery payloads so Home Assistant stops showing the device.

```sh
SSH="ssh -o KexAlgorithms=+diffie-hellman-group1-sha1 -o HostKeyAlgorithms=+ssh-rsa"

# Stop and remove from EC-100
$SSH root@<EC100_IP> "
  /etc/init.d/c4bridge stop
  rm -f /etc/rc.d/90c4bridge
  rm -f /etc/init.d/c4bridge
  rm -f /mnt/internal/c4_mqtt_bridge
"

# Clear retained HA discovery payloads (requires mosquitto_pub or similar)
mosquitto_pub -h <MQTT_BROKER_IP> -t homeassistant/climate/c4_thermostat/config -n -r
mosquitto_pub -h <MQTT_BROKER_IP> -t homeassistant/sensor/c4_thermostat_battery/config -n -r
mosquitto_pub -h <MQTT_BROKER_IP> -t c4/thermostat/state -n -r
mosquitto_pub -h <MQTT_BROKER_IP> -t c4/thermostat/avail -n -r
```

The `-n -r` flags publish a zero-length retained message, which clears the retained
value and causes Home Assistant to remove the auto-discovered device.

## MQTT Topics

| Topic | Direction | Description |
|---|---|---|
| `c4/thermostat/state` | Bridge → HA | JSON state (retained) |
| `c4/thermostat/avail` | Bridge → HA | `online` / `offline` (retained) |
| `homeassistant/climate/c4_thermostat/config` | Bridge → HA | HA MQTT Discovery payload (retained) |
| `homeassistant/sensor/c4_thermostat_battery/config` | Bridge → HA | Battery sensor discovery (retained) |
| `c4/thermostat/mode/set` | HA → Bridge | `off` / `heat` / `cool` |
| `c4/thermostat/fan/set` | HA → Bridge | `auto` / `on` |
| `c4/thermostat/temp_hi/set` | HA → Bridge | Cool setpoint in °F |
| `c4/thermostat/temp_lo/set` | HA → Bridge | Heat setpoint in °F |
| `c4/thermostat/preset/set` | HA → Bridge | `hold` / `none` |

State JSON example:
```json
{
  "temperature": 72.0,
  "heat_setpoint": 70.0,
  "cool_setpoint": 78.0,
  "hvac_mode": "heat",
  "fan_mode": "auto",
  "preset": "none",
  "battery": 79
}
```

---

# Control4 EC-100 Internal Protocol Notes

Everything below was reverse-engineered from live traffic captures,
and EC-100 on device log files.

## Director TCP API (c4soap)

The director (`/sbin/director`) listens on two ports:

| Port | Interface | Purpose |
|---|---|---|
| 5020 | 127.0.0.1 (loopback only) | Flash navigator (local) — **use this one** |
| 5021 | 0.0.0.0 (all interfaces) | Composer Pro (PC software), appears to speak a different protocol |

**Wire format:** null-byte (`\x00`) terminated XML strings over a persistent TCP connection.
Each message is one XML element followed by `\x00`. Responses come back the same way.
There is no framing header, no length prefix, no TLS.

**Authentication:** Send `AuthenticatePassword` first on the same connection. On this
system the director password is empty (no password set in `DirectorState.xml`):

```xml
<c4soap name="AuthenticatePassword" async="False" seq="1">
  <param name="password" type="string"></param>
</c4soap>
```

Response: `<c4soap name="AuthenticatePassword" seq="1" result="1"><success/></c4soap>`

Read-only commands (GetVariables, GetItems, GetBindings) work unauthenticated.
Write commands (SendToDeviceAsync, ExecuteCommand) require auth on the same connection.

**Event subscription:**
```xml
<c4soap name="EnableEvents" async="False" seq="2">
  <param name="enable" type="bool">1</param>
</c4soap>
```

After enabling events, the director pushes `OnVariableChanged` messages whenever any
variable changes, without polling.

## Device Hierarchy (thermostat)

| Device ID | Type | Description |
|---|---|---|
| 83 | WT-100 Zigbee driver | Hardware device — directly handles Zigbee |
| 84 | thermostatV2 proxy | Child proxy — **the correct target for all commands** |
| 76 | thermostatV2 proxy | IS_CONNECTED=False, inactive — ignore |

All thermostat commands go to **device 84** via `SendToDeviceAsync`.
Battery level comes from **device 83** (the hardware driver).

## Reading Variables

`GetVariables` (no filter) returns all variables for all devices as a single large XML
response (~88 KB). `GetVariablesByDevice` is broken and returns empty for most devices.
Parse the `GetVariables` dump locally, filtering by `deviceid`.

```xml
<c4soap name="GetVariables" async="False" seq="3"></c4soap>
```

Variables are returned as:
```xml
<variable deviceid="84" variableid="1104" name="HVAC_MODE"
          type="1" readonly="1" hidden="0" bindingid="0" bindingname="">Heat</variable>
```

## Device 84 Variable Reference

### Display variables (°F integer — most useful)

| varid | name | example | notes |
|---|---|---|---|
| 1117 | DISPLAY_TEMPERATURE | 72 | Current temperature in °F |
| 1118 | DISPLAY_HEATSETPOINT | 70 | Heat setpoint in °F |
| 1119 | DISPLAY_COOLSETPOINT | 78 | Cool setpoint in °F |

### V2 mode/state variables

| varid | name | example | notes |
|---|---|---|---|
| 1100 | SCALE | F | Temperature scale in use |
| 1101 | TEMPERATURE | 222 | Current temp in Celsius×10 |
| 1102 | HEAT_SETPOINT | 211 | Heat setpoint in Celsius×10 |
| 1103 | COOL_SETPOINT | 255 | Cool setpoint in Celsius×10 |
| 1104 | HVAC_MODE | Heat | "Off", "Heat", "Cool", "Emergency Heat" |
| 1105 | FAN_MODE | Auto | "Auto", "On" |
| 1106 | HOLD_MODE | Off | "Off", "2 Hours" |
| 1107 | HVAC_STATE | Off | Whether the unit is actively heating/cooling |
| 1108 | FAN_STATE | Off | Whether the fan is running |
| 1112 | IS_CONNECTED | True | Zigbee connectivity |
| 1120 | HVAC_MODES_LIST | Off,Heat,Cool,Emergency Heat | Comma-separated available modes |
| 1121 | FAN_MODES_LIST | Auto,On | |
| 1122 | HOLD_MODES_LIST | Off,2 Hours | Only two options |

### V2 temperature unit encoding

Celsius×10: `211` = 21.1 °C = 70 °F.  
Conversion: `°F = C10 / 10 * 9 / 5 + 32`

### V1 variables (legacy, °F, some writable)

varid 1000–1018 mirror the V2 state in °F integer. varids 1004 (HEAT_SETPOINT) and
1005 (COOL_SETPOINT) have `readonly=0` but writing them via `SetVariable` only updates
the director cache — it does not push the value to the thermostat hardware.

### Device 83 variables

| varid | name | example | notes |
|---|---|---|---|
| 1001 | BatteryLevel | 79 | % integer; fluctuates slightly with ambient temperature |

## Sending Commands (confirmed working)

**Use `SendToDeviceAsync`** — NOT `ExecuteCommand`. `ExecuteCommand` with `iditem`
returns `result=1` but has no effect on the thermostat.

### Command XML envelope

```xml
<c4soap name="SendToDeviceAsync" async="True" seq="10">
  <param type="number" name="iddevice">84</param>
  <param type="string" name="data">
    <devicecommand>
      <command>COMMAND_NAME</command>
      <params>
        <param>
          <name>PARAM_NAME</name>
          <value type="string"><static>VALUE</static></value>
        </param>
      </params>
    </devicecommand>
  </param>
</c4soap>
```

### Mode commands — param name `MODE`

| Command | Param | Values |
|---|---|---|
| `SET_MODE_HVAC` | `MODE` | `Heat`, `Cool`, `Off`, `Emergency Heat` |
| `SET_MODE_FAN` | `MODE` | `Auto`, `On` |
| `SET_MODE_HOLD` | `MODE` | `Off`, `2 Hours` |

Values are **titlecase** (e.g. `Heat` not `heat`, `2 Hours` not `2 hours`).

The param name `MODE` is always `MODE` for these three commands regardless of what
the command does. 

### Setpoint commands — param name `KELVIN`, value in Kelvin×10

| Command | Param | Value |
|---|---|---|
| `SET_SETPOINT_HEAT` | `KELVIN` | Kelvin×10 integer |
| `SET_SETPOINT_COOL` | `KELVIN` | Kelvin×10 integer |

**Why Kelvin×10:** The Navigator SDK's `ThermostatV2Driver` ActionScript class uses
`KELVIN` as the param name for both vacation setpoints (`SET_VAC_SETPOINT_COOL/HEAT`)
and regular setpoints (`SET_SETPOINT_COOL/HEAT`). The `SendSetPoint` helper has no
separate string constant in the binary, meaning it reuses the `KELVIN` constant already
in the pool (confirmed: it appears before `SET_SETPOINT_HEAT/COOL` in the binary and
after `SET_VAC_SETPOINT_COOL/HEAT` which explicitly use it).

The director converts Kelvin×10 to Celsius×10 for storage (subtracts 2732).

Conversion formula (°F → Kelvin×10):
```c
int k10 = (int)((f - 32.0f) * 50.0f / 9.0f + 2731.5f + 0.5f);
```

Verification: 70 °F → k10=2943, 2943−2732=211 ✓ (matches stored HEAT_SETPOINT)  
Verification: 78 °F → k10=2987, 2987−2732=255 ✓ (matches stored COOL_SETPOINT)

**Commands that do NOT work:**
- `SET_SETPOINT_COOL` with param `hvac_mode`, `holdmode`, `hold_mode`, `SETPOINT` — silently ignored
- `SET_SETPOINT_COOL` with param `MODE` and °F value — silently ignored (wrong unit)
- `ExecuteCommand` with `iditem` for any thermostat command — returns result=1 but no effect

## Thermostat Polling Behavior (Zigbee timing)

The WT100 is a sleepy Zigbee end device. Two distinct polling patterns:

- **Idle / night:** polls every ~18 minutes (matches `IndoorTemp` log entries in
  `/mnt/ram/var/log/tmp_selogs/`)
- **Active (unit running):** polls every 4–5 minutes

Commands sent to the director are queued for the next Zigbee poll. If the thermostat is
in 18-minute sleep, the command sits in the queue and may expire before the thermostat
wakes. During active cooling/heating (daytime), commands reliably take effect within
5–7 seconds because the poll interval is short.

The bridge does not implement wakeup-waiting — commands are sent immediately upon receipt
from MQTT. This works well during active use; during deep sleep the command may be
delayed up to 18 minutes.

## Hold Mode Behavior

`SET_MODE_HOLD MODE=2 Hours` enables a 2-hour manual override on the current setpoints
and mode, preventing the schedule from reverting them. After 2 hours the thermostat
returns to its programmed schedule.

The bridge exposes hold as an HA `preset_mode`: `none` (hold off, follow schedule) or
`hold` (2-hour override). The 2-hour timer resets each time `SET_MODE_HOLD MODE=2 Hours`
is sent. The bridge re-sends it automatically every 90 minutes while hold is active to
prevent expiry.

Setpoint or mode changes from HA do **not** automatically set hold — the user must
explicitly set `preset=hold` from HA to lock the new settings against the schedule.

## EC-100 SSH Access

The EC-100 runs an old OpenSSH server that requires legacy negotiation:

```sh
ssh -o KexAlgorithms=+diffie-hellman-group1-sha1 \
    -o HostKeyAlgorithms=+ssh-rsa \
    root@<EC100_IP>
```

For `scp`, add `-O` to force the legacy SCP protocol (the default SFTP subsystem is
not available on this server):

```sh
scp -O \
    -o KexAlgorithms=+diffie-hellman-group1-sha1 \
    -o HostKeyAlgorithms=+ssh-rsa \
    file root@<EC100_IP>:/tmp/
```

`pkill` is not available (BusyBox). To kill a process by name:
```sh
kill $(ps | grep c4_mqtt_bridge | grep -v grep | awk '{print $1}')
```

`nohup` is also not available. Background a process with:
```sh
/tmp/c4_mqtt_bridge > /tmp/bridge.log 2>&1 &
```

## Thermostat Activity Log

The director writes a timestamped CSV log for each thermostat device to:
```
/mnt/ram/var/log/tmp_selogs/YYYY-MM-DD_Thermostat_84_activity.log
```

Format: `YYYY-MM-DDThh:mm:ss,EventType,Value`

Useful for correlating command delivery with thermostat wakeup events:
```
2026-07-08T20:31:02,IndoorTemp,22.6
```

Each `IndoorTemp` entry marks a thermostat wakeup (Zigbee poll). If the gap between
entries is ~18 minutes, the thermostat is in deep sleep.
