/*
 * c4_mqtt_bridge.c
 * Control4 EC-100 Thermostat ↔ Home Assistant MQTT bridge (no external deps)
 *
 * Cross-compile for ARMv5TEJL (EC-100):
 *   zig cc -target arm-linux-musleabi -mcpu=arm926ej_s -O2 -static \
 *          c4_mqtt_bridge.c arm5_atomics.S -o c4_mqtt_bridge
 *
 * Director (c4soap): null-byte terminated XML over TCP 127.0.0.1:5020
 * MQTT: v3.1.1, QoS 0, no TLS, no auth
 *
 * Thermostat device 84 (thermostatV2 proxy). All commands via SendToDeviceAsync
 * with iddevice=84 and param name "MODE". Values are in °F (SCALE=F on this system).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <time.h>

/* ── Configuration ─────────────────────────────────────────────────────── */
#define DIRECTOR_HOST   "127.0.0.1"
#define DIRECTOR_PORT   5020
#define DIRECTOR_PASS   ""
#define DEVICE_ID       84
#define HW_DEVICE_ID    83   /* WT100 hardware driver — holds BatteryLevel */

#define MQTT_HOST       "MQTT_BROKER_IP"   /* override: ./c4_mqtt_bridge <host> [port] */
#define MQTT_PORT       1883
#define MQTT_CLIENT_ID  "c4_thermostat_bridge"
#define MQTT_KEEPALIVE  60

#define DIRECTOR_RECONNECT_DELAY  10
#define HOLD_REFRESH_INTERVAL     5400  /* re-assert hold every 90 min */

/* ── MQTT topics ────────────────────────────────────────────────────────── */
/* Single JSON state topic; HA reads sub-values via templates */
#define T_STATE     "c4/thermostat/state"
#define T_AVAIL     "c4/thermostat/avail"
#define T_DISC      "homeassistant/climate/c4_thermostat/config"
#define T_DISC_BATT "homeassistant/sensor/c4_thermostat_battery/config"

/* Inbound command topics */
#define T_MODE_SET      "c4/thermostat/mode/set"       /* "heat"|"cool"|"off" */
#define T_FAN_SET       "c4/thermostat/fan/set"        /* "auto"|"on" */
#define T_TEMP_HI_SET   "c4/thermostat/temp_hi/set"   /* cool setpoint °F */
#define T_TEMP_LO_SET   "c4/thermostat/temp_lo/set"   /* heat setpoint °F */
#define T_PRESET_SET    "c4/thermostat/preset/set"     /* "none"|"hold" */

/* ── Thermostat variable IDs (device 84) ───────────────────────────────── */
/* V2 mode/state variables — reported via OnVariableChanged */
#define VID_HVAC_MODE        1104   /* "Off","Heat","Cool","Emergency Heat" */
#define VID_FAN_MODE         1105   /* "Auto","On" */
#define VID_HOLD_MODE        1106   /* "Off","2 Hours" */
#define VID_HVAC_STATE       1107   /* "Off","On" — whether unit is running */
#define VID_IS_CONNECTED     1112   /* "True"/"False" */
/* Display variables already in °F — simplest for HA */
#define VID_DISPLAY_TEMP     1117   /* °F integer */
#define VID_DISPLAY_HEAT_SP  1118   /* heat setpoint °F */
#define VID_DISPLAY_COOL_SP  1119   /* cool setpoint °F */
/* V2 raw Celsius×10 — fallback if display vars don't fire events */
#define VID_TEMPERATURE      1101   /* Celsius×10 */
#define VID_HEAT_SETPOINT    1102   /* Celsius×10 */
#define VID_COOL_SETPOINT    1103   /* Celsius×10 */
/* Hardware device 83 */
#define VID_BATTERY_LEVEL    1001   /* % integer, on HW_DEVICE_ID=83 */

/* ── Minimal MQTT v3.1.1 ────────────────────────────────────────────────── */

