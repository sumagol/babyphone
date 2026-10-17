/// Streaming linear-interpolation resampler for mics that can't run at 48 kHz
/// (WASAPI shared mode only offers the rate set in Windows sound settings).
/// Linear is plenty for a voice monitor; capture picks the rate closest to 48 kHz,
/// so large downsampling ratios (and their aliasing) are rare.
pub struct Resampler {
    step: f64,
    pos: f64,
    prev: f32,
}

impl Resampler {
    pub fn new(in_rate: u32, out_rate: u32) -> Self {
        Self { step: in_rate as f64 / out_rate as f64, pos: 0.0, prev: 0.0 }
    }

    pub fn is_passthrough(&self) -> bool {
        self.step == 1.0
    }

    pub fn process(&mut self, input: &[i16], out: &mut Vec<i16>) {
        if self.is_passthrough() {
            out.extend_from_slice(input);
            return;
        }
        let Some(&last) = input.last() else { return };

        // Index 0 is the last sample of the previous chunk, input follows at 1..
        let at = |i: usize| if i == 0 { self.prev } else { input[i - 1] as f32 };
        loop {
            let i = self.pos as usize;
            if i + 1 > input.len() {
                break;
            }
            let frac = (self.pos - i as f64) as f32;
            out.push((at(i) + (at(i + 1) - at(i)) * frac).round() as i16);
            self.pos += self.step;
        }
        self.pos -= input.len() as f64;
        self.prev = last as f32;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run(in_rate: u32, secs: usize, chunk: usize) -> Vec<i16> {
        let mut r = Resampler::new(in_rate, 48_000);
        let input: Vec<i16> = (0..in_rate as usize * secs).map(|i| (i % 1000) as i16).collect();
        let mut out = Vec::new();
        for c in input.chunks(chunk) {
            r.process(c, &mut out);
        }
        out
    }

    #[test]
    fn output_rate_matches_48k_regardless_of_chunking() {
        for (rate, chunk) in [(44_100, 441), (44_100, 1000), (16_000, 160), (96_000, 480)] {
            let n = run(rate, 2, chunk).len() as i64;
            assert!((n - 96_000).abs() <= 2, "{rate} Hz, chunk {chunk}: {n} samples");
        }
    }

    #[test]
    fn interpolates_between_samples() {
        let mut r = Resampler::new(24_000, 48_000);
        let mut out = Vec::new();
        r.process(&[100, 200], &mut out);
        assert_eq!(out, [0, 50, 100, 150]);
    }

    #[test]
    fn passthrough_at_48k() {
        let mut r = Resampler::new(48_000, 48_000);
        let mut out = Vec::new();
        r.process(&[1, 2, 3], &mut out);
        assert_eq!(out, [1, 2, 3]);
    }
}
