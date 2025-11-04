/**
 * Copyright (C) 2025, Axis Communications AB, Lund, Sweden
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Camera Health Monitor ACAP Application
 *
 * This application collects camera health data including CPU usage, memory
 * consumption, network statistics, and device information, then sends it to
 * InfluxDB for monitoring and analysis.
 */

#include <axparameter.h>
#include <curl/curl.h>
#include <glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

// Configuration structure
typedef struct {
    char* influxdb_url;
    char* influxdb_org;
    char* influxdb_bucket;
    char* influxdb_token;
    int collection_interval;
    bool enable_sending;
} Config;

// Health metrics structure
typedef struct {
    double cpu_usage;
    unsigned long memory_total_kb;
    unsigned long memory_available_kb;
    unsigned long memory_used_kb;
    double memory_usage_percent;
    unsigned long network_rx_bytes;
    unsigned long network_tx_bytes;
    char* serial_number;
    char* product_full_name;
    char* firmware_version;
} HealthMetrics;

// Global variables
static GMainLoop* main_loop        = NULL;
static Config config               = {0};
static unsigned long prev_rx_bytes = 0;
static unsigned long prev_tx_bytes = 0;
static bool first_network_sample   = true;

// Static CPU usage tracking
static unsigned long long prev_idle  = 0;
static unsigned long long prev_total = 0;
static bool first_cpu_sample         = true;

// Function to log and exit on fatal error
static void panic(const char* msg) {
    syslog(LOG_ERR, "%s", msg);
    exit(EXIT_FAILURE);
}

// Read CPU usage from /proc/stat
static double read_cpu_usage(void) {
    FILE* fp = fopen("/proc/stat", "r");
    if (!fp) {
        syslog(LOG_WARNING, "Failed to open /proc/stat");
        return 0.0;
    }

    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    if (fscanf(fp,
               "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user,
               &nice,
               &system,
               &idle,
               &iowait,
               &irq,
               &softirq,
               &steal) != 8) {
        fclose(fp);
        syslog(LOG_WARNING, "Failed to parse /proc/stat");
        return 0.0;
    }
    fclose(fp);

    unsigned long long total      = user + nice + system + idle + iowait + irq + softirq + steal;
    unsigned long long total_idle = idle + iowait;

    if (first_cpu_sample) {
        prev_total       = total;
        prev_idle        = total_idle;
        first_cpu_sample = false;
        return 0.0;
    }

    unsigned long long diff_total = total - prev_total;
    unsigned long long diff_idle  = total_idle - prev_idle;

    prev_total = total;
    prev_idle  = total_idle;

    if (diff_total == 0) {
        return 0.0;
    }

    return (double)(diff_total - diff_idle) * 100.0 / diff_total;
}

// Read memory information from /proc/meminfo
static void read_memory_info(HealthMetrics* metrics) {
    FILE* fp = fopen("/proc/meminfo", "r");
    if (!fp) {
        syslog(LOG_WARNING, "Failed to open /proc/meminfo");
        return;
    }

    char line[256];
    unsigned long mem_total     = 0;
    unsigned long mem_free      = 0;
    unsigned long mem_available = 0;
    unsigned long buffers       = 0;
    unsigned long cached        = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %lu kB", &mem_total) == 1)
            continue;
        if (sscanf(line, "MemFree: %lu kB", &mem_free) == 1)
            continue;
        if (sscanf(line, "MemAvailable: %lu kB", &mem_available) == 1)
            continue;
        if (sscanf(line, "Buffers: %lu kB", &buffers) == 1)
            continue;
        if (sscanf(line, "Cached: %lu kB", &cached) == 1)
            continue;
    }
    fclose(fp);

    metrics->memory_total_kb = mem_total;
    if (mem_available > 0) {
        metrics->memory_available_kb = mem_available;
        metrics->memory_used_kb      = mem_total - mem_available;
    } else {
        // Fallback calculation if MemAvailable is not present
        metrics->memory_available_kb = mem_free + buffers + cached;
        metrics->memory_used_kb      = mem_total - metrics->memory_available_kb;
    }

    if (mem_total > 0) {
        metrics->memory_usage_percent = (double)metrics->memory_used_kb * 100.0 / mem_total;
    } else {
        metrics->memory_usage_percent = 0.0;
    }
}

