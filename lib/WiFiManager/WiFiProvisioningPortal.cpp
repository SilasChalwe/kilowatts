#include "WiFiProvisioningPortal.h"

#include "WiFiCredentialsStore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#endif

namespace kilowatts {

namespace {

#ifdef ESP_PLATFORM
static const char* TAG = "WIFI_PROVISIONING";
#endif

constexpr std::uint16_t DNS_PORT = 53U;
constexpr std::uint16_t HTTP_PORT = 80U;

#ifdef ESP_PLATFORM

esp_netif_t* apNetif = nullptr;
httpd_handle_t httpServer = nullptr;
TaskHandle_t dnsTaskHandle = nullptr;
esp_timer_handle_t restartTimer = nullptr;
volatile bool dnsTaskShouldRun = false;
std::uint32_t portalIpAddress = 0U;   // network byte order, from esp_netif_get_ip_info()

const char PAGE_TOP[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Kilowatts Setup</title></head><body style='font-family:sans-serif;max-width:420px;margin:2em auto;padding:0 1em'>"
    "<h2>Kilowatts Central - Wi-Fi Setup</h2>";

const char FORM_BODY[] =
    "<p>Enter the household/installation Wi-Fi this Central Node should join.</p>"
    "<form method='POST' action='/save'>"
    "<label>Network name (SSID)</label><br>"
    "<input name='ssid' maxlength='32' required style='width:100%;padding:.5em;margin:.3em 0 1em'><br>"
    "<label>Password</label><br>"
    "<input name='password' type='password' maxlength='64' style='width:100%;padding:.5em;margin:.3em 0 1em'><br>"
    "<button type='submit' style='padding:.7em 1.5em'>Save and Connect</button>"
    "</form></body></html>";

const char PAGE_SAVED[] =
    "Saved. Central is restarting and will attempt to join that network - "
    "reconnect your phone to your normal Wi-Fi to continue in the Kilowatts app.</p></body></html>";

const char PAGE_REJECTED[] =
    "That SSID could not be saved (empty, or the password is too short/long).</p></body></html>";


void scheduleRestart()
{
    if (restartTimer == nullptr) {
        const esp_timer_create_args_t timerArgs = {
            [](void*) { esp_restart(); },
            nullptr,
            ESP_TIMER_TASK,
            "kw_wifi_prov_restart",
            true
        };
        esp_timer_create(&timerArgs, &restartTimer);
    }
    if (restartTimer != nullptr) {
        esp_timer_start_once(restartTimer, 1500000ULL);   // 1.5s - let the HTTP response flush first
    }
}


esp_err_t handleGetRoot(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_TOP);
    httpd_resp_sendstr_chunk(req, FORM_BODY);
    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_OK;
}


/* Every unmatched path (captive-portal probe URLs like /generate_204,
 * /hotspot-detect.html, connectivitycheck.gstatic.com's probe path, ...)
 * is redirected to '/' so the OS's captive-portal detector opens the real
 * setup page instead of reporting "no internet, ignore". */
esp_err_t handleCaptiveProbe(httpd_req_t* req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}


void urlDecodeInPlace(char* text)
{
    char* out = text;
    for (char* in = text; *in != '\0'; ++in) {
        if (*in == '+') {
            *out++ = ' ';
        } else if (*in == '%' && in[1] != '\0' && in[2] != '\0') {
            char hex[3] = {in[1], in[2], '\0'};
            *out++ = static_cast<char>(std::strtol(hex, nullptr, 16));
            in += 2;
        } else {
            *out++ = *in;
        }
    }
    *out = '\0';
}


bool extractFormField(const char* body, const char* key, char* out, std::size_t outSize)
{
    const char* cursor = std::strstr(body, key);
    if (cursor == nullptr) {
        return false;
    }
    cursor += std::strlen(key);
    std::size_t written = 0U;
    while (*cursor != '\0' && *cursor != '&' && written + 1U < outSize) {
        out[written++] = *cursor++;
    }
    out[written] = '\0';
    urlDecodeInPlace(out);
    return true;
}


esp_err_t handlePostSave(httpd_req_t* req)
{
    char body[256] = {};
    const int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[WiFiCredentialsStore::SSID_BUFFER_SIZE] = {};
    char password[WiFiCredentialsStore::PASSWORD_BUFFER_SIZE] = {};
    extractFormField(body, "ssid=", ssid, sizeof(ssid));
    extractFormField(body, "password=", password, sizeof(password));

    const WiFiCredentialsStore store;
    const bool saved = store.save(ssid, password);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_TOP);
    httpd_resp_sendstr_chunk(req, saved ? PAGE_SAVED : PAGE_REJECTED);
    httpd_resp_sendstr_chunk(req, nullptr);

