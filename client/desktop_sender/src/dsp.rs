use std::str::FromStr;

pub const FRAME_SAMPLES: usize = 960; // 20 ms at 48 kHz
const HANGOVER_FRAMES: u32 = 125; // 2.5 s hold after noise drops

/// Noise Guard modes; discriminants match the firmware enum sent in RTP ext byte 3.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum GuardMode {
    HalfSecond = 0,
    FiveSeconds = 1,
    TenSeconds = 2,
}

impl GuardMode {
    pub fn from_index(i: u8) -> Self {
        match i {
            1 => Self::FiveSeconds,
            2 => Self::TenSeconds,
            _ => Self::HalfSecond,
        }
    }

    pub fn next(self) -> Self {
        Self::from_index((self as u8 + 1) % 3)
    }

    /// Consecutive noisy frames required before the gate opens.
    pub fn required_frames(self) -> u32 {
        match self {
            Self::HalfSecond => 25,
            Self::FiveSeconds => 250,
            Self::TenSeconds => 500,
        }
    }

    /// Duration in tenths of a second (RTP ext byte 2).
    pub fn tenths(self) -> u8 {
        match self {
            Self::HalfSecond => 5,
            Self::FiveSeconds => 50,
            Self::TenSeconds => 100,
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            Self::HalfSecond => "0.5s",
            Self::FiveSeconds => "5.0s",
            Self::TenSeconds => "10.0s",
        }
    }
}

impl FromStr for GuardMode {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s.trim_end_matches('s') {
            "0.5" => Ok(Self::HalfSecond),
            "5" | "5.0" => Ok(Self::FiveSeconds),
            "10" | "10.0" => Ok(Self::TenSeconds),
            _ => Err(format!("guard must be 0.5, 5 or 10 (got {s})")),
        }
    }
}

/// 2nd-order Butterworth high-pass (RBJ cookbook), cuts desk rumble and fan hum.
pub struct HighPass {
    b0: f32,
    b1: f32,
    b2: f32,
    a1: f32,
    a2: f32,
    x1: f32,
    x2: f32,
    y1: f32,
    y2: f32,
}

impl HighPass {
    pub fn new(cutoff_hz: f32, sample_rate: f32) -> Self {
        let w0 = 2.0 * std::f32::consts::PI * cutoff_hz / sample_rate;
        // Q = 1/sqrt(2) for a maximally flat Butterworth response.
        let alpha = w0.sin() / (2.0 * std::f32::consts::FRAC_1_SQRT_2);
        let cos = w0.cos();
        let a0 = 1.0 + alpha;
        Self {
            b0: (1.0 + cos) / 2.0 / a0,
            b1: -(1.0 + cos) / a0,
            b2: (1.0 + cos) / 2.0 / a0,
            a1: -2.0 * cos / a0,
            a2: (1.0 - alpha) / a0,
            x1: 0.0,
            x2: 0.0,
            y1: 0.0,
            y2: 0.0,
        }
    }

    pub fn process(&mut self, frame: &mut [i16]) {
        for s in frame.iter_mut() {
            let x = *s as f32;
            let y = self.b0 * x + self.b1 * self.x1 + self.b2 * self.x2 - self.a1 * self.y1 - self.a2 * self.y2;
            self.x2 = self.x1;
            self.x1 = x;
            self.y2 = self.y1;
            self.y1 = y;
            *s = y.clamp(i16::MIN as f32, i16::MAX as f32) as i16;
        }
    }
}

/// Port of the firmware gate in audio_capture.c plus the RMS floor in audio_encoder.c.
pub struct NoiseGate {
    threshold: f64,
    noise_frames: u32,
    silence_frames: u32,
    open: bool,
}

impl NoiseGate {
    pub fn new(threshold: f64) -> Self {
        Self { threshold, noise_frames: 0, silence_frames: 0, open: false }
    }

    pub fn threshold(&self) -> f64 {
        self.threshold
    }

    pub fn is_open(&self) -> bool {
        self.open
    }

    /// Gates the frame in place and returns its pre-gate RMS for the VU meter.
    pub fn process(&mut self, frame: &mut [i16], mode: GuardMode) -> f64 {
        let rms = rms(frame);

        if rms >= self.threshold {
            self.noise_frames += 1;
            self.silence_frames = 0;
            if self.noise_frames >= mode.required_frames() {
                self.open = true;
            }
        } else {
            self.noise_frames = 0;
            if self.open {
                self.silence_frames += 1;
                if self.silence_frames >= HANGOVER_FRAMES {
                    self.open = false;
                    self.silence_frames = 0;
                }
            }
        }

        if !self.open {
            // Closed gate still yields a (silent) frame so receivers' watchdogs stay quiet.
            frame.fill(0);
            return rms;
        }

        if rms < self.threshold {
            // Soft gate during the hangover to suppress hiss between cries.
            let factor = (rms / self.threshold).powi(2);
            for s in frame.iter_mut() {
                *s = (*s as f64 * factor) as i16;
            }
        }

        // Firmware floor: RMS 50 against threshold 75, kept proportional here.
        if self::rms(frame) < self.threshold * 2.0 / 3.0 {
            frame.fill(0);
        }
        rms
    }
}

fn rms(frame: &[i16]) -> f64 {
    let sum_sq: f64 = frame.iter().map(|&s| (s as f64) * (s as f64)).sum();
    (sum_sq / frame.len() as f64).sqrt()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn loud() -> Vec<i16> {
        (0..FRAME_SAMPLES).map(|i| if i % 2 == 0 { 1000 } else { -1000 }).collect()
    }

    #[test]
    fn gate_opens_after_required_frames_and_holds() {
        let mut gate = NoiseGate::new(75.0);
        for _ in 0..24 {
            gate.process(&mut loud(), GuardMode::HalfSecond);
        }
        assert!(!gate.is_open());
        let mut f = loud();
        gate.process(&mut f, GuardMode::HalfSecond);
        assert!(gate.is_open());
        assert_eq!(f[0], 1000);

        for _ in 0..124 {
            gate.process(&mut vec![0; FRAME_SAMPLES], GuardMode::HalfSecond);
        }
        assert!(gate.is_open());
        gate.process(&mut vec![0; FRAME_SAMPLES], GuardMode::HalfSecond);
        assert!(!gate.is_open());
    }

    #[test]
    fn closed_gate_zeroes_audio() {
        let mut gate = NoiseGate::new(75.0);
        let mut f = loud();
        gate.process(&mut f, GuardMode::TenSeconds);
        assert!(f.iter().all(|&s| s == 0));
    }

    #[test]
    fn highpass_removes_dc() {
        let mut hp = HighPass::new(80.0, 48_000.0);
        let mut f = vec![5000i16; FRAME_SAMPLES];
        for _ in 0..10 {
            f.fill(5000);
            hp.process(&mut f);
        }
        assert!(f.iter().all(|s| s.abs() < 5));
    }

    #[test]
    fn guard_parsing() {
        assert_eq!("0.5".parse::<GuardMode>().unwrap(), GuardMode::HalfSecond);
        assert_eq!("5s".parse::<GuardMode>().unwrap(), GuardMode::FiveSeconds);
        assert_eq!("10".parse::<GuardMode>().unwrap(), GuardMode::TenSeconds);
        assert!("3".parse::<GuardMode>().is_err());
    }
}