typedef struct {
    int      fd;
    int      keepalive;
    time_t   last_ping;
    uint16_t pkt_id;
    void (*on_connect)(int rc);
    void (*on_message)(const char *topic, const char *payload, int len);
} MqttClient;

static int mqtt_encode_len(uint8_t *buf, int len) {
    int pos = 0;
    do {
        uint8_t b = len % 128; len /= 128;
        if (len > 0) b |= 0x80;
        buf[pos++] = b;
    } while (len > 0);
    return pos;
}

static int mqtt_connect(MqttClient *mc, const char *host, int port) {
    struct sockaddr_in addr = {0};
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    mc->fd = fd;

    /* CONNECT packet — clean session, no credentials */
    uint8_t var[10]; int vp = 0;
    var[vp++] = 0; var[vp++] = 4;
    var[vp++] = 'M'; var[vp++] = 'Q'; var[vp++] = 'T'; var[vp++] = 'T';
    var[vp++] = 4;         /* protocol level MQTTv3.1.1 */
    var[vp++] = 0x02;      /* flags: clean session only */
    uint16_t ka = (uint16_t)mc->keepalive;
    var[vp++] = ka >> 8; var[vp++] = ka & 0xFF;

    uint16_t cidlen = (uint16_t)strlen(MQTT_CLIENT_ID);
    int paylen = 2 + cidlen;

    uint8_t buf[256]; int bp = 0;
    buf[bp++] = 0x10;
    bp += mqtt_encode_len(buf + bp, vp + paylen);
    memcpy(buf + bp, var, vp); bp += vp;
    buf[bp++] = cidlen >> 8; buf[bp++] = cidlen & 0xFF;
    memcpy(buf + bp, MQTT_CLIENT_ID, cidlen); bp += cidlen;

    if (send(fd, buf, bp, 0) != bp) { close(fd); mc->fd = -1; return -1; }
    mc->last_ping = time(NULL);
    return 0;
}

static int mqtt_publish(MqttClient *mc, const char *topic,
                        const char *payload, int plen, int retain) {
    if (mc->fd < 0) return -1;
    uint8_t buf[4096]; int bp = 0;
    uint16_t tlen = (uint16_t)strlen(topic);
    int remaining = 2 + tlen + plen;
    buf[bp++] = retain ? 0x31 : 0x30;
    bp += mqtt_encode_len(buf + bp, remaining);
    buf[bp++] = tlen >> 8; buf[bp++] = tlen & 0xFF;
    memcpy(buf + bp, topic, tlen); bp += tlen;
    if (plen > 0) { memcpy(buf + bp, payload, plen); bp += plen; }
    return (send(mc->fd, buf, bp, 0) == bp) ? 0 : -1;
}

static void mqtt_subscribe(MqttClient *mc, const char *topic) {
    if (mc->fd < 0) return;
    uint8_t buf[256]; int bp = 0;
    uint16_t tlen = (uint16_t)strlen(topic);
    uint16_t pid = ++mc->pkt_id;
    buf[bp++] = 0x82;
    bp += mqtt_encode_len(buf + bp, 2 + 2 + tlen + 1);
    buf[bp++] = pid >> 8; buf[bp++] = pid & 0xFF;
    buf[bp++] = tlen >> 8; buf[bp++] = tlen & 0xFF;
    memcpy(buf + bp, topic, tlen); bp += tlen;
    buf[bp++] = 0x00;
    send(mc->fd, buf, bp, 0);
}

static int mqtt_ping(MqttClient *mc) {
    uint8_t p[2] = {0xC0, 0x00};
    mc->last_ping = time(NULL);
    return (send(mc->fd, p, 2, 0) == 2) ? 0 : -1;
}

