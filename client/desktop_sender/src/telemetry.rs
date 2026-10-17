/// Values for RTP extension bytes 0 and 1.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Power {
    pub battery: u8,
    pub charging: bool,
    pub temperature_c: u8,
}

/// Reported instead of a real CPU temperature: desktop CPUs routinely run
/// above the app's 68 °C warning band, which is calibrated for the M5StickS3.
const REPORTED_TEMP_C: u8 = 35;

/// Desktops without a battery report 100 % on AC power (0xE4), as the plan specifies.
const MAINS: Power = Power { battery: 100, charging: true, temperature_c: REPORTED_TEMP_C };

#[cfg(target_os = "linux")]
pub fn read() -> Power {
    use std::fs;
    use std::path::Path;

    let read = |dir: &Path, file: &str| fs::read_to_string(dir.join(file)).map(|s| s.trim().to_owned()).ok();

    let Ok(entries) = fs::read_dir("/sys/class/power_supply") else {
        return MAINS;
    };

    let mut battery = None;
    let mut ac_online = false;
    for dir in entries.flatten().map(|e| e.path()) {
        match read(&dir, "type").as_deref() {
            Some("Battery") if read(&dir, "scope").as_deref() != Some("Device") => {
                // Skip peripheral batteries (mice, headsets): they report scope=Device.
                if battery.is_none() {
                    battery = read(&dir, "capacity").and_then(|c| c.parse::<u8>().ok());
                }
            }
            Some("Mains" | "USB" | "USB_C") => ac_online |= read(&dir, "online").as_deref() == Some("1"),
            _ => {}
        }
    }

    match battery {
        None => MAINS,
        Some(pct) => Power { battery: pct.min(100), charging: ac_online, temperature_c: REPORTED_TEMP_C },
    }
}

#[cfg(windows)]
pub fn read() -> Power {
    use windows_sys::Win32::System::Power::{GetSystemPowerStatus, SYSTEM_POWER_STATUS};

    const NO_SYSTEM_BATTERY: u8 = 128;
    const UNKNOWN: u8 = 255;

    let mut status: SYSTEM_POWER_STATUS = unsafe { std::mem::zeroed() };
    if unsafe { GetSystemPowerStatus(&mut status) } == 0
        || status.BatteryFlag & NO_SYSTEM_BATTERY != 0
        || status.BatteryLifePercent == UNKNOWN
    {
        return MAINS;
    }
    Power {
        battery: status.BatteryLifePercent.min(100),
        charging: status.ACLineStatus == 1,
        temperature_c: REPORTED_TEMP_C,
    }
}

#[cfg(not(any(target_os = "linux", windows)))]
pub fn read() -> Power {
    MAINS
}