    if (saved) {
        ESP_LOGI(TAG, "Wi-Fi credentials provisioned for SSID '%s'; restarting shortly", ssid);
        scheduleRestart();
    } else {
        ESP_LOGW(TAG, "Rejected provisioning submission (invalid SSID/password length)");
    }
    return ESP_OK;
}


/**
 * Minimal captive-portal DNS responder: answers every A-record query with
 * the portal's own IP so a connected phone's OS considers this network's
 * "internet check" URL reachable there and auto-opens the setup page,
 * rather than being manually navigated to by the installer.
 */
void dnsResponderTask(void*)
{
    const int sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS responder socket() failed");
        dnsTaskHandle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    sockaddr_in bindAddress{};
    bindAddress.sin_family = AF_INET;
    bindAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddress.sin_port = htons(DNS_PORT);
    if (lwip_bind(sock, reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress)) != 0) {
        ESP_LOGE(TAG, "DNS responder bind() failed");
        lwip_close(sock);
        dnsTaskHandle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    timeval receiveTimeout{1, 0};
    lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &receiveTimeout, sizeof(receiveTimeout));

    std::uint8_t packet[512];
    while (dnsTaskShouldRun) {
        sockaddr_in clientAddress{};
        socklen_t clientAddressLength = sizeof(clientAddress);
        const int queryLength = lwip_recvfrom(sock, packet, sizeof(packet), 0,
                                               reinterpret_cast<sockaddr*>(&clientAddress), &clientAddressLength);
        if (queryLength < 12) {
            continue;   // timeout, or shorter than a DNS header - nothing to answer
        }

        /* Flip QR=1 (response), keep OPCODE/RD from the query, RCODE=0 (no error). */
        packet[2] = static_cast<std::uint8_t>(0x80U | (packet[2] & 0x79U));
        packet[3] = 0x80U;
        packet[6] = 0U; packet[7] = 1U;    // ANCOUNT = 1
        packet[8] = 0U; packet[9] = 0U;    // NSCOUNT = 0
        packet[10] = 0U; packet[11] = 0U;  // ARCOUNT = 0

        std::size_t responseLength = static_cast<std::size_t>(queryLength);
        if (responseLength + 16U > sizeof(packet)) {
            continue;
        }

        packet[responseLength++] = 0xC0U; packet[responseLength++] = 0x0CU;  // name = pointer to question
        packet[responseLength++] = 0x00U; packet[responseLength++] = 0x01U;  // TYPE = A
        packet[responseLength++] = 0x00U; packet[responseLength++] = 0x01U;  // CLASS = IN
        packet[responseLength++] = 0x00U; packet[responseLength++] = 0x00U;
        packet[responseLength++] = 0x00U; packet[responseLength++] = 0x3CU;  // TTL = 60s
        packet[responseLength++] = 0x00U; packet[responseLength++] = 0x04U;  // RDLENGTH = 4
        std::memcpy(&packet[responseLength], &portalIpAddress, 4U);
        responseLength += 4U;

        lwip_sendto(sock, packet, responseLength, 0,
                    reinterpret_cast<sockaddr*>(&clientAddress), clientAddressLength);
    }

