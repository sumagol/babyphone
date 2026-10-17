use std::sync::mpsc::{self, Receiver, SyncSender};

use anyhow::{anyhow, bail, Context, Result};
use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{FromSample, Sample, SampleFormat, SampleRate, SizedSample, Stream, StreamConfig};

const TARGET_RATE: u32 = 48_000;

pub fn list_devices() -> Result<()> {
    let host = cpal::default_host();
    let default = host.default_input_device().and_then(|d| d.name().ok());
    println!("Audio input devices ({}):", host.id().name());
    for device in host.input_devices()? {
        let name = device.name().unwrap_or_else(|_| "<unknown>".into());
        let marker = if Some(&name) == default.as_ref() { "*" } else { " " };
        let rate = device
            .supported_input_configs()
            .ok()
            .and_then(|c| c.filter(convertible).map(|r| closest_rate(&r)).min_by_key(|&r| r.abs_diff(TARGET_RATE)));
        let note = match rate {
            Some(TARGET_RATE) => String::new(),
            Some(r) => format!("  ({r} Hz, resampled)"),
            None => "  (no usable input format)".into(),
        };
        println!("  {marker} {name}{note}");
    }
    Ok(())
}

/// Opens the mic and returns mono i16 chunks of arbitrary length at the returned sample rate.
/// The Stream must be kept alive for capture to continue.
pub fn start_capture(device_name: Option<&str>) -> Result<(Stream, Receiver<Vec<i16>>, u32)> {
    let host = cpal::default_host();
    let device = match device_name {
        None => host.default_input_device().context("no default input device")?,
        Some(wanted) => host
            .input_devices()?
            .find(|d| d.name().map(|n| n.contains(wanted)).unwrap_or(false))
            .ok_or_else(|| anyhow!("no input device matching {wanted:?}, see --list"))?,
    };
    let name = device.name().unwrap_or_default();

    // Prefer the rate closest to 48 kHz (ideally exact), then the fewest channels.
    let range = device
        .supported_input_configs()
        .with_context(|| format!("querying {name}"))?
        .filter(convertible)
        .min_by_key(|r| (closest_rate(r).abs_diff(TARGET_RATE), r.channels()))
        .ok_or_else(|| anyhow!("{name} offers no i16/i32/f32 input format"))?;
    let rate = closest_rate(&range);
    let format = range.sample_format();
    let config: StreamConfig = range.with_sample_rate(SampleRate(rate)).into();
    let resampled = if rate == TARGET_RATE { "" } else { ", resampled to 48000 Hz" };
    eprintln!("Mic: {name} ({} ch, {format:?}, {rate} Hz{resampled})", config.channels);

    // Bounded so a stalled encoder drops audio instead of growing memory forever.
    let (tx, rx) = mpsc::sync_channel(64);
    let stream = match format {
        SampleFormat::I16 => build::<i16>(&device, &config, tx)?,
        SampleFormat::F32 => build::<f32>(&device, &config, tx)?,
        SampleFormat::I32 => build::<i32>(&device, &config, tx)?,
        other => bail!("unsupported sample format {other:?}"),
    };
    stream.play()?;
    Ok((stream, rx, rate))
}

fn convertible(r: &cpal::SupportedStreamConfigRange) -> bool {
    matches!(r.sample_format(), SampleFormat::I16 | SampleFormat::F32 | SampleFormat::I32)
}

fn closest_rate(r: &cpal::SupportedStreamConfigRange) -> u32 {
    TARGET_RATE.clamp(r.min_sample_rate().0, r.max_sample_rate().0)
}

fn build<T>(device: &cpal::Device, config: &StreamConfig, tx: SyncSender<Vec<i16>>) -> Result<Stream>
where
    T: SizedSample,
    i16: FromSample<T>,
{
    let channels = config.channels as usize;
    let stream = device.build_input_stream(
        config,
        move |data: &[T], _| {
            let mono: Vec<i16> = data
                .chunks_exact(channels)
                .map(|f| {
                    let sum: i32 = f.iter().map(|s| i16::from_sample(*s) as i32).sum();
                    (sum / channels as i32) as i16
                })
                .collect();
            // Never block the realtime audio thread.
            let _ = tx.try_send(mono);
        },
        |e| eprintln!("\naudio stream error: {e}"),
        None,
    )?;
    Ok(stream)
}
