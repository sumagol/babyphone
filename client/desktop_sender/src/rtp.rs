use std::hash::{BuildHasher, Hasher};

use aes::cipher::{KeyIvInit, StreamCipher};

use crate::dsp::GuardMode;
use crate::telemetry::Power;

type Aes128Ctr = ctr::Ctr128BE<aes::Aes128>;

const HEADER_LEN: usize = 20; // 12 RTP + 8 extension
const MAX_PACKET: usize = HEADER_LEN + 256;

/// Builds packets byte-identical in layout to firmware/main/audio_encoder.c.
pub struct Packetizer {
    key: Option<[u8; 16]>,
    ssrc: u32,
    sequence: u16,
    timestamp: u32,
    sent: u64,
    buf: [u8; MAX_PACKET],
}

impl Packetizer {
    pub fn new(key: Option<[u8; 16]>) -> Self {
        Self::with_ssrc(key, random_ssrc())
    }

    fn with_ssrc(key: Option<[u8; 16]>, ssrc: u32) -> Self {
        Self { key, ssrc, sequence: 0, timestamp: 0, sent: 0, buf: [0; MAX_PACKET] }
    }

    pub fn encrypted(&self) -> bool {
        self.key.is_some()
    }

    pub fn packets_sent(&self) -> u64 {
        self.sent
    }

    pub fn build(&mut self, opus: &[u8], power: Power, guard: GuardMode) -> &[u8] {
        let b = &mut self.buf;
        b[0] = 0x90; // V=2, X=1
        b[1] = 96; // PT
        b[2..4].copy_from_slice(&self.sequence.to_be_bytes());
        b[4..8].copy_from_slice(&self.timestamp.to_be_bytes());
        b[8..12].copy_from_slice(&self.ssrc.to_be_bytes());

        b[12..14].copy_from_slice(&[0xBA, 0xBB]); // profile "Baby"
        b[14..16].copy_from_slice(&1u16.to_be_bytes()); // 1 word
        b[16] = (power.battery & 0x7F) | if power.charging { 0x80 } else { 0 };
        b[17] = power.temperature_c;
        b[18] = guard.tenths();
        b[19] = guard as u8;

        let end = HEADER_LEN + opus.len();
        b[HEADER_LEN..end].copy_from_slice(opus);

        if let Some(key) = &self.key {
            // IV = SSRC(4) | seq(2) | timestamp(4) | zero(6)
            let mut iv = [0u8; 16];
            iv[0..4].copy_from_slice(&b[8..12]);
            iv[4..6].copy_from_slice(&b[2..4]);
            iv[6..10].copy_from_slice(&b[4..8]);
            Aes128Ctr::new(key.into(), &iv.into()).apply_keystream(&mut b[HEADER_LEN..end]);
        }

        self.sequence = self.sequence.wrapping_add(1);
        self.timestamp = self.timestamp.wrapping_add(960);
        self.sent += 1;
        &self.buf[..end]
    }
}

/// Random per run so the CTR IV never repeats the firmware's (fixed SSRC 0x12345678) under the same key.
fn random_ssrc() -> u32 {
    let mut h = std::collections::hash_map::RandomState::new().build_hasher();
    h.write_u128(std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_nanos());
    h.write_u32(std::process::id());
    h.finish() as u32
}

#[cfg(test)]
mod tests {
    use super::*;

    const POWER: Power = Power { battery: 100, charging: true, temperature_c: 35 };

    #[test]
    fn header_layout_matches_firmware() {
        let mut p = Packetizer::with_ssrc(None, 0x12345678);
        p.build(&[1, 2, 3], POWER, GuardMode::HalfSecond);
        let pkt = p.build(&[1, 2, 3], POWER, GuardMode::FiveSeconds).to_vec();
        assert_eq!(
            pkt,
            [
                0x90, 96, 0, 1, 0, 0, 0x03, 0xC0, 0x12, 0x34, 0x56, 0x78, //
                0xBA, 0xBB, 0, 1, 0xE4, 35, 50, 1, //
                1, 2, 3
            ]
        );
    }

    #[test]
    fn encryption_round_trips_with_reconstructed_iv() {
        let key = *b"BabyPhoneKey2026";
        let payload = [0x11u8; 40];
        let mut p = Packetizer::with_ssrc(Some(key), 0xDEADBEEF);
        let pkt = p.build(&payload, POWER, GuardMode::HalfSecond).to_vec();
        assert_ne!(&pkt[HEADER_LEN..], &payload);

        // Decrypt the way babyphone_receiver.py does.
        let mut iv = [0u8; 16];
        iv[0..4].copy_from_slice(&pkt[8..12]);
        iv[4..6].copy_from_slice(&pkt[2..4]);
        iv[6..10].copy_from_slice(&pkt[4..8]);
        let mut body = pkt[HEADER_LEN..].to_vec();
        Aes128Ctr::new(&key.into(), &iv.into()).apply_keystream(&mut body);
        assert_eq!(body, payload);
    }
}
