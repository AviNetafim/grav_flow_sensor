/* Answers the Windows network connectivity probes (NCSI), so Windows reports the AP as
 * having internet and does not leave it for another network with internet.
 *
 * DNS on port 53: the probe host names get their expected addresses, every other name gets
 *                 NXDOMAIN, so other PC traffic does not reach the server.
 * HTTP on port 80: /connecttest.txt and /ncsi.txt return the text Windows expects.
 *
 * The AP DHCP server gives the AP address as DNS server (CONFIG_LWIP_DHCPS_ADD_DNS).
 * Sockets are bound to all addresses, so they keep working when wifi is stopped and started.
 */

#ifndef NCSI_RESPONDER_h
#define NCSI_RESPONDER_h

#include "esp_err.h"

// Call after the AP netif is created. Uses 7 lwIP sockets (DNS 1, HTTP 6).
esp_err_t ncsi_responder_start();

#endif
