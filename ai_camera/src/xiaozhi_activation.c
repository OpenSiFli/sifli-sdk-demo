#include "xiaozhi_activation.h"

#include <rtthread.h>

#include <lwip/ip_addr.h>
#include <lwip/tcpip.h>
#include <mbedtls/sha256.h>
#include <netdev.h>
#include <webclient.h>

#include "bts2_bt.h"
#include "bf0_ble_common.h"
#include "bf0_sys_cfg.h"
#include "drv_flash.h"
#include "xiaozhi_activation_policy.h"
#include "xiaozhi_websocket.h"

extern BTS2S_ETHER_ADDR bt_pan_get_mac_address(void);

#define AI_XIAOZHI_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"
#define AI_XIAOZHI_HTTP_HEADER_SIZE 512U
#define AI_XIAOZHI_ERROR_PREVIEW_MAX 256U

/* Use the factory address when available, otherwise derive a stable address
 * from the chip UID instead of exposing the SDK's placeholder address. */
ble_common_update_type_t ble_request_public_address(bd_addr_t *addr)
{
    uint8_t factory_address[BD_ADDR_LEN] = {0};
    uint8_t read_length;
    int result;

    if (addr == RT_NULL)
        return BLE_UPDATE_NO_UPDATE;

    read_length = rt_flash_config_read(FACTORY_CFG_ID_MAC, factory_address,
                                       sizeof(factory_address));
    if (read_length == 0U)
    {
        result = bt_mac_addr_generate_via_uid_v2(addr);
        if (result != 0)
        {
            rt_kprintf("xiaozhi: UID MAC generation failed (%d)\n", result);
            return BLE_UPDATE_NO_UPDATE;
        }
        /* Keep the generated address unicast. */
        addr->addr[5] &= (uint8_t)~0x03U;
    }
    else if (read_length == sizeof(factory_address))
    {
        rt_memcpy(addr->addr, factory_address, sizeof(factory_address));
    }
    else
    {
        rt_kprintf("xiaozhi: invalid factory MAC length %u\n", read_length);
        return BLE_UPDATE_NO_UPDATE;
    }

    return BLE_UPDATE_ONCE;
}

static int ai_xiaozhi_network_ready(void)
{
    struct netdev *netdev = netdev_get_by_name("b0");

    return netdev != RT_NULL && netdev_is_up(netdev) &&
           netdev_is_link_up(netdev) && !ip_addr_isany(&netdev->ip_addr);
}

int ai_xiaozhi_get_identity(char *device_id, size_t device_id_size,
                            char *client_id, size_t client_id_size)
{
    BTS2S_ETHER_ADDR pan_address = bt_pan_get_mac_address();
    uint8_t raw_address[6];
    uint8_t digest[32];

    raw_address[0] = (uint8_t)(pan_address.w[0] & 0xffU);
    raw_address[1] = (uint8_t)(pan_address.w[0] >> 8U);
    raw_address[2] = (uint8_t)(pan_address.w[1] & 0xffU);
    raw_address[3] = (uint8_t)(pan_address.w[1] >> 8U);
    raw_address[4] = (uint8_t)(pan_address.w[2] & 0xffU);
    raw_address[5] = (uint8_t)(pan_address.w[2] >> 8U);
    if (device_id == RT_NULL || client_id == RT_NULL ||
        mbedtls_sha256_ret(raw_address, sizeof(raw_address), digest, 0) != 0 ||
        ai_xiaozhi_format_pan_device_id(device_id, device_id_size,
                                        raw_address) < 0 ||
        ai_xiaozhi_format_client_id(client_id, client_id_size, digest) < 0)
        return -1;
    return 0;
}

/* Last activation answer, kept so the sidebar panel can show the bind code. The
 * writer is the connect worker and the reader is the UI thread; the code is
 * written before the valid flag, so a torn read can only show the older code. */
static char s_bind_code[AI_XIAOZHI_ACTIVATION_CODE_MAX];
static volatile int s_bind_code_valid;

int ai_xiaozhi_activation_bind_code(char *buffer, size_t size)
{
    if (buffer == RT_NULL || size == 0U || !s_bind_code_valid)
        return 0;
    rt_strncpy(buffer, s_bind_code, size - 1U);
    buffer[size - 1U] = '\0';
    return s_bind_code[0] != '\0';
}