// Read network statistics from /proc/net/dev
static void read_network_stats(HealthMetrics* metrics) {
    FILE* fp = fopen("/proc/net/dev", "r");
    if (!fp) {
        syslog(LOG_WARNING, "Failed to open /proc/net/dev");
        return;
    }

    char line[512];
    unsigned long rx_bytes = 0;
    unsigned long tx_bytes = 0;

    // Skip first two header lines
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return;
    }
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return;
    }

    // Sum up all network interfaces except loopback
    while (fgets(line, sizeof(line), fp)) {
        char iface[64];
        unsigned long rx, tx;
        if (sscanf(line, "%s %lu %*u %*u %*u %*u %*u %*u %*u %lu", iface, &rx, &tx) == 3) {
            // Skip loopback interface
            if (strncmp(iface, "lo:", 3) != 0) {
                rx_bytes += rx;
                tx_bytes += tx;
            }
        }
    }
    fclose(fp);

    if (first_network_sample) {
        prev_rx_bytes             = rx_bytes;
        prev_tx_bytes             = tx_bytes;
        first_network_sample      = false;
        metrics->network_rx_bytes = 0;
        metrics->network_tx_bytes = 0;
    } else {
        metrics->network_rx_bytes = rx_bytes - prev_rx_bytes;
        metrics->network_tx_bytes = tx_bytes - prev_tx_bytes;
        prev_rx_bytes             = rx_bytes;
        prev_tx_bytes             = tx_bytes;
    }
}

// Read device information from system parameters
static void read_device_info(HealthMetrics* metrics) {
    AXParameter* ax_param = ax_parameter_new("camera_health_monitor", NULL);
    if (!ax_param) {
        syslog(LOG_WARNING, "Failed to create AXParameter instance");
        return;
    }

    // Read serial number
    GError* error = NULL;
    gchar* serial = NULL;
    if (ax_parameter_get(ax_param, "Properties.System.SerialNumber", &serial, &error)) {
        metrics->serial_number = g_strdup(serial);
        g_free(serial);
    } else {
        syslog(LOG_WARNING, "Failed to read serial number: %s", error ? error->message : "unknown");
        metrics->serial_number = g_strdup("unknown");
        if (error)
            g_error_free(error);
    }

    // Read product full name
    error          = NULL;
    gchar* product = NULL;
    if (ax_parameter_get(ax_param, "Properties.System.ProductFullName", &product, &error)) {
        metrics->product_full_name = g_strdup(product);
        g_free(product);
    } else {
        syslog(LOG_WARNING, "Failed to read product name: %s", error ? error->message : "unknown");
        metrics->product_full_name = g_strdup("unknown");
        if (error)
            g_error_free(error);
    }

    // Read firmware version
    error           = NULL;
    gchar* firmware = NULL;
    if (ax_parameter_get(ax_param, "Properties.Firmware.Version", &firmware, &error)) {
        metrics->firmware_version = g_strdup(firmware);
        g_free(firmware);
    } else {
        syslog(LOG_WARNING,
               "Failed to read firmware version: %s",
               error ? error->message : "unknown");
        metrics->firmware_version = g_strdup("unknown");
        if (error)
            g_error_free(error);
    }

    ax_parameter_free(ax_param);
}

// Free health metrics memory
static void free_health_metrics(HealthMetrics* metrics) {
    if (metrics->serial_number) {
        g_free(metrics->serial_number);
        metrics->serial_number = NULL;
    }
    if (metrics->product_full_name) {
        g_free(metrics->product_full_name);
        metrics->product_full_name = NULL;
    }
    if (metrics->firmware_version) {
        g_free(metrics->firmware_version);
        metrics->firmware_version = NULL;
    }
}

// Escape special characters for InfluxDB tag values
static char* escape_influxdb_tag(const char* str) {
    if (!str)
        return g_strdup("unknown");

    // Escape spaces, commas, and equals signs
    GString* escaped = g_string_new("");
    for (const char* p = str; *p; p++) {
        if (*p == ' ' || *p == ',' || *p == '=') {
            g_string_append_c(escaped, '\\');
        }
        g_string_append_c(escaped, *p);
    }
    return g_string_free(escaped, FALSE);
}