static int mqtt_read_packet(MqttClient *mc) {
    uint8_t hdr;
    int r = recv(mc->fd, &hdr, 1, MSG_DONTWAIT);
    if (r == 0) return -1;
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 1 : -1;

    int remaining = 0, shift = 0; uint8_t b;
    do {
        if (recv(mc->fd, &b, 1, 0) != 1) return -1;
        remaining |= (b & 0x7F) << shift; shift += 7;
    } while (b & 0x80);

    uint8_t *pkt = NULL;
    if (remaining > 0) {
        pkt = malloc(remaining + 1);
        if (!pkt) return -1;
        int got = 0;
        while (got < remaining) {
            int n = recv(mc->fd, pkt + got, remaining - got, 0);
            if (n <= 0) { free(pkt); return -1; }
            got += n;
        }
        pkt[remaining] = '\0';
    }

    switch ((hdr >> 4) & 0x0F) {
    case 2:   /* CONNACK */
        if (mc->on_connect)
            mc->on_connect(pkt && remaining >= 2 ? pkt[1] : 1);
        break;
    case 3: { /* PUBLISH */
        if (mc->on_message && pkt && remaining >= 2) {
            uint16_t tlen = ((uint16_t)pkt[0] << 8) | pkt[1];
            if ((int)(2 + tlen) <= remaining) {
                char topic[256]; int tc = tlen < 255 ? (int)tlen : 255;
                memcpy(topic, pkt + 2, tc); topic[tc] = '\0';
                const char *data = (char*)(pkt + 2 + tlen);
                int dlen = remaining - 2 - (int)tlen;
                mc->on_message(topic, data, dlen < 0 ? 0 : dlen);
            }
        }
        break;
    }
    default: break;
    }
    if (pkt) free(pkt);
    return 0;
}

/* ── Thermostat state ───────────────────────────────────────────────────── */

typedef struct {
    float temperature_f;     /* DISPLAY_TEMPERATURE (1117) */
    float heat_setpoint_f;   /* DISPLAY_HEATSETPOINT (1118) */
    float cool_setpoint_f;   /* DISPLAY_COOLSETPOINT (1119) */
    char  hvac_mode[32];     /* HVAC_MODE (1104): "Off","Heat","Cool" */
    char  fan_mode[16];      /* FAN_MODE (1105): "Auto","On" */
    char  hold_mode[16];     /* HOLD_MODE (1106): "Off","2 Hours" */
    int   is_connected;
    int   battery;           /* BatteryLevel % (device 83, varid 1001) */
} TstatState;

static TstatState  g_state  = {0};
static int         g_dir_fd = -1;
static MqttClient  g_mqtt   = {.fd = -1, .keepalive = MQTT_KEEPALIVE};
static time_t      g_last_hold_refresh = 0;

/* ── XML helpers ────────────────────────────────────────────────────────── */

static int xml_attr(const char *xml, const char *attr, char *out, int outlen) {
    char srch[128]; snprintf(srch, sizeof(srch), "%s=\"", attr);
    const char *p = strstr(xml, srch);
    if (!p) return 0;
    p += strlen(srch);
    const char *e = strchr(p, '"'); if (!e) return 0;
    int len = (int)(e - p); if (len >= outlen) len = outlen - 1;
    memcpy(out, p, len); out[len] = '\0';
    return 1;
}

static int xml_param_val(const char *xml, const char *pname, char *out, int outlen) {
    char srch[128]; snprintf(srch, sizeof(srch), "name=\"%s\"", pname);
    const char *p = strstr(xml, srch); if (!p) return 0;
    p = strchr(p, '>'); if (!p) return 0; p++;
    const char *e = strchr(p, '<'); if (!e) return 0;
    int len = (int)(e - p); if (len >= outlen) len = outlen - 1;
    memcpy(out, p, len); out[len] = '\0';
    return 1;
}

/* ── Director command sender ────────────────────────────────────────────── */

/* Confirmed-working format: SendToDeviceAsync, iddevice=84.
 * Mode commands use param name "MODE". Setpoint commands use param name "KELVIN"
 * with value in Kelvin×10 (as used by both vacation and regular setpoints in SDK). */