/* Pending activation means unbound: cache the code, or drop the old one when the
 * device is bound (or the answer carried no code). */
static void ai_xiaozhi_activation_remember(
    const ai_xiaozhi_activation_t *activation)
{
    const char *code = activation->activation_pending
                           ? activation->activation_code : "";

    s_bind_code_valid = 0;
    rt_strncpy(s_bind_code, code, sizeof(s_bind_code) - 1U);
    s_bind_code[sizeof(s_bind_code) - 1U] = '\0';
    s_bind_code_valid = 1;
}

int ai_xiaozhi_fetch_activation(ai_xiaozhi_activation_t *activation)
{
    char device_id[AI_XIAOZHI_DEVICE_ID_MAX];
    char client_id[AI_XIAOZHI_CLIENT_ID_MAX];
    char *request = RT_NULL;
    char *response = RT_NULL;
    char content_length_header[AI_XIAOZHI_CONTENT_LENGTH_HEADER_MAX];
    struct webclient_session *session = RT_NULL;
    size_t response_length = 0U;
    int request_length;
    int result = -1;

    if (activation == RT_NULL || !ai_xiaozhi_network_ready() ||
        ai_xiaozhi_get_identity(device_id, sizeof(device_id), client_id,
                                sizeof(client_id)) < 0)
        return -1;

    request = rt_malloc(AI_XIAOZHI_ACTIVATION_REQUEST_MAX);
    session = webclient_session_create(AI_XIAOZHI_HTTP_HEADER_SIZE);
    if (request == RT_NULL || session == RT_NULL)
        goto done;
    request_length = ai_xiaozhi_build_activation_request(
        request, AI_XIAOZHI_ACTIVATION_REQUEST_MAX, device_id, client_id);
    if (request_length < 0 ||
        webclient_header_fields_add(session, "Device-Id: %s\r\n", device_id) <=
            0 ||
        webclient_header_fields_add(session, "Client-Id: %s\r\n", client_id) <=
            0 ||
        webclient_header_fields_add(session, "Content-Type: application/json\r\n") <=
            0 ||
        ai_xiaozhi_format_content_length_header(
            content_length_header, sizeof(content_length_header),
            (size_t)request_length) < 0 ||
        webclient_header_fields_add(session, "%s", content_length_header) <=
            0)
        goto done;

    result = webclient_post(session, AI_XIAOZHI_OTA_URL, request,
                            (size_t)request_length);
    if (ai_xiaozhi_activation_http_status_ok(result) < 0)
    {
        rt_kprintf("xiaozhi: activation HTTP status %d\n", result);
        if (result > 0)
        {
            response_length = 0U;
            response = RT_NULL;
            if (webclient_response(session, (void **)&response,
                                   &response_length) > 0 &&
                response != RT_NULL)
            {
                char preview[AI_XIAOZHI_ERROR_PREVIEW_MAX];
                size_t preview_length = response_length;

                if (preview_length >= sizeof(preview))
                    preview_length = sizeof(preview) - 1U;
                rt_memcpy(preview, response, preview_length);
                preview[preview_length] = '\0';
                rt_kprintf("xiaozhi: activation response (%u bytes): %s\n",
                           (unsigned int)response_length, preview);
                web_free(response);
                response = RT_NULL;
            }
        }
        result = -1;
        goto done;
    }
    result = webclient_response(session, (void **)&response, &response_length);
    if (result <= 0 || response == RT_NULL)
    {
        rt_kprintf("xiaozhi: activation response empty\n");
        goto done;
    }
    result = ai_xiaozhi_parse_activation_response(response, activation);
    if (result < 0)
        rt_kprintf("xiaozhi: activation response schema invalid (%u bytes)\n",
                   (unsigned int)response_length);
    else
        ai_xiaozhi_activation_remember(activation);
done:
    if (response != RT_NULL)
        web_free(response);
    if (request != RT_NULL)
        rt_free(request);
    if (session != RT_NULL)
    {
        LOCK_TCPIP_CORE();
        webclient_close(session);
        UNLOCK_TCPIP_CORE();
    }
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
    if (ai_xiaozhi_activation_should_skip(
            ai_xiaozhi_websocket_is_active(),
            ai_xiaozhi_websocket_is_connecting()))
    {
        rt_kprintf("xiaozhi: activation skipped, WebSocket is active or connecting\n");
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
