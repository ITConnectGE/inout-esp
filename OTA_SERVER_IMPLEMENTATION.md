# OTA Firmware Update — Server Implementation Guide

The ESP32 device polls for OTA triggers via its existing heartbeat endpoint.
No new polling endpoint is needed — the server piggybacks OTA info onto the
heartbeat JSON response. The dashboard button sets a pending OTA on the device
record; the next heartbeat (≤30 s) delivers it.

---

## Flow

```
Dashboard user clicks "Update Firmware"
  → POST /admin/devices/{id}/ota   { ota_url, ota_version }
  → device record: ota_url + ota_version saved, ota_triggered_at = now

Device heartbeat fires (every ~30 s)
  → POST /device/heartbeat
  → server sees pending OTA, includes it in response JSON

Device receives response, starts download
  → LCD shows "Updating v0.4.4 / Progress: 80%"
  → NFC + relay frozen during download

On success: device reboots into new partition, next heartbeat reports firmware = "0.4.4"
  → server clears ota_url / ota_version on the device record

On failure: device reboots into old partition, next heartbeat still reports old firmware
  → server may retry or alert admin
```

---

## 1. Database — devices table

Add three nullable columns:

```php
// migration
Schema::table('devices', function (Blueprint $table) {
    $table->string('ota_url')->nullable()->after('firmware_version');
    $table->string('ota_version')->nullable()->after('ota_url');
    $table->timestamp('ota_triggered_at')->nullable()->after('ota_version');
});
```

| Column              | Type        | Purpose                                      |
|---------------------|-------------|----------------------------------------------|
| `ota_url`           | string/null | Direct download URL for the firmware binary  |
| `ota_version`       | string/null | Version string, e.g. `"0.4.4"`              |
| `ota_triggered_at`  | timestamp/null | When the admin triggered the update       |

---

## 2. Heartbeat endpoint — response change

`POST /device/heartbeat`  (existing endpoint)

The device sends:
```json
{
  "firmware": "0.4.3",
  "ip": "10.150.6.77",
  "rssi": -42,
  "config_version": 5,
  "unsynced_events": 0,
  "sd_mounted": true,
  "uptime_s": 3600
}
```

**Update the controller** to include OTA fields when pending, and clear them
once the device reports the new version:

```php
// DeviceHeartbeatController@handle (or equivalent)

public function handle(Request $request, Device $device)
{
    $reportedFirmware = $request->input('firmware');

    // Clear OTA flag once device has upgraded successfully
    if ($device->ota_version && $reportedFirmware === $device->ota_version) {
        $device->update([
            'ota_url'          => null,
            'ota_version'      => null,
            'ota_triggered_at' => null,
        ]);
    }

    // Persist heartbeat data (existing logic)
    $device->update([
        'firmware_version' => $reportedFirmware,
        'last_seen_at'     => now(),
        'ip_address'       => $request->input('ip'),
        // ... other fields
    ]);

    $response = [
        'resync' => $this->needsResync($device),
    ];

    // Deliver pending OTA if set and device is not already on that version
    if ($device->ota_url && $device->ota_version !== $reportedFirmware) {
        $response['ota_url']     = $device->ota_url;
        $response['ota_version'] = $device->ota_version;
    }

    return response()->json($response);
}
```

**Response when OTA is pending:**
```json
{
  "resync": false,
  "ota_url": "https://github.com/yourorg/inout-esp/releases/download/v0.4.4/firmware.bin",
  "ota_version": "0.4.4"
}
```

**Response when no OTA is pending:**
```json
{
  "resync": false
}
```

---

## 3. Admin trigger endpoint

`POST /admin/devices/{device}/ota`

```php
// routes/web.php or api.php (admin-authenticated)
Route::post('/admin/devices/{device}/ota', [DeviceOtaController::class, 'trigger'])
     ->middleware(['auth', 'admin']);
```

