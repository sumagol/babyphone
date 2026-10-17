mod audio;
mod dsp;
mod key;
mod net;
mod resample;
mod rtp;
mod telemetry;

use std::io::{BufRead, Write};
use std::path::PathBuf;
use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::Arc;

use anyhow::{Context, Result};
use clap::Parser;

use dsp::{GuardMode, HighPass, NoiseGate, FRAME_SAMPLES};

#[derive(Parser, Debug)]
#[command(version, about = "Turns this computer into a multicast babyphone sender")]
struct Args {
    /// List audio input devices and network interfaces, then exit
    #[arg(long)]
    list: bool,

    /// Input device name (substring match). Defaults to the system default mic
    #[arg(long)]
    device: Option<String>,

    /// Egress interface for multicast, by name (e.g. wlan0) or IPv4 address
    #[arg(long)]
    iface: Option<String>,

    /// Multicast TTL (use 2 if Wi-Fi repeaters sit between sender and receiver)
    #[arg(long, default_value_t = 1, value_parser = clap::value_parser!(u32).range(1..=2))]
    ttl: u32,

    /// Noise Guard: seconds of continuous noise before the gate opens (0.5, 5 or 10)
    #[arg(long, default_value = "0.5")]
    guard: GuardMode,

    /// RMS level (16-bit PCM units) that counts as noise. Firmware uses 75
    #[arg(long, default_value_t = 75.0)]
    threshold: f64,

    /// Send raw Opus without AES so VLC can play client/vlc/stream.sdp
    #[arg(long)]
    unencrypted: bool,

    /// Path to babyphone_key.env (default: ./ then the repository root)
    #[arg(long)]
    key_file: Option<PathBuf>,
}

fn main() -> Result<()> {
    let args = Args::parse();

    if args.list {
        audio::list_devices()?;
        net::list_interfaces()?;
        return Ok(());
    }

    let key = if args.unencrypted {
        None
    } else {
        Some(key::load(args.key_file.as_deref())?)
    };

    let sender = net::MulticastSender::new(args.iface.as_deref(), args.ttl)?;
    let (_stream, chunks, mic_rate) = audio::start_capture(args.device.as_deref())?;
    let mut resampler = resample::Resampler::new(mic_rate, 48_000);

    let mut encoder =
        opus::Encoder::new(48_000, opus::Channels::Mono, opus::Application::Voip)
            .context("creating Opus encoder")?;
    encoder.set_bitrate(opus::Bitrate::Bits(24_000))?;
    encoder.set_vbr(true)?;

    let mut packetizer = rtp::Packetizer::new(key);
    let mut highpass = HighPass::new(80.0, 48_000.0);
    let mut gate = NoiseGate::new(args.threshold);

    // Enter on stdin cycles the guard mode, like KEY2 on the M5StickS3.
    let guard = Arc::new(AtomicU8::new(args.guard as u8));
    {
        let guard = guard.clone();
        std::thread::spawn(move || {
            for _ in std::io::stdin().lock().lines() {
                let next = GuardMode::from_index(guard.load(Ordering::Relaxed)).next();
                guard.store(next as u8, Ordering::Relaxed);
            }
        });
    }

    eprintln!(
        "Streaming to {} ({}), threshold {:.0}. Press Enter to cycle Noise Guard, Ctrl+C to stop.",
        net::TARGET,
        if packetizer.encrypted() { "AES-128-CTR" } else { "UNENCRYPTED" },
        args.threshold,
    );

    let mut pending: Vec<i16> = Vec::with_capacity(FRAME_SAMPLES * 4);
    let mut frame = [0i16; FRAME_SAMPLES];
    let mut opus_buf = [0u8; 256];
    let mut power = telemetry::read();
    let mut frame_count: u64 = 0;

    for chunk in chunks {
        resampler.process(&chunk, &mut pending);
        while pending.len() >= FRAME_SAMPLES {
            frame.copy_from_slice(&pending[..FRAME_SAMPLES]);
            pending.drain(..FRAME_SAMPLES);
            frame_count += 1;

            // Same 5 s cadence the firmware uses for PMIC polling.
            if frame_count % 250 == 0 {
                power = telemetry::read();
            }

            let mode = GuardMode::from_index(guard.load(Ordering::Relaxed));
            highpass.process(&mut frame);
            let rms = gate.process(&mut frame, mode);

            let len = encoder.encode(&frame, &mut opus_buf).context("Opus encode")?;
            let packet = packetizer.build(&opus_buf[..len], power, mode);
            if let Err(e) = sender.send(packet) {
                eprintln!("\nsend failed: {e}");
            }

            if frame_count % 5 == 0 {
                draw_meter(rms, &gate, mode, power, packetizer.packets_sent());
            }
        }
    }

    anyhow::bail!("audio input stream ended")
}

fn draw_meter(rms: f64, gate: &NoiseGate, mode: GuardMode, power: telemetry::Power, packets: u64) {
    const WIDTH: usize = 30;
    let db = if rms > 1.0 { 20.0 * (rms / 32768.0).log10() } else { -60.0 };
    // -60 dBFS .. 0 dBFS mapped onto the bar.
    let filled = (((db + 60.0) / 60.0).clamp(0.0, 1.0) * WIDTH as f64) as usize;
    let bar: String = "█".repeat(filled) + &"░".repeat(WIDTH - filled);
    let state = if gate.is_open() { "OPEN  " } else { "closed" };
    let plug = if power.charging { "⚡" } else { " " };
    eprint!(
        "\r[{bar}] {db:>4.0} dB  rms {rms:>6.0}/{:<4.0} gate {state} guard {:<5} bat {:>3}%{plug} pkts {packets}  ",
        gate.threshold(),
        mode.label(),
        power.battery,
    );
    let _ = std::io::stderr().flush();
}
