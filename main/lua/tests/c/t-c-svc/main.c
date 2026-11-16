// A C app's background service (2.3.0, C_API.md "service"): the same module
// runs twice, the app at on_start and its service at on_service_start, each
// in its own instance. The app starts the service and sends it a message on
// SELECT; the service counts ticks, echoes messages back, sets its status
// and proves the screen and spending aren't there. Lines start "t-c-svc".
#include "../tlib.h"

static int s_ticks, s_msgs, s_svc;  // s_svc: this instance is the service

// The value at idx as JSON text (json.encode), for the log.
static void say_json(const char *prefix, int idx)
{
    char line[200], buf[160];
    unsigned n = slen(prefix);
    G("json.encode");
    sos_push_value(idx < 0 ? idx - 1 : idx);
    sos_call(1, 1);
    sos_to_str(-1, buf, sizeof(buf));
    sos_pop(1);
    for (unsigned i = 0; i < n; i++) line[i] = prefix[i];
    unsigned m = slen(buf);
    for (unsigned i = 0; i < m && n + i + 1 < sizeof(line); i++) line[n + i] = buf[i];
    line[n + m < sizeof(line) ? n + m : sizeof(line) - 1] = 0;
    say(line);
}

static int32_t started(void *ud, int32_t nargs)
{
    check(sos_to_bool(1), "t-c-svc service.start answered true");
    if (!sos_to_bool(1)) say_json("t-c-svc start error: ", 2);
    return 0;
}

// ---- the app (on_start) ----

__attribute__((export_name("on_start"))) void on_start(void)
{
    G("service.running");
    sos_call(0, 1);
    say(sos_to_bool(-1) ? "t-c-svc app: service already running" : "t-c-svc app: starting the service");
    sos_pop(1);
    G("service.start");
    sos_push_fn(started, 0);
    sos_call(1, 0);
    G("service.status");
    check(sos_type(-1) == SOS_NIL, "t-c-svc app: no service.status in the app");
    sos_pop(1);
}

__attribute__((export_name("on_key"))) int32_t on_key(int32_t key, int32_t rep)
{
    if (key != SOS_KEY_SELECT) return 0;
    G("service.send");
    sos_new_table();
    sos_push_int(++s_msgs);
    set_field(-2, "n");
    sos_call(1, 1);
    check(sos_to_bool(-1), "t-c-svc app: service.send queued");
    sos_pop(1);
    return 1;
}

// The service's message: badge_arg(1), frame value 1.
__attribute__((export_name("on_service"))) void on_service(void)
{
    say_json("t-c-svc app: from the service ", 1);
}

// ---- the service (on_service_start) ----

__attribute__((export_name("on_service_start"))) void on_service_start(void)
{
    s_svc = 1;
    say("t-c-svc svc: started");
    static const char *const nils[] = {"ui", "led", "wifi", "ble", "lamps.spend", "lamps.transfer", "items.grant",
                                       "mesh.channel", "sys.keys", "service.start"};
    int bad = 0;
    for (unsigned i = 0; i < sizeof(nils) / sizeof(nils[0]); i++) {
        if (G(nils[i]) != SOS_NIL) {
            bad++;
            say(nils[i]);
        }
        sos_pop(1);
    }
    check(bad == 0, "t-c-svc svc: the screen, spending and the radio's channel are nil");
    // Calling one fails like any call of nothing (C_API.md "service"): BADGE_ERR in badge.h.
    G("lamps.spend");
    sos_push_int(1);
    int32_t rc = sos_call(1, 0);
    char err[96];
    sos_to_str(-1, err, sizeof(err));
    check(rc != 0, "t-c-svc svc: calling lamps.spend fails");
    say(err);
    sos_pop(sos_top());
    check(G("service.status") == SOS_FN, "t-c-svc svc: service.status is there");
    sos_pop(1);
}

__attribute__((export_name("on_tick"))) void on_tick(int32_t ms)
{
    s_ticks++;
    // The buttons never reach a service (svc-run.ts holds some down meanwhile).
    if (s_svc) {
        int keys = sos_keys();
        if (keys) say_int("t-c-svc svc: FAIL read the buttons ", keys);
        static unsigned short px[16] __attribute__((aligned(64)));
        if (sos_frame(px, 4, 4, 0, 0, 4, 4, 0) != -1) say("t-c-svc svc: FAIL drew a frame");
    }
    G("service.status");
    char line[32];
    fmt_int(line, "C ticks ", s_ticks);
    push_s(line);
    sos_call(1, 0);
    if (s_ticks == 2) say_int("t-c-svc svc: tick ms ", ms);
}

__attribute__((export_name("on_message"))) void on_message(void)
{
    say_json("t-c-svc svc: message ", 1);
    G("service.send");
    sos_new_table();
    sos_push_int(s_ticks);
    set_field(-2, "ticks");
    sos_call(1, 1);
    sos_pop(1);
}

__attribute__((export_name("on_stop"))) void on_stop(void)
{
    say_int("t-c-svc svc: on_stop after ticks ", s_ticks);
}