// Format metrics in InfluxDB line protocol
static char* format_influxdb_line(HealthMetrics* metrics) {
    time_t now = time(NULL);

    // Escape tag values
    char* serial_escaped   = escape_influxdb_tag(metrics->serial_number);
    char* product_escaped  = escape_influxdb_tag(metrics->product_full_name);
    char* firmware_escaped = escape_influxdb_tag(metrics->firmware_version);

    char* line = g_strdup_printf(
        "camera_health,serial=%s,product=%s,firmware=%s "
        "cpu_usage=%.2f,memory_total=%lu,memory_used=%lu,memory_available=%lu,"
        "memory_usage_percent=%.2f,network_rx_bytes=%lu,network_tx_bytes=%lu %ld000000000",
        serial_escaped,
        product_escaped,
        firmware_escaped,
        metrics->cpu_usage,
        metrics->memory_total_kb,
        metrics->memory_used_kb,
        metrics->memory_available_kb,
        metrics->memory_usage_percent,
        metrics->network_rx_bytes,
        metrics->network_tx_bytes,
        now);

    g_free(serial_escaped);
    g_free(product_escaped);
    g_free(firmware_escaped);

    return line;
}

// cURL write callback (we don't need the response data)
static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    (void)contents;
    (void)userp;
    return size * nmemb;
}

// Send metrics to InfluxDB
static void send_to_influxdb(const char* data) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        syslog(LOG_ERR, "Failed to initialize curl");
        return;
    }

    // URL encode organization and bucket names
    char* org_escaped    = curl_easy_escape(curl, config.influxdb_org, 0);
    char* bucket_escaped = curl_easy_escape(curl, config.influxdb_bucket, 0);

    if (!org_escaped || !bucket_escaped) {
        syslog(LOG_ERR, "Failed to URL encode parameters");
        curl_free(org_escaped);
        curl_free(bucket_escaped);
        curl_easy_cleanup(curl);
        return;
    }

    // Construct the write URL with escaped parameters
    char* url = g_strdup_printf("%s/api/v2/write?org=%s&bucket=%s",
                                config.influxdb_url,
                                org_escaped,
                                bucket_escaped);

    curl_free(org_escaped);
    curl_free(bucket_escaped);

    // Set up request headers with dynamic allocation for auth header
    struct curl_slist* headers = NULL;
    char* auth_header          = g_strdup_printf("Authorization: Token %s", config.influxdb_token);
    headers                    = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: text/plain; charset=utf-8");

    // Configure curl
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    // Perform the request
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        syslog(LOG_ERR, "Failed to send data to InfluxDB: %s", curl_easy_strerror(res));
    } else {
        long response_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        if (response_code >= 200 && response_code < 300) {
            syslog(LOG_INFO, "Successfully sent metrics to InfluxDB (HTTP %ld)", response_code);
        } else {
            syslog(LOG_ERR, "InfluxDB returned error code: %ld", response_code);
        }
    }

    g_free(url);
    g_free(auth_header);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

// Collect and send health metrics
static gboolean collect_and_send_metrics(gpointer user_data) {
    (void)user_data;

    HealthMetrics metrics = {0};

    // Collect all metrics
    metrics.cpu_usage = read_cpu_usage();
    read_memory_info(&metrics);
    read_network_stats(&metrics);
    read_device_info(&metrics);

    // Log collected metrics
    syslog(LOG_INFO,
           "Metrics: CPU=%.2f%%, Memory=%lu/%lu KB (%.2f%%), "
           "Network RX=%lu bytes, TX=%lu bytes",
           metrics.cpu_usage,
           metrics.memory_used_kb,
           metrics.memory_total_kb,
           metrics.memory_usage_percent,
           metrics.network_rx_bytes,
           metrics.network_tx_bytes);

    // Send to InfluxDB if enabled
    if (config.enable_sending) {
        char* line = format_influxdb_line(&metrics);
        send_to_influxdb(line);
        g_free(line);
    } else {
        syslog(LOG_INFO, "Sending to InfluxDB is disabled");
    }

    free_health_metrics(&metrics);

    return G_SOURCE_CONTINUE;
}