```php
// DeviceOtaController@trigger

public function trigger(Request $request, Device $device)
{
    $request->validate([
        'ota_url'     => 'required|url|max:512',
        'ota_version' => 'required|string|max:32',
    ]);

    $device->update([
        'ota_url'          => $request->ota_url,
        'ota_version'      => $request->ota_version,
        'ota_triggered_at' => now(),
    ]);

    return response()->json(['ok' => true]);
}
```

---

## 4. GitHub release URL

The device firmware binary must be reachable without authentication.
GitHub release assets redirect (HTTP 302) — the ESP32 follows these automatically.

Use the release asset URL format:
```
https://github.com/{owner}/{repo}/releases/download/{tag}/firmware.bin
```

Example:
```
https://github.com/yourorg/inout-esp/releases/download/v0.4.4/firmware.bin
```

The dashboard can fetch available versions from the GitHub Releases API and
present them in a dropdown, or the admin can paste the URL manually.

**GitHub Releases API** (no auth required for public repos):
```
GET https://api.github.com/repos/{owner}/{repo}/releases/latest
```
Returns `tag_name` and `assets[].browser_download_url`.

---

## 5. Dashboard UI (minimal)

On the device detail page, add an "Update Firmware" section:

```
┌─────────────────────────────────────────┐
│  Firmware Update                        │
│                                         │
│  Current:  v0.4.3                       │
│  Latest:   v0.4.4  (fetched from GitHub)│
│                                         │
│  URL:  [https://github.com/.../fw.bin ] │
│                                         │
│  [ Update to v0.4.4 ]                   │
└─────────────────────────────────────────┘
```

On button press → `POST /admin/devices/{id}/ota` with `ota_url` and
`ota_version`. The device will pick it up within 30 seconds.

If `ota_triggered_at` is set and more than 5 minutes have passed without the
device reporting the new firmware version, surface a warning:
**"Update may have failed — device still reporting v0.4.3"**.

---

## 6. OTA delivery guarantees

| Scenario | Behaviour |
|---|---|
| Device offline when triggered | OTA stays pending in DB; delivered on next heartbeat after reconnect |
| Device already on `ota_version` | Server skips OTA fields in response; device-side also guards against this |
| Download fails on device | Device reboots into old partition; next heartbeat still reports old firmware; server keeps OTA pending for retry |
| Admin triggers twice in a row | Second trigger overwrites — only one pending OTA per device |

---

## 7. Security notes

