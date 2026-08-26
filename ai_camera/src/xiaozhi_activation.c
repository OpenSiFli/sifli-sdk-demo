#include "xiaozhi_activation.h"

#include <rtthread.h>

#include <lwip/ip_addr.h>
#include <mbedtls/sha256.h>
#include <netdev.h>
#include <webclient.h>

#include "bf0_ble_common.h"

#define AI_XIAOZHI_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"

static int ai_xiaozhi_network_ready(void)
{
    struct netdev *netdev = netdev_get_by_name("b0");

    return netdev != RT_NULL && netdev_is_up(netdev) &&
           netdev_is_link_up(netdev) && !ip_addr_isany(&netdev->ip_addr);
}

int ai_xiaozhi_get_identity(char *device_id, size_t device_id_size,
                            char *client_id, size_t client_id_size)
{
    bd_addr_t address = {0};
    uint8_t digest[32];

    if (device_id == RT_NULL || client_id == RT_NULL ||
        ble_get_public_address(&address) != 0U ||
        mbedtls_sha256_ret(address.addr, sizeof(address.addr), digest, 0) != 0 ||
        ai_xiaozhi_format_device_id(device_id, device_id_size, address.addr) <
            0 ||
        ai_xiaozhi_format_client_id(client_id, client_id_size, digest) < 0)
        return -1;
    return 0;
}

int ai_xiaozhi_fetch_activation(ai_xiaozhi_activation_t *activation)
{
    char device_id[AI_XIAOZHI_DEVICE_ID_MAX];
    char client_id[AI_XIAOZHI_CLIENT_ID_MAX];
    char request[AI_XIAOZHI_ACTIVATION_REQUEST_MAX];
    char headers[128];
    char *response = RT_NULL;
    size_t response_length = 0U;
    int request_length;
    int header_length;
    int result;

    if (activation == RT_NULL || !ai_xiaozhi_network_ready() ||
        ai_xiaozhi_get_identity(device_id, sizeof(device_id), client_id,
                                sizeof(client_id)) < 0)
        return -1;

    request_length = ai_xiaozhi_build_activation_request(
        request, sizeof(request), device_id, client_id);
    header_length = rt_snprintf(headers, sizeof(headers),
                                "Device-Id: %s\r\nClient-Id: %s\r\n"
                                "Content-Type: application/json\r\n",
                                device_id, client_id);
    if (request_length < 0 || header_length < 0 ||
        (size_t)header_length >= sizeof(headers))
        return -1;

    result = webclient_request(AI_XIAOZHI_OTA_URL, headers, request,
                               (size_t)request_length, (void **)&response,
                               &response_length);
    if (result <= 0 || response == RT_NULL || response_length == 0U)
    {
        if (response != RT_NULL)
            web_free(response);
        return -1;
    }
    result = ai_xiaozhi_parse_activation_response(response, activation);
    web_free(response);
    return result;
}

static void ai_xiaozhi_activation(int argc, char **argv)
{
    ai_xiaozhi_activation_t activation;

    (void)argv;
    if (argc != 1)
    {
        rt_kprintf("usage: ai_xiaozhi_activation\n");
        return;
    }
    if (ai_xiaozhi_fetch_activation(&activation) < 0)
    {
        rt_kprintf("xiaozhi: activation request failed\n");
        return;
    }
    if (activation.activation_pending)
    {
        rt_kprintf("xiaozhi: bind device with code %s\n",
                   activation.activation_code);
        return;
    }
    rt_kprintf("xiaozhi: activation ready %s:%u%s\n", activation.endpoint.host,
               activation.endpoint.port, activation.endpoint.path);
}
MSH_CMD_EXPORT(ai_xiaozhi_activation, request XiaoZhi activation data);
