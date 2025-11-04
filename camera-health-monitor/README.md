*Copyright (C) 2025, Axis Communications AB, Lund, Sweden. All Rights Reserved.*

# Camera Health Monitor ACAP Application

This ACAP application collects comprehensive camera health data and sends it to InfluxDB for monitoring and analysis. The application periodically gathers system metrics including CPU usage, memory consumption, network statistics, and device information.

## Features

The Camera Health Monitor collects the following metrics:

- **CPU Usage**: Real-time CPU utilization percentage
- **Memory Information**: Total, used, and available memory in KB, plus usage percentage
- **Network Statistics**: Bytes received and transmitted (delta since last sample)
- **Device Information**: Serial number, product name, and firmware version

All metrics are sent to InfluxDB using the InfluxDB v2 line protocol format.

## Configuration

The application can be configured through the camera's web interface after installation. The following parameters are available:

| Parameter | Description | Default |
|-----------|-------------|---------|
| **InfluxDB URL** | URL of the InfluxDB server | `http://localhost:8086` |
| **InfluxDB Organization** | InfluxDB organization name | `myorg` |
| **InfluxDB Bucket** | InfluxDB bucket to store metrics | `camera_health` |
| **InfluxDB Token** | Authentication token (hidden field) | *(empty)* |
| **Collection Interval** | How often to collect metrics (seconds) | `60` |
| **Enable Sending** | Enable/disable sending to InfluxDB | `no` |

### Configuring InfluxDB Destination

To modify the InfluxDB destination:

1. Navigate to `http://<AXIS_DEVICE_IP>/index.html#apps`
2. Find the **Camera Health Monitor** application
3. Click the **Settings** button
4. Update the following fields:
   - **InfluxDB URL**: Your InfluxDB server URL (e.g., `http://influxdb.example.com:8086`)
   - **InfluxDB Organization**: Your organization name
   - **InfluxDB Bucket**: Your bucket name
   - **InfluxDB Token**: Your authentication token (click "Show" to enter)
5. Set **Enable Sending to InfluxDB** to `yes`
6. Adjust **Collection Interval** as needed (10-3600 seconds)
7. Click **Save**

### Setting Up InfluxDB

Before using this application, you need an InfluxDB instance. Here's a quick setup guide:

#### Using InfluxDB Cloud