- The download URL must be HTTPS. The device does not validate the server TLS
  certificate by default (no `/data/ca.pem` for GitHub's CDN). Place a CA
  bundle on the SD card at `/data/ca.pem` to enable certificate pinning.
- Restrict the trigger endpoint to `super_admin` role if you have one —
  flashing firmware is a privileged action.
- Consider verifying a SHA-256 checksum of the binary server-side before
  storing the URL, to prevent accidentally pointing devices at a bad build.

---

# Camera OTA Firmware Update — Server Implementation Guide

The ESP32-CAM is updated indirectly: the main ESP32 receives the camera
firmware URL from the server via heartbeat, relays it to the camera over UART,
and the camera downloads the binary itself over WiFi. The server only needs to
track two extra fields per device.

---

## Flow

```
Dashboard user clicks "Update Camera Firmware"
  → POST /admin/devices/{id}/cam-ota   { cam_ota_url, cam_ota_version }
  → device record: cam_ota_url + cam_ota_version saved

Device heartbeat fires (every ~30 s)
  → POST /device/heartbeat  { "firmware": "0.4.4", "cam_firmware": "1.0.0", ... }
  → server sees pending cam OTA, includes it in response JSON

Device (main ESP32) receives response
  → sends  CAM_OTA <url>  to camera over UART
  → camera replies OTA_START, disconnects from UART, connects WiFi
  → LCD shows "Camera update / In progress..."
  → NFC scans frozen; event sync continues uninterrupted

Camera downloads + flashes firmware (HTTPUpdate), reboots
  → main ESP polls PING every 3 s for up to 120 s
  → camera replies CAM_READY once back online
  → main ESP queries CAM_VERSION, updates ApiClient.camFirmwareVersion

Next heartbeat reports "cam_firmware": "1.1.0"
  → server clears cam_ota_url / cam_ota_version on device record
```

---

## 1. Database — devices table

Add four nullable columns (in addition to the three from main OTA):

```php
Schema::table('devices', function (Blueprint $table) {
    $table->string('cam_firmware_version')->nullable()->after('ota_triggered_at');
    $table->string('cam_ota_url')->nullable()->after('cam_firmware_version');
    $table->string('cam_ota_version')->nullable()->after('cam_ota_url');
    $table->timestamp('cam_ota_triggered_at')->nullable()->after('cam_ota_version');
});
```

| Column                  | Type           | Purpose                                          |
|-------------------------|----------------|--------------------------------------------------|
| `cam_firmware_version`  | string/null    | Last reported camera firmware version            |
| `cam_ota_url`           | string/null    | Direct download URL for the camera binary        |
| `cam_ota_version`       | string/null    | Expected version after update, e.g. `"1.1.0"`   |
| `cam_ota_triggered_at`  | timestamp/null | When the admin triggered the camera update       |

---

## 2. Heartbeat endpoint — request and response changes

`POST /device/heartbeat`

The device now sends `cam_firmware` in the payload (only present when a camera
is detected at boot):

```json
{
  "firmware": "0.4.4",
  "cam_firmware": "1.0.0",
  "ip": "10.150.6.77",
  "rssi": -42,
  "config_version": 5,
  "unsynced_events": 0,
  "sd_mounted": true,
  "uptime_s": 3600
}
```

**Update the controller** to handle `cam_firmware` the same way `firmware` is
handled for main OTA:

```php
public function handle(Request $request, Device $device)
{
    $reportedFirmware    = $request->input('firmware');
    $reportedCamFirmware = $request->input('cam_firmware');  // nullable

    // Clear main OTA flag once device has upgraded
    if ($device->ota_version && $reportedFirmware === $device->ota_version) {
        $device->update([
            'ota_url'          => null,
            'ota_version'      => null,
            'ota_triggered_at' => null,
        ]);
    }

    // Clear camera OTA flag once camera has upgraded
    if ($reportedCamFirmware &&
        $device->cam_ota_version &&
        $reportedCamFirmware === $device->cam_ota_version)
    {
        $device->update([
            'cam_ota_url'          => null,
            'cam_ota_version'      => null,
            'cam_ota_triggered_at' => null,
        ]);
    }

    // Persist heartbeat data
    $device->update([
        'firmware_version'     => $reportedFirmware,
        'cam_firmware_version' => $reportedCamFirmware,   // null if no camera
        'last_seen_at'         => now(),
        'ip_address'           => $request->input('ip'),
        // ... other existing fields
    ]);

    $response = [
        'resync' => $this->needsResync($device),
    ];

    // Deliver pending main OTA
    if ($device->ota_url && $device->ota_version !== $reportedFirmware) {
        $response['ota_url']     = $device->ota_url;
        $response['ota_version'] = $device->ota_version;
    }

    // Deliver pending camera OTA
    if ($device->cam_ota_url && $device->cam_ota_version !== $reportedCamFirmware) {
        $response['cam_ota_url']     = $device->cam_ota_url;
        $response['cam_ota_version'] = $device->cam_ota_version;
    }

    return response()->json($response);
}
```

**Response when camera OTA is pending:**
```json
{
  "resync": false,
  "cam_ota_url": "https://github.com/yourorg/inout-esp/releases/download/cam-v1.1.0/cam-firmware.bin",
  "cam_ota_version": "1.1.0"
}
```

---

## 3. Admin trigger endpoint

`POST /admin/devices/{device}/cam-ota`

```php
// routes/web.php or api.php
Route::post('/admin/devices/{device}/cam-ota', [DeviceCamOtaController::class, 'trigger'])
     ->middleware(['auth', 'admin']);
```

```php
// DeviceCamOtaController@trigger

public function trigger(Request $request, Device $device)
{
    $request->validate([
        'cam_ota_url'     => 'required|url|max:512',
        'cam_ota_version' => 'required|string|max:32',
    ]);

    $device->update([
        'cam_ota_url'          => $request->cam_ota_url,
        'cam_ota_version'      => $request->cam_ota_version,
        'cam_ota_triggered_at' => now(),
    ]);

    return response()->json(['ok' => true]);
}
```

---

## 4. GitHub release URL

Camera firmware is a separate binary built from `cam-firmware/`. Tag and name
it distinctly from the main firmware to avoid confusion:

```
https://github.com/{owner}/{repo}/releases/download/cam-v1.1.0/cam-firmware.bin
```

The camera uses a custom partition table (`cam_ota.csv`, two 1.875 MB
partitions). The binary must fit within 1.875 MB. Build size can be checked
with `pio run` in `cam-firmware/` — the linker will error if it overflows.

---

## 5. Dashboard UI

Add a "Camera Firmware" row alongside the main firmware row on the device
detail page:

```
┌─────────────────────────────────────────────┐
│  Main Firmware                              │
│  Current: v0.4.4   Latest: v0.4.4  (up to date) │
│                                             │
│  Camera Firmware                            │
│  Current: v1.0.0   Latest: v1.1.0          │
│  URL: [https://github.com/.../cam-fw.bin ] │
│  [ Update Camera to v1.1.0 ]               │
└─────────────────────────────────────────────┘
```

If `cam_firmware_version` is null on the device record, the camera is not
present — hide the camera firmware row entirely.

If `cam_ota_triggered_at` is set and more than 5 minutes have passed without
`cam_firmware_version` matching `cam_ota_version`, surface a warning:
**"Camera update may have failed — still reporting v1.0.0"**.

---

## 6. Camera OTA delivery guarantees

| Scenario | Behaviour |
|---|---|
| Camera not present (cam_firmware null in heartbeat) | Server may still set cam_ota fields; device-side guard (`CamUart.isReady()`) silently skips the update |
| Camera already on `cam_ota_version` | Device-side version check skips OTA; next heartbeat clears DB fields |
| WiFi credentials not yet loaded on camera | Camera replies `OTA_FAILED`; `requestOta()` returns false; `performCamOta()` shows "Cam update failed!" on LCD |
| WiFi connection fails | Camera re-inits, replies `OTA_FAILED`; main ESP shows error on LCD; DB keeps pending OTA for retry |
| Download fails | Same as WiFi failure — camera re-inits and resumes normal operation |
| Camera does not come back within 120 s | `requestOta()` returns false; main ESP shows "timed out!"; DB keeps pending OTA |
| Main ESP reboots mid-camera-OTA | Camera finishes its own OTA independently; next boot the main ESP queries `CAM_VERSION` and reports the new version on the next heartbeat |

---

## 7. Key differences from main firmware OTA

| | Main OTA | Camera OTA |
|---|---|---|
| Downloader | Main ESP32 (HTTPUpdate) | ESP32-CAM (HTTPUpdate) |
| WiFi during update | Main ESP already connected | Camera connects independently using stored credentials |
| NFC during update | Frozen | Frozen |
| Event sync during update | Frozen | Continues normally |
| Failure recovery | Dual-partition automatic rollback | Camera re-inits; no rollback (single active partition replaced only on success) |
| Partition table | Standard `min_spiffs.csv` | Custom `cam_ota.csv` (1.875 MB × 2) |
| Serial trigger | `update <url> <version>` | `cam update <url> <version>` |
