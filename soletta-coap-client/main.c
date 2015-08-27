#include <stdio.h>

#include <sol-coap.h>
#include <sol-gpio.h>
#include <sol-log.h>
#include <sol-mainloop.h>
#include <sol-network.h>

#include <ps.h>
#include <periph_cpu.h>

#define DEFAULT_UDP_PORT 5683

struct remote_light_context {
    struct sol_coap_server *server;
    struct sol_gpio *button;
    struct sol_gpio *led;
    struct sol_timeout *timeout;
    struct sol_network_link_addr addr;
    bool state;
    bool found;
};

static void
remote_light_toggle(struct remote_light_context *ctx)
{
    struct sol_coap_packet *req;
    uint8_t *payload;
    uint16_t len;

    req = sol_coap_packet_request_new(SOL_COAP_METHOD_PUT, SOL_COAP_TYPE_NONCON);
    sol_coap_add_option(req, SOL_COAP_OPTION_URI_PATH, "a", sizeof("a") - 1);
    sol_coap_add_option(req, SOL_COAP_OPTION_URI_PATH, "light", sizeof("light") - 1);

    sol_coap_packet_get_payload(req, &payload, &len);
    len = snprintf((char *)payload, len, "{\"oc\":[{\"rep\":{\"state\":\%s}}]}",
        ctx->state ? "true" : "false");
    sol_coap_packet_set_payload_used(req, len);
    sol_coap_send_packet(ctx->server, req, &ctx->addr);
}

static bool
timeout_cb(void *data)
{
    struct remote_light_context *ctx = data;

    ctx->state = !ctx->state;

    remote_light_toggle(ctx);

    ctx->timeout = NULL;

    return false;
}

static void
button_pressed(void *data, struct sol_gpio *gpio)
{
    struct remote_light_context *ctx = data;
    if (!ctx->found || ctx->timeout) return;
    ctx->timeout = sol_timeout_add(300, timeout_cb, ctx);
}

static struct sol_gpio *
setup_button(const struct remote_light_context *ctx)
{
    struct sol_gpio_config conf = {
        .api_version = SOL_GPIO_CONFIG_API_VERSION,
        .dir = SOL_GPIO_DIR_IN,
        .in = {
            .trigger_mode = SOL_GPIO_EDGE_FALLING,
            .cb = button_pressed,
            .user_data = ctx
        }
    };

    return sol_gpio_open(GPIO(PA, 28), &conf);
}

static struct sol_gpio *
setup_led(void)
{
    struct sol_gpio_config conf = {
        .api_version = SOL_GPIO_CONFIG_API_VERSION,
        .dir = SOL_GPIO_DIR_OUT,
        .active_low = true
    };

    return sol_gpio_open(GPIO(PA, 19), &conf);
}

static bool
get_state(struct sol_coap_packet *pkt)
{
    char *sub = NULL;
    uint8_t *payload;
    uint16_t len;

    if (!sol_coap_packet_has_payload(pkt)) {
        printf("No payload\n");
        return false;
    }

    sol_coap_packet_get_payload(pkt, &payload, &len);
    if (!payload)
        return false;
    printf("Payload: %.*s\n", len, payload);
    sub = strstr((char *)payload, "state\":");
    if (!sub)
        return false;
    return !memcmp(sub + strlen("state\":"), "true", sizeof("true") - 1);
}

static int
notification_reply_cb(struct sol_coap_packet *req, const struct sol_network_link_addr *cliaddr, void *data)
{
    struct remote_light_context *ctx = data;

    ps();

    ctx->state = get_state(req);
    sol_gpio_write(ctx->led, ctx->state);

    return 0;
}

static void
observe(struct remote_light_context *ctx)
{
    struct sol_coap_packet *req;
    uint8_t observe = 0;
    uint8_t token[] = { 0x36, 0x36, 0x36, 0x21 };

    req = sol_coap_packet_request_new(SOL_COAP_METHOD_GET, SOL_COAP_TYPE_CON);

    sol_coap_header_set_token(req, token, sizeof(token));

    sol_coap_add_option(req, SOL_COAP_OPTION_OBSERVE, &observe, sizeof(observe));

    sol_coap_add_option(req, SOL_COAP_OPTION_URI_PATH, "a", sizeof("a") - 1);
    sol_coap_add_option(req, SOL_COAP_OPTION_URI_PATH, "light", sizeof("light") - 1);

    sol_coap_send_packet_with_reply(ctx->server, req, &ctx->addr, notification_reply_cb, ctx);

    return true;
}

static int
discover_reply_cb(struct sol_coap_packet *req, const struct sol_network_link_addr *cliaddr, void *data)
{
    struct remote_light_context *ctx = data;
    char buf[SOL_INET_ADDR_STRLEN];

    sol_network_addr_to_str(cliaddr, buf, sizeof(buf));
    printf("Found resource in: %s\n", buf);

    if (ctx->found) {
        sol_network_addr_to_str(&ctx->addr, buf, sizeof(buf));
        printf("Ignoring, as we already had resource from: %s\n", buf);
        return 0;
    }

    ctx->found = true;

    memcpy(&ctx->addr, cliaddr, sizeof(ctx->addr));

    ctx->state = get_state(req);

    observe(ctx);

    return 0;
}

static bool
discover_resource(struct remote_light_context *ctx)
{
    struct sol_coap_packet *req;
    struct sol_network_link_addr cliaddr = { };

    req = sol_coap_packet_request_new(SOL_COAP_METHOD_GET, SOL_COAP_TYPE_CON);

    sol_coap_add_option(req, SOL_COAP_OPTION_URI_PATH, "a", sizeof("a") - 1);
    sol_coap_add_option(req, SOL_COAP_OPTION_URI_PATH, "light", sizeof("light") - 1);

    cliaddr.family = AF_INET6;
    sol_network_addr_from_str(&cliaddr, "ff05::fd");
    cliaddr.port = DEFAULT_UDP_PORT;

    sol_coap_send_packet_with_reply(ctx->server, req, &cliaddr, discover_reply_cb, ctx);

    return true;
}

static bool
setup_server(void)
{
    struct remote_light_context *ctx;

    ctx = calloc(1, sizeof(*ctx));
    SOL_NULL_CHECK(ctx, false);

    ctx->server = sol_coap_server_new(0);
    SOL_NULL_CHECK_GOTO(ctx->server, server_failed);

    ctx->button = setup_button(ctx);
    SOL_NULL_CHECK_GOTO(ctx->button, button_failed);

    ctx->led = setup_led();
    SOL_NULL_CHECK_GOTO(ctx->led, led_failed);

    discover_resource(ctx);

    return true;
led_failed:
    sol_gpio_close(ctx->button);
button_failed:
    sol_coap_server_unref(ctx->server);
server_failed:
    free(ctx);
    return false;
}

static void
show_interfaces(void)
{
    const struct sol_vector *links;

    links = sol_network_get_available_links();
    if (links) {
        const struct sol_network_link *l;
        uint16_t i;

        SOL_VECTOR_FOREACH_IDX(links, l, i) {
            uint16_t j;
            const struct sol_network_link_addr *addr;

            printf("Link #%d\n", i);
            SOL_VECTOR_FOREACH_IDX(&l->addrs, addr, j) {
                char buf[100];
                const char *ret;
                ret = sol_network_addr_to_str(addr, buf, sizeof(buf));
                if (ret)
                    printf("\tAddr #%d: %s\n", j, ret);
            }
        }
    }
}

int main(void)
{
    sol_init();

    show_interfaces();

    setup_server();

    sol_run();

    sol_shutdown();

    return 0;
}