1. Sign up at [InfluxDB Cloud](https://cloud2.influxdata.com/)
2. Create a new bucket (e.g., `camera_health`)
3. Generate an API token with write permissions
4. Note your organization name and bucket name

#### Using InfluxDB OSS (Self-hosted)

```bash
# Using Docker
docker run -d -p 8086:8086 \
  -v influxdb-data:/var/lib/influxdb2 \
  -v influxdb-config:/etc/influxdb2 \
  influxdb:2.7

# Access the UI at http://localhost:8086
# Complete the setup wizard to create:
#   - Initial user
#   - Organization
#   - Bucket
#   - API token
```

## Getting Started

### Project Structure

```sh
camera-health-monitor
├── app
│   ├── camera_health_monitor.c
│   ├── LICENSE
│   ├── Makefile
│   └── manifest.json
├── Dockerfile
└── README.md
```

- **app/camera_health_monitor.c** - Main application source code
- **app/LICENSE** - Open source license information
- **app/Makefile** - Build instructions
- **app/manifest.json** - Application configuration and parameters
- **Dockerfile** - Container for building the application
- **README.md** - This documentation

## How to Build and Install

### Build the Application

Standing in your working directory, run:

> [!NOTE]
>
> Depending on the network your local build machine is connected to, you may need to add proxy
> settings for Docker. See
> [Proxy in build time](https://developer.axis.com/acap/develop/proxy/#proxy-in-build-time).

```sh
docker build --tag <APP_IMAGE> --build-arg ARCH=<ARCH> .
```

- `<APP_IMAGE>` is the name to tag the image with, e.g., `camera_health_monitor:1.0`
- `<ARCH>` is the SDK architecture, `armv7hf` or `aarch64`

Example:
```sh
docker build --tag camera_health_monitor:1.0 --build-arg ARCH=aarch64 .
```

Copy the result from the container image to a local directory `build`:

```sh
docker cp $(docker create camera_health_monitor:1.0):/opt/app ./build
```

The `build` directory will contain the ACAP application package:
- `camera_health_monitor_1_0_0_aarch64.eap` (for aarch64)
- `camera_health_monitor_1_0_0_armv7hf.eap` (for armv7hf)

### Install the Application

1. Browse to your Axis device's application page:
   ```
   http://<AXIS_DEVICE_IP>/index.html#apps
   ```

2. Click on the **Apps** tab in the device GUI
3. Enable **Allow unsigned apps** toggle
4. Click **(+ Add app)** button to upload the application
5. Browse to the `.eap` file matching your device architecture
6. Click **Install**
7. Configure the application settings (see Configuration section above)
8. Enable the **Start** switch to run the application

## Usage

### Viewing Application Logs

Application logs can be accessed at:

```
http://<AXIS_DEVICE_IP>/axis-cgi/admin/systemlog.cgi?appname=camera_health_monitor
```

Or through the Apps page by clicking **App log**.

### Expected Log Output

```
[ INFO    ] camera_health_monitor[1234]: Camera Health Monitor starting...
[ INFO    ] camera_health_monitor[1234]: Configuration loaded: URL=http://influxdb.example.com:8086, Org=myorg, Bucket=camera_health, Interval=60, Enabled=yes
[ INFO    ] camera_health_monitor[1234]: Metrics: CPU=15.23%, Memory=45678/98304 KB (46.45%), Network RX=1024 bytes, TX=2048 bytes
[ INFO    ] camera_health_monitor[1234]: Successfully sent metrics to InfluxDB (HTTP 204)
[ INFO    ] camera_health_monitor[1234]: Camera Health Monitor running, collecting metrics every 60 seconds
```

### Querying Data in InfluxDB

Once data is being sent, you can query it using Flux or InfluxQL:

#### Flux Query Example

```flux
from(bucket: "camera_health")
  |> range(start: -1h)
  |> filter(fn: (r) => r._measurement == "camera_health")
  |> filter(fn: (r) => r._field == "cpu_usage" or r._field == "memory_usage_percent")
```

#### Creating Dashboards

Use InfluxDB's built-in visualization tools or Grafana to create dashboards:

1. **In InfluxDB UI**:
   - Navigate to Dashboards
   - Create a new dashboard
   - Add cells with queries for CPU, memory, and network metrics

2. **With Grafana**:
   - Add InfluxDB as a data source
   - Create panels using Flux queries
   - Set up alerts based on thresholds

## Data Format

Metrics are sent in InfluxDB line protocol format:

```
camera_health,serial=<SERIAL>,product=<PRODUCT>,firmware=<VERSION> cpu_usage=<VALUE>,memory_total=<VALUE>,memory_used=<VALUE>,memory_available=<VALUE>,memory_usage_percent=<VALUE>,network_rx_bytes=<VALUE>,network_tx_bytes=<VALUE> <TIMESTAMP>
```

### Example Data Point

```
camera_health,serial=ACCC12345678,product=AXIS_P1375,firmware=11.6.67 cpu_usage=15.23,memory_total=98304,memory_used=45678,memory_available=52626,memory_usage_percent=46.45,network_rx_bytes=1024,network_tx_bytes=2048 1704067200000000000
```

## Troubleshooting

### Application Not Sending Data

1. **Check Enable Sending**: Ensure "Enable Sending to InfluxDB" is set to `yes`
2. **Verify Configuration**: Check InfluxDB URL, organization, bucket, and token
3. **Check Logs**: Review application logs for error messages
4. **Network Connectivity**: Ensure the camera can reach the InfluxDB server
5. **Proxy Settings**: If behind a proxy, configure global device proxy settings

### Authentication Errors (HTTP 401)

- Verify the InfluxDB token has write permissions for the specified bucket
- Regenerate the token if necessary

### Network Errors

- Check firewall rules between camera and InfluxDB server
- Verify InfluxDB is running and accessible
- Test connectivity: `curl http://<INFLUXDB_URL>/ping`

### High CPU Usage

- Increase the collection interval to reduce overhead
- Recommended minimum: 30 seconds for production use

## Advanced Configuration

### Using VAPIX to Configure Parameters

Parameters can also be set via VAPIX API:

```bash
# Set InfluxDB URL
curl -u root:pass "http://<DEVICE_IP>/axis-cgi/param.cgi?action=update&root.CameraHealthMonitor.InfluxDBURL=http://influxdb.example.com:8086"

# Enable sending
curl -u root:pass "http://<DEVICE_IP>/axis-cgi/param.cgi?action=update&root.CameraHealthMonitor.EnableSending=yes"

# Set collection interval
curl -u root:pass "http://<DEVICE_IP>/axis-cgi/param.cgi?action=update&root.CameraHealthMonitor.CollectionInterval=120"
```

### Modifying the Application

To modify the destination or add custom metrics:

1. Edit `app/camera_health_monitor.c`
2. Modify the `collect_and_send_metrics()` function to add new metrics
3. Update the `format_influxdb_line()` function to include new fields
4. Rebuild and reinstall the application

## API Documentation

This application uses the following Axis APIs:

- [Parameter API (AXParameter)](https://developer.axis.com/acap/api/native-sdk-api/#parameter-api) - For reading configuration and device information
- [cURL library](https://curl.se/libcurl/c/) - For HTTP communication with InfluxDB
- [GLib](https://developer.gnome.org/glib/) - For main loop and utilities

## Performance Considerations

- **Collection Interval**: Balance between data granularity and system load
  - Minimum recommended: 10 seconds
  - Default: 60 seconds
  - For low-impact monitoring: 300 seconds (5 minutes)

- **Network Bandwidth**: Each data point is approximately 200-300 bytes
  - At 60-second intervals: ~5 KB/minute
  - At 10-second intervals: ~30 KB/minute

- **CPU Impact**: Minimal (<1% CPU usage on most devices)

## Security Considerations

- **Token Storage**: The InfluxDB token is stored as a hidden parameter but is accessible via VAPIX
- **Network Security**: Use HTTPS URLs for InfluxDB in production (`https://...`)
- **Access Control**: Restrict VAPIX access to authorized users only
- **Token Permissions**: Use tokens with minimal required permissions (write-only to specific bucket)

## License

**[Apache License 2.0](../LICENSE)**