static void dir_cmd_param(int fd, const char *cmd, const char *pname,
                          const char *val, int seq) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "<c4soap name=\"SendToDeviceAsync\" async=\"True\" seq=\"%d\">"
        "<param type=\"number\" name=\"iddevice\">%d</param>"
        "<param type=\"string\" name=\"data\">"
        "<devicecommand><command>%s</command>"
        "<params><param><name>%s</name>"
        "<value type=\"string\"><static>%s</static></value>"
        "</param></params></devicecommand>"
        "</param></c4soap>",
        seq, DEVICE_ID, cmd, pname, val);
    int n = strlen(buf); buf[n] = '\0';
    send(fd, buf, n + 1, 0);
    fprintf(stderr, "[c4] %s %s=%s\n", cmd, pname, val);
}

static void dir_cmd(int fd, const char *cmd, const char *val, int seq) {
    dir_cmd_param(fd, cmd, "MODE", val, seq);
}

/* Convert °F to Kelvin×10 for setpoint commands */
static int f_to_k10(float f) {
    return (int)((f - 32.0f) * 50.0f / 9.0f + 2731.5f + 0.5f);
}

/* ── State → MQTT publish ───────────────────────────────────────────────── */

static void publish_state(void) {
    if (g_mqtt.fd < 0) return;

    /* HA expects lowercase mode names */
    char mode[32] = {0};
    for (int i = 0; g_state.hvac_mode[i] && i < 31; i++)
        mode[i] = tolower((unsigned char)g_state.hvac_mode[i]);

    char fan[16] = {0};
    for (int i = 0; g_state.fan_mode[i] && i < 15; i++)
        fan[i] = tolower((unsigned char)g_state.fan_mode[i]);

    /* hold → HA preset: "Off" = "none", "2 Hours" = "hold" */
    const char *preset = strcmp(g_state.hold_mode, "Off") == 0 ? "none" : "hold";

    char json[512];
    snprintf(json, sizeof(json),
        "{\"temperature\":%.1f,"
        "\"heat_setpoint\":%.1f,"
        "\"cool_setpoint\":%.1f,"
        "\"hvac_mode\":\"%s\","
        "\"fan_mode\":\"%s\","
        "\"preset\":\"%s\","
        "\"battery\":%d}",
        g_state.temperature_f,
        g_state.heat_setpoint_f,
        g_state.cool_setpoint_f,
        mode, fan, preset,
        g_state.battery);

    mqtt_publish(&g_mqtt, T_STATE, json, strlen(json), 1);
}

/* ── Variable parsing ───────────────────────────────────────────────────── */

static void apply_var(int varid, const char *val) {
    switch (varid) {
    case VID_DISPLAY_TEMP:
        g_state.temperature_f = (float)atoi(val); break;
    case VID_DISPLAY_HEAT_SP:
        g_state.heat_setpoint_f = (float)atoi(val); break;
    case VID_DISPLAY_COOL_SP:
        g_state.cool_setpoint_f = (float)atoi(val); break;
    /* V2 Celsius×10 fallback if display vars not yet received */
    case VID_TEMPERATURE:
        if (g_state.temperature_f == 0)
            g_state.temperature_f = atoi(val) / 10.0f * 9.0f / 5.0f + 32.0f;
        break;
    case VID_HEAT_SETPOINT:
        if (g_state.heat_setpoint_f == 0)
            g_state.heat_setpoint_f = atoi(val) / 10.0f * 9.0f / 5.0f + 32.0f;
        break;
    case VID_COOL_SETPOINT:
        if (g_state.cool_setpoint_f == 0)
            g_state.cool_setpoint_f = atoi(val) / 10.0f * 9.0f / 5.0f + 32.0f;
        break;
    case VID_HVAC_MODE:
        strncpy(g_state.hvac_mode, val, sizeof(g_state.hvac_mode) - 1); break;
    case VID_FAN_MODE:
        strncpy(g_state.fan_mode, val, sizeof(g_state.fan_mode) - 1); break;
    case VID_HOLD_MODE:
        strncpy(g_state.hold_mode, val, sizeof(g_state.hold_mode) - 1); break;
    case VID_IS_CONNECTED:
        g_state.is_connected = (strcmp(val, "True") == 0); break;
    /* VID_BATTERY_LEVEL is on HW_DEVICE_ID=83, handled separately */
    default: break;
    }
}