    lwip_close(sock);
    dnsTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

#endif // ESP_PLATFORM

} // namespace


WiFiProvisioningPortal::WiFiProvisioningPortal() : active_(false) {}

WiFiProvisioningPortal::~WiFiProvisioningPortal()
{
    end();
}


bool WiFiProvisioningPortal::begin(std::uint8_t channel)
{
#ifdef ESP_PLATFORM
    if (active_) {
        return true;
    }

    if (apNetif == nullptr) {
        apNetif = esp_netif_create_default_wifi_ap();
        if (apNetif == nullptr) {
            ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap() failed");
            return false;
        }
    }

    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) failed");
        return false;
    }

    std::uint8_t macAddress[6] = {};
    esp_wifi_get_mac(WIFI_IF_AP, macAddress);

    wifi_config_t apConfig{};
    std::snprintf(reinterpret_cast<char*>(apConfig.ap.ssid), sizeof(apConfig.ap.ssid),
                   "Kilowatts-Setup-%02X%02X", macAddress[4], macAddress[5]);
    apConfig.ap.ssid_len = static_cast<std::uint8_t>(std::strlen(reinterpret_cast<char*>(apConfig.ap.ssid)));
    apConfig.ap.channel = channel;
    apConfig.ap.authmode = WIFI_AUTH_OPEN;
    apConfig.ap.max_connection = 4U;
    apConfig.ap.ssid_hidden = 0U;

    if (esp_wifi_set_config(WIFI_IF_AP, &apConfig) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed");
        return false;
    }

    esp_netif_ip_info_t ipInfo{};
    esp_netif_get_ip_info(apNetif, &ipInfo);
    portalIpAddress = ipInfo.ip.addr;

    httpd_config_t httpConfig = HTTPD_DEFAULT_CONFIG();
    httpConfig.server_port = HTTP_PORT;
    httpConfig.max_uri_handlers = 3;
    httpConfig.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&httpServer, &httpConfig) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start() failed");
        return false;
    }

    const httpd_uri_t rootUri = {.uri = "/", .method = HTTP_GET, .handler = handleGetRoot, .user_ctx = nullptr};
    const httpd_uri_t saveUri = {.uri = "/save", .method = HTTP_POST, .handler = handlePostSave, .user_ctx = nullptr};
    const httpd_uri_t catchAllUri = {.uri = "/*", .method = HTTP_GET, .handler = handleCaptiveProbe, .user_ctx = nullptr};
    httpd_register_uri_handler(httpServer, &rootUri);
    httpd_register_uri_handler(httpServer, &saveUri);
    httpd_register_uri_handler(httpServer, &catchAllUri);

    dnsTaskShouldRun = true;
    if (xTaskCreate(dnsResponderTask, "wifi_prov_dns", 3072U, nullptr, 3U, &dnsTaskHandle) != pdPASS) {
        ESP_LOGE(TAG, "Could not start captive-portal DNS responder task");
    }

    char ssidText[33] = {};
    std::snprintf(ssidText, sizeof(ssidText), "Kilowatts-Setup-%02X%02X", macAddress[4], macAddress[5]);
    ESP_LOGI(TAG, "Provisioning portal active: connect to Wi-Fi '%s' (open) and open http://" IPSTR "/",
             ssidText, IP2STR(&ipInfo.ip));

    active_ = true;
    return true;
#else
    (void)channel;
    return false;
#endif
}


void WiFiProvisioningPortal::end()
{
#ifdef ESP_PLATFORM
    if (!active_) {
        return;
    }

    dnsTaskShouldRun = false;
    for (int waited = 0; dnsTaskHandle != nullptr && waited < 20; ++waited) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (httpServer != nullptr) {
        httpd_stop(httpServer);
        httpServer = nullptr;
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    active_ = false;
#endif
}


bool WiFiProvisioningPortal::isActive() const
{
    return active_;
}


} // namespace kilowatts
