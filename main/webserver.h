#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_http_server.h"
#include "esp_err.h"

// Global webserver handle
extern httpd_handle_t server;

// Start/stop webserver
esp_err_t start_webserver(void);
void stop_webserver(void);

#ifdef __cplusplus
}
#endif