static void apply_hw_var(int varid, const char *val) {
    if (varid == VID_BATTERY_LEVEL)
        g_state.battery = atoi(val);
}

/* Map variable NAME (from GetVariables) → varid, then apply */
static void apply_var_by_name(const char *name, const char *val) {
    static const struct { const char *n; int id; } map[] = {
        {"DISPLAY_TEMPERATURE",  VID_DISPLAY_TEMP},
        {"DISPLAY_HEATSETPOINT", VID_DISPLAY_HEAT_SP},
        {"DISPLAY_COOLSETPOINT", VID_DISPLAY_COOL_SP},
        {"TEMPERATURE",          VID_TEMPERATURE},
        {"HEAT_SETPOINT",        VID_HEAT_SETPOINT},
        {"COOL_SETPOINT",        VID_COOL_SETPOINT},
        {"HVAC_MODE",            VID_HVAC_MODE},
        {"FAN_MODE",             VID_FAN_MODE},
        {"HOLD_MODE",            VID_HOLD_MODE},
        {"IS_CONNECTED",         VID_IS_CONNECTED},
        {NULL, 0}
    };
    for (int i = 0; map[i].n; i++)
        if (!strcmp(name, map[i].n)) { apply_var(map[i].id, val); return; }
    if (!strcmp(name, "BatteryLevel")) g_state.battery = atoi(val);
}

/* ── Director message processor ─────────────────────────────────────────── */

static void process_dir_msg(const char *msg) {
    char ename[64] = {0};
    xml_attr(msg, "name", ename, sizeof(ename));

    if (!strcmp(ename, "OnVariableChanged")) {
        char devid[16] = {0}, varid_s[16] = {0}, val[256] = {0};
        xml_param_val(msg, "iddevice",  devid,  sizeof(devid));
        xml_param_val(msg, "idvariable", varid_s, sizeof(varid_s));
        xml_param_val(msg, "value",     val,    sizeof(val));
        int dev = atoi(devid);
        if (dev == DEVICE_ID)
            apply_var(atoi(varid_s), val);
        else if (dev == HW_DEVICE_ID)
            apply_hw_var(atoi(varid_s), val);
        else return;
        publish_state();

    } else if (!strcmp(ename, "GetVariables")) {
        /* Parse initial state dump */
        const char *p = msg;
        while ((p = strstr(p, "<variable ")) != NULL) {
            char devid[16] = {0}, vname[64] = {0};
            xml_attr(p, "deviceid", devid, sizeof(devid));
            int devi = atoi(devid);
            if (devi == DEVICE_ID || devi == HW_DEVICE_ID) {
                xml_attr(p, "name", vname, sizeof(vname));
                const char *vs = strchr(p + 10, '>');
                if (vs++) {
                    const char *ve = strchr(vs, '<');
                    if (ve) {
                        char vval[256] = {0};
                        int vl = (int)(ve - vs);
                        if (vl > 0 && vl < (int)sizeof(vval))
                            memcpy(vval, vs, vl);
                        apply_var_by_name(vname, vval);
                    }
                }
            }
            p++;
        }
        fprintf(stderr, "[c4] state: mode=%s temp=%.1f°F cool=%.1f heat=%.1f hold=%s\n",
                g_state.hvac_mode, g_state.temperature_f,
                g_state.cool_setpoint_f, g_state.heat_setpoint_f,
                g_state.hold_mode);
        publish_state();
    }
}

/* ── Director connection ─────────────────────────────────────────────────── */

static int director_connect(void) {
    struct sockaddr_in addr = {0};
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DIRECTOR_PORT);
    inet_pton(AF_INET, DIRECTOR_HOST, &addr.sin_addr);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }

    char buf[512]; int n;