// Load configuration from parameters
static void load_config(void) {
    AXParameter* ax_param = ax_parameter_new("camera_health_monitor", NULL);
    if (!ax_param) {
        panic("Failed to create AXParameter instance");
    }

    GError* error = NULL;
    gchar* value  = NULL;

    // Load InfluxDB URL
    if (ax_parameter_get(ax_param, "root.CameraHealthMonitor.InfluxDBURL", &value, &error)) {
        config.influxdb_url = g_strdup(value);
        g_free(value);
    } else {
        config.influxdb_url = g_strdup("http://localhost:8086");
    }
    if (error) {
        g_error_free(error);
        error = NULL;
    }

    // Load InfluxDB organization
    value = NULL;
    if (ax_parameter_get(ax_param, "root.CameraHealthMonitor.InfluxDBOrg", &value, &error)) {
        config.influxdb_org = g_strdup(value);
        g_free(value);
    } else {
        config.influxdb_org = g_strdup("myorg");
    }
    if (error) {
        g_error_free(error);
        error = NULL;
    }

    // Load InfluxDB bucket
    value = NULL;
    if (ax_parameter_get(ax_param, "root.CameraHealthMonitor.InfluxDBBucket", &value, &error)) {
        config.influxdb_bucket = g_strdup(value);
        g_free(value);
    } else {
        config.influxdb_bucket = g_strdup("camera_health");
    }
    if (error) {
        g_error_free(error);
        error = NULL;
    }

    // Load InfluxDB token
    value = NULL;
    if (ax_parameter_get(ax_param, "root.CameraHealthMonitor.InfluxDBToken", &value, &error)) {
        config.influxdb_token = g_strdup(value);
        g_free(value);
    } else {
        config.influxdb_token = g_strdup("");
    }
    if (error) {
        g_error_free(error);
        error = NULL;
    }

    // Load collection interval
    value = NULL;
    if (ax_parameter_get(ax_param, "root.CameraHealthMonitor.CollectionInterval", &value, &error)) {
        char* endptr;
        long interval = strtol(value, &endptr, 10);
        // Validate: must be a valid number and within bounds
        if (*endptr == '\0' && interval >= 10 && interval <= 3600) {
            config.collection_interval = (int)interval;
        } else {
            syslog(LOG_WARNING,
                   "Invalid collection interval '%s', using default 60 seconds",
                   value);
            config.collection_interval = 60;
        }
        g_free(value);
    } else {
        config.collection_interval = 60;
    }
    if (error) {
        g_error_free(error);
        error = NULL;
    }

    // Load enable sending flag
    value = NULL;
    if (ax_parameter_get(ax_param, "root.CameraHealthMonitor.EnableSending", &value, &error)) {
        config.enable_sending = (g_strcmp0(value, "yes") == 0);
        g_free(value);
    } else {
        config.enable_sending = false;
    }
    if (error) {
        g_error_free(error);
        error = NULL;
    }

    ax_parameter_free(ax_param);

    syslog(LOG_INFO,
           "Configuration loaded: URL=%s, Org=%s, Bucket=%s, Interval=%d, Enabled=%s",
           config.influxdb_url,
           config.influxdb_org,
           config.influxdb_bucket,
           config.collection_interval,
           config.enable_sending ? "yes" : "no");
}

// Free configuration
static void free_config(void) {
    g_free(config.influxdb_url);
    g_free(config.influxdb_org);
    g_free(config.influxdb_bucket);
    g_free(config.influxdb_token);
}

// Signal handler
static void signal_handler(int signum) {
    syslog(LOG_INFO, "Received signal %d, shutting down", signum);
    if (main_loop) {
        g_main_loop_quit(main_loop);
    }
}

int main(void) {
    openlog("camera_health_monitor", LOG_PID, LOG_USER);
    syslog(LOG_INFO, "Camera Health Monitor starting...");

    // Initialize curl globally
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Load configuration
    load_config();

    // Set up signal handlers
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    // Create main loop
    main_loop = g_main_loop_new(NULL, FALSE);

    // Schedule periodic metric collection
    g_timeout_add_seconds(config.collection_interval, collect_and_send_metrics, NULL);

    // Collect metrics once immediately
    collect_and_send_metrics(NULL);

    syslog(LOG_INFO,
           "Camera Health Monitor running, collecting metrics every %d seconds",
           config.collection_interval);

    // Run main loop
    g_main_loop_run(main_loop);

    // Cleanup
    syslog(LOG_INFO, "Camera Health Monitor shutting down");
    g_main_loop_unref(main_loop);
    free_config();
    curl_global_cleanup();
    closelog();

    return EXIT_SUCCESS;
}
