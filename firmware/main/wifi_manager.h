#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start the Wi-Fi manager.
 * 
 * This function will:
 * 1. Read saved credentials from NVS.
 * 2. Attempt to connect to the saved Wi-Fi network (Station mode).
 * 3. If it fails or no credentials exist, it starts a SoftAP and a Captive Portal.
 * 4. The Captive Portal runs an HTTP server and a DNS server to capture clients.
 * 5. Once new credentials are provided via the portal, it saves them and reboots the ESP32.
 * 
 * This function will block until a successful Station connection is made.
 */
void wifi_manager_start(void);

#ifdef __cplusplus
}
#endif