#define SEND_NULL(s) do { n = strlen(s); (s)[n] = '\0'; send(fd, (s), n+1, 0); } while(0)

    snprintf(buf, sizeof(buf),
        "<c4soap name=\"AuthenticatePassword\" async=\"False\" seq=\"1\">"
        "<param name=\"password\" type=\"string\">%s</param></c4soap>", DIRECTOR_PASS);
    SEND_NULL(buf);

    snprintf(buf, sizeof(buf),
        "<c4soap name=\"EnableEvents\" async=\"False\" seq=\"2\">"
        "<param name=\"enable\" type=\"bool\">1</param></c4soap>");
    SEND_NULL(buf);

    snprintf(buf, sizeof(buf),
        "<c4soap name=\"GetVariables\" async=\"False\" seq=\"3\"></c4soap>");
    SEND_NULL(buf);
#undef SEND_NULL

    fprintf(stderr, "[c4] connected to director\n");
    return fd;
}

/* ── MQTT callbacks ──────────────────────────────────────────────────────── */

static const char *DISC_PAYLOAD =
    "{"
    "\"name\":\"C4 Thermostat\","
    "\"unique_id\":\"c4_tstat_84\","
    "\"device\":{"
      "\"identifiers\":[\"c4_tstat_84\"],"
      "\"name\":\"Control4 Thermostat\","
      "\"manufacturer\":\"Control4\","
      "\"model\":\"WT100\""
    "},"
    "\"current_temperature_topic\":\"" T_STATE "\","
    "\"current_temperature_template\":\"{{ value_json.temperature }}\","
    "\"mode_state_topic\":\"" T_STATE "\","
    "\"mode_state_template\":\"{{ value_json.hvac_mode }}\","
    "\"mode_command_topic\":\"" T_MODE_SET "\","
    "\"temperature_high_state_topic\":\"" T_STATE "\","
    "\"temperature_high_state_template\":\"{{ value_json.cool_setpoint }}\","
    "\"temperature_high_command_topic\":\"" T_TEMP_HI_SET "\","
    "\"temperature_low_state_topic\":\"" T_STATE "\","
    "\"temperature_low_state_template\":\"{{ value_json.heat_setpoint }}\","
    "\"temperature_low_command_topic\":\"" T_TEMP_LO_SET "\","
    "\"fan_mode_state_topic\":\"" T_STATE "\","
    "\"fan_mode_state_template\":\"{{ value_json.fan_mode }}\","
    "\"fan_mode_command_topic\":\"" T_FAN_SET "\","
    "\"preset_mode_state_topic\":\"" T_STATE "\","
    "\"preset_mode_value_template\":\"{{ value_json.preset }}\","
    "\"preset_mode_command_topic\":\"" T_PRESET_SET "\","
    "\"preset_modes\":[\"hold\"],"
    "\"modes\":[\"off\",\"heat\",\"cool\"],"
    "\"fan_modes\":[\"auto\",\"on\"],"
    "\"temperature_unit\":\"F\","
    "\"min_temp\":60,\"max_temp\":90,\"temp_step\":1,"
    "\"availability_topic\":\"" T_AVAIL "\","
    "\"payload_available\":\"online\","
    "\"payload_not_available\":\"offline\""
    "}";

static const char *DISC_BATT_PAYLOAD =
    "{"
    "\"name\":\"C4 Thermostat Battery\","
    "\"unique_id\":\"c4_tstat_84_battery\","
    "\"device\":{"
      "\"identifiers\":[\"c4_tstat_84\"],"
      "\"name\":\"Control4 Thermostat\","
      "\"manufacturer\":\"Control4\","
      "\"model\":\"WT100\""
    "},"
    "\"state_topic\":\"" T_STATE "\","
    "\"value_template\":\"{{ value_json.battery }}\","
    "\"device_class\":\"battery\","
    "\"unit_of_measurement\":\"%\","
    "\"entity_category\":\"diagnostic\","
    "\"availability_topic\":\"" T_AVAIL "\","
    "\"payload_available\":\"online\","
    "\"payload_not_available\":\"offline\""
    "}";

