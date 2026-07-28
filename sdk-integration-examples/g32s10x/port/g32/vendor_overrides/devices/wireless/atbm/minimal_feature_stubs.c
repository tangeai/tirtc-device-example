struct cmd_arg;
struct netif;

void ble_msg_func(unsigned char *buffer)
{
    (void)buffer;
}

void ble_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
}

void wifi_test_help(void)
{
}

void atbm_wifi_test(struct cmd_arg *arg, int argc, char **argv)
{
    (void)arg;
    (void)argc;
    (void)argv;
}

void dhcpd_start(struct netif *netif)
{
    (void)netif;
}

void dhcpd_stop(struct netif *netif)
{
    (void)netif;
}
