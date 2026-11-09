#include "NcsiResponder.h"

#include <cstdint>
#include <cstring>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

const char *TAG = "NCSI";

constexpr uint16_t DNS_PORT = 53;
constexpr size_t DNS_MAX_PACKET = 512;                                  // plain UDP DNS limit
constexpr size_t DNS_HEADER_SIZE = 12;
constexpr size_t DNS_ANSWER_SIZE = 16;                                  // name pointer, type, class, TTL, length, IPv4 address
constexpr size_t DNS_MAX_NAME = 255;
constexpr uint16_t DNS_TYPE_A = 1;
constexpr uint16_t DNS_CLASS_IN = 1;
constexpr uint8_t DNS_TTL_S = 10;                                       // short, the PC leaves this network during tests
constexpr uint8_t RCODE_NOERROR = 0;
constexpr uint8_t RCODE_FORMERR = 1;
constexpr uint8_t RCODE_NXDOMAIN = 3;
constexpr uint8_t RCODE_NOTIMP = 4;

constexpr uint16_t HTTP_MAX_OPEN_SOCKETS = 3;                           // +3 internal, CONFIG_LWIP_MAX_SOCKETS is 10

struct ProbeHost {
    const char *name;
    bool ap_address;                                                    // true: answer with the AP address
    uint8_t address[4];
};

constexpr ProbeHost PROBE_HOSTS[] = {
    {"www.msftconnecttest.com", true, {}},                              // Windows 10/11 HTTP probe, served below
    {"www.msftncsi.com", true, {}},                                     // older Windows HTTP probe
    {"dns.msftncsi.com", false, {131, 107, 255, 255}},                  // Windows compares the answer with this address
};

uint8_t ap_address[4] = {192, 168, 4, 1};                               // replaced by the AP netif address at start

// Copy the question name as dotted text. Return the index after the name, 0 if malformed.
size_t read_question_name(const uint8_t *packet, size_t len, char *name) {
    size_t pos = DNS_HEADER_SIZE;
    size_t out = 0;
    while (true) {
        if (pos >= len)
            return 0;
        const uint8_t label = packet[pos++];
        if (label == 0)
            break;
        if (label > 63 || pos + label > len || out + label + 1 > DNS_MAX_NAME)   // no compression in a question
            return 0;
        if (out > 0)
            name[out++] = '.';
        memcpy(name + out, packet + pos, label);
        out += label;
        pos += label;
    }
    name[out] = '\0';
    return pos;
}

const ProbeHost *find_probe_host(const char *name) {
    for (const ProbeHost &host : PROBE_HOSTS) {
        if (strcasecmp(host.name, name) == 0)
            return &host;
    }
    return nullptr;
}

// Turn the query in packet into its response in place. Return response length, 0 to send nothing.
size_t build_dns_response(uint8_t *packet, size_t len) {
    if (len < DNS_HEADER_SIZE || (packet[2] & 0x80))                    // too short, or a response
        return 0;
    const uint8_t opcode = (packet[2] >> 3) & 0x0F;
    const uint16_t question_count = (packet[4] << 8) | packet[5];

    uint8_t rcode = RCODE_NOERROR;
    size_t end = DNS_HEADER_SIZE;                                       // response keeps the question, drops other sections
    const uint8_t *answer = nullptr;
    if (opcode != 0) {
        rcode = RCODE_NOTIMP;
    } else if (question_count != 1) {
        rcode = RCODE_FORMERR;
    } else {
        char name[DNS_MAX_NAME + 1];
        const size_t pos = read_question_name(packet, len, name);
        if (pos == 0 || pos + 4 > len) {
            rcode = RCODE_FORMERR;
        } else {
            end = pos + 4;
            const uint16_t type = (packet[pos] << 8) | packet[pos + 1];
            const uint16_t qclass = (packet[pos + 2] << 8) | packet[pos + 3];
            const ProbeHost *host = find_probe_host(name);
            if (host == nullptr)
                rcode = RCODE_NXDOMAIN;
            else if (type == DNS_TYPE_A && qclass == DNS_CLASS_IN)
                answer = host->ap_address ? ap_address : host->address;
            // other types of a probe name, e.g. AAAA: no error and no answer
        }
    }

    packet[2] = 0x80 | (opcode << 3) | 0x04 | (packet[2] & 0x01);       // response, authoritative, keep recursion desired
    packet[3] = rcode;
    packet[4] = 0;
    packet[5] = end > DNS_HEADER_SIZE ? 1 : 0;
    packet[6] = 0;
    packet[7] = answer != nullptr ? 1 : 0;
    memset(packet + 8, 0, 4);                                           // no authority or additional records
    if (answer != nullptr) {
        const uint8_t record[DNS_ANSWER_SIZE] = {
            0xC0, 0x0C,                                                 // name: pointer to the question name
            0x00, DNS_TYPE_A, 0x00, DNS_CLASS_IN,
            0x00, 0x00, 0x00, DNS_TTL_S,
            0x00, 0x04, answer[0], answer[1], answer[2], answer[3]};
        memcpy(packet + end, record, sizeof(record));                   // end <= 12 + 255 + 4, fits the buffer
        end += sizeof(record);
    }
    return end;
}

void dns_task(void *arg) {
    (void)arg;
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(nullptr);
    }
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(DNS_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(nullptr);
    }
    static uint8_t packet[DNS_MAX_PACKET + DNS_ANSWER_SIZE];
    while (true) {
        sockaddr_in peer = {};
        socklen_t peer_len = sizeof(peer);
        const int len = recvfrom(sock, packet, DNS_MAX_PACKET, 0, reinterpret_cast<sockaddr *>(&peer), &peer_len);
        if (len <= 0) {
            vTaskDelay(pdMS_TO_TICKS(100));                             // e.g. while wifi is stopped
            continue;
        }
        const size_t response_len = build_dns_response(packet, static_cast<size_t>(len));
        if (response_len > 0)
            sendto(sock, packet, response_len, 0, reinterpret_cast<sockaddr *>(&peer), peer_len);
    }
}

esp_err_t send_probe_text(httpd_req_t *req, const char *text) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_sendstr(req, text);
}

esp_err_t connecttest_handler(httpd_req_t *req) {
    return send_probe_text(req, "Microsoft Connect Test");
}

esp_err_t ncsi_handler(httpd_req_t *req) {
    return send_probe_text(req, "Microsoft NCSI");
}

}  // namespace

esp_err_t ncsi_responder_start() {
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip_info;
    if (ap_netif != nullptr && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        memcpy(ap_address, &ip_info.ip.addr, sizeof(ap_address));      // lwIP keeps addresses in network order
    }

    if (xTaskCreate(dns_task, "dns_task", 3072, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "DNS task not created");
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = HTTP_MAX_OPEN_SOCKETS;
    config.lru_purge_enable = true;                                     // a stale PC connection cannot block the probe
    httpd_handle_t server = nullptr;
    const esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uris[] = {
        {"/connecttest.txt", HTTP_GET, connecttest_handler, nullptr},
        {"/ncsi.txt", HTTP_GET, ncsi_handler, nullptr},
    };
    for (const httpd_uri_t &uri : uris)
        httpd_register_uri_handler(server, &uri);
    ESP_LOGI(TAG, "NCSI responder started");
    return ESP_OK;
}