static void on_mqtt_connect(int rc) {
    if (rc != 0) { fprintf(stderr, "[mqtt] connect failed rc=%d\n", rc); return; }
    fprintf(stderr, "[mqtt] connected\n");

    /* Announce device to Home Assistant */
    mqtt_publish(&g_mqtt, T_DISC, DISC_PAYLOAD, strlen(DISC_PAYLOAD), 1);
    mqtt_publish(&g_mqtt, T_DISC_BATT, DISC_BATT_PAYLOAD, strlen(DISC_BATT_PAYLOAD), 1);
    mqtt_publish(&g_mqtt, T_AVAIL, "online", 6, 1);

    /* Subscribe to all command topics */
    mqtt_subscribe(&g_mqtt, T_MODE_SET);
    mqtt_subscribe(&g_mqtt, T_FAN_SET);
    mqtt_subscribe(&g_mqtt, T_TEMP_HI_SET);
    mqtt_subscribe(&g_mqtt, T_TEMP_LO_SET);
    mqtt_subscribe(&g_mqtt, T_PRESET_SET);

    publish_state();
}

/* Titlecase: "heat" → "Heat" (C4 mode values are titlecase) */
static void titlecase(const char *src, char *dst, int maxlen) {
    if (!src[0]) { dst[0] = '\0'; return; }
    int i = 0;
    dst[i] = toupper((unsigned char)src[i]); i++;
    for (; src[i] && i < maxlen - 1; i++)
        dst[i] = tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

static void on_mqtt_message(const char *topic, const char *payload, int plen) {
    if (g_dir_fd < 0) { fprintf(stderr, "[mqtt] no director, dropping\n"); return; }
    char buf[64]; int len = plen < 63 ? plen : 63;
    memcpy(buf, payload, len); buf[len] = '\0';
    fprintf(stderr, "[mqtt] %s = \"%s\"\n", topic, buf);

    int seq = 20;
    if (!strcmp(topic, T_MODE_SET)) {
        /* HA sends lowercase; C4 wants titlecase */
        char mode[32]; titlecase(buf, mode, sizeof(mode));
        dir_cmd(g_dir_fd, "SET_MODE_HVAC", mode, seq++);

    } else if (!strcmp(topic, T_FAN_SET)) {
        char fan[16]; titlecase(buf, fan, sizeof(fan));
        dir_cmd(g_dir_fd, "SET_MODE_FAN", fan, seq++);

    } else if (!strcmp(topic, T_TEMP_HI_SET)) {
        /* Cool setpoint: convert °F → Kelvin×10, param name KELVIN */
        char kval[16]; snprintf(kval, sizeof(kval), "%d", f_to_k10(atof(buf)));
        dir_cmd_param(g_dir_fd, "SET_SETPOINT_COOL", "KELVIN", kval, seq++);

    } else if (!strcmp(topic, T_TEMP_LO_SET)) {
        /* Heat setpoint: convert °F → Kelvin×10, param name KELVIN */
        char kval[16]; snprintf(kval, sizeof(kval), "%d", f_to_k10(atof(buf)));
        dir_cmd_param(g_dir_fd, "SET_SETPOINT_HEAT", "KELVIN", kval, seq++);

    } else if (!strcmp(topic, T_PRESET_SET)) {
        if (!strcmp(buf, "hold")) {
            dir_cmd(g_dir_fd, "SET_MODE_HOLD", "2 Hours", seq++);
            g_last_hold_refresh = time(NULL);
        } else {
            /* "none" or anything else → clear hold */
            dir_cmd(g_dir_fd, "SET_MODE_HOLD", "Off", seq++);
            g_last_hold_refresh = 0;
        }
    }
}

/* ── Main loop ──────────────────────────────────────────────────────────── */

static char g_dir_buf[131072];
static int  g_dir_buf_len = 0;

int main(int argc, char *argv[]) {
    const char *mqtt_host = (argc > 1) ? argv[1] : MQTT_HOST;
    int mqtt_port = (argc > 2) ? atoi(argv[2]) : MQTT_PORT;

    g_mqtt.on_connect = on_mqtt_connect;
    g_mqtt.on_message = on_mqtt_message;

    fprintf(stderr, "[bridge] device %d → MQTT %s:%d\n", DEVICE_ID, mqtt_host, mqtt_port);

    if (mqtt_connect(&g_mqtt, mqtt_host, mqtt_port) < 0) {
        fprintf(stderr, "[mqtt] initial connect failed, will retry\n");
        g_mqtt.fd = -1;
    }

    while (1) {
        if (g_dir_fd < 0) {
            g_dir_fd = director_connect();
            if (g_dir_fd < 0) {
                fprintf(stderr, "[c4] unavailable, retry in %ds\n", DIRECTOR_RECONNECT_DELAY);
                sleep(DIRECTOR_RECONNECT_DELAY);
                continue;
            }
            g_dir_buf_len = 0;
        }

        if (g_mqtt.fd < 0) {
            if (mqtt_connect(&g_mqtt, mqtt_host, mqtt_port) < 0)
                g_mqtt.fd = -1;
        }

        struct pollfd fds[2];
        fds[0].fd = g_dir_fd; fds[0].events = POLLIN;
        int nfds = 1;
        if (g_mqtt.fd >= 0) {
            fds[1].fd = g_mqtt.fd; fds[1].events = POLLIN;
            nfds = 2;
        }

        int ret = poll(fds, nfds, 1000);
        if (ret < 0) { if (errno == EINTR) continue; break; }

        /* Director */
        if (fds[0].revents & (POLLERR | POLLHUP)) {
            fprintf(stderr, "[c4] disconnected\n");
            close(g_dir_fd); g_dir_fd = -1;
            if (g_mqtt.fd >= 0)
                mqtt_publish(&g_mqtt, T_AVAIL, "offline", 7, 1);
            continue;
        }
        if (fds[0].revents & POLLIN) {
            int avail = (int)sizeof(g_dir_buf) - g_dir_buf_len - 1;
            if (avail <= 0) { g_dir_buf_len = 0; continue; }
            int n = recv(g_dir_fd, g_dir_buf + g_dir_buf_len, avail, 0);
            if (n <= 0) {
                close(g_dir_fd); g_dir_fd = -1;
                if (g_mqtt.fd >= 0)
                    mqtt_publish(&g_mqtt, T_AVAIL, "offline", 7, 1);
                continue;
            }
            g_dir_buf_len += n;
            int start = 0;
            for (int i = 0; i < g_dir_buf_len; i++) {
                if (g_dir_buf[i] == '\0') {
                    if (i > start) process_dir_msg(g_dir_buf + start);
                    start = i + 1;
                }
            }
            if (start > 0) {
                g_dir_buf_len -= start;
                if (g_dir_buf_len > 0)
                    memmove(g_dir_buf, g_dir_buf + start, g_dir_buf_len);
            }
        }

        /* MQTT */
        if (nfds > 1 && (fds[1].revents & (POLLERR | POLLHUP))) {
            fprintf(stderr, "[mqtt] disconnected\n");
            close(g_mqtt.fd); g_mqtt.fd = -1; continue;
        }
        if (nfds > 1 && (fds[1].revents & POLLIN)) {
            if (mqtt_read_packet(&g_mqtt) < 0) {
                close(g_mqtt.fd); g_mqtt.fd = -1;
            }
        }

        /* MQTT keepalive */
        if (g_mqtt.fd >= 0 &&
            (time(NULL) - g_mqtt.last_ping) >= g_mqtt.keepalive / 2)
            mqtt_ping(&g_mqtt);

        /* Hold-mode refresh while hold is active */
        if (g_dir_fd >= 0 && g_last_hold_refresh > 0 &&
            (time(NULL) - g_last_hold_refresh) >= HOLD_REFRESH_INTERVAL) {
            dir_cmd(g_dir_fd, "SET_MODE_HOLD", "2 Hours", 99);
            g_last_hold_refresh = time(NULL);
        }
    }

    if (g_dir_fd >= 0) close(g_dir_fd);
    if (g_mqtt.fd >= 0) close(g_mqtt.fd);
    return 0;
}
