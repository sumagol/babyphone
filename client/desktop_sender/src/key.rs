use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};

const DEFAULT_KEY: &[u8; 16] = b"BabyPhoneKey2026";
const FILE_NAME: &str = "babyphone_key.env";

/// Loads AES_KEY from babyphone_key.env, the same file the firmware, app and Python receiver use.
pub fn load(explicit: Option<&Path>) -> Result<[u8; 16]> {
    let path = match explicit {
        Some(p) => Some(p.to_path_buf()),
        None => candidates().into_iter().find(|p| p.is_file()),
    };

    let Some(path) = path else {
        eprintln!("Warning: {FILE_NAME} not found, using the default key.");
        return Ok(*DEFAULT_KEY);
    };

    let text = fs::read_to_string(&path).with_context(|| format!("reading {}", path.display()))?;
    let value = text
        .lines()
        .find_map(|l| l.trim().strip_prefix("AES_KEY="))
        .with_context(|| format!("no AES_KEY= line in {}", path.display()))?
        .trim();

    let Ok(key) = <[u8; 16]>::try_from(value.as_bytes()) else {
        bail!("AES_KEY in {} must be exactly 16 characters, got {}", path.display(), value.len());
    };
    eprintln!("Key: {}", path.display());
    Ok(key)
}

fn candidates() -> Vec<PathBuf> {
    let mut paths = vec![PathBuf::from(FILE_NAME)];
    // Next to the executable, for a copied .exe.
    if let Some(dir) = std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf)) {
        paths.push(dir.join(FILE_NAME));
    }
    // Repository root, relative to this crate (client/desktop_sender).
    paths.push(Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").join(FILE_NAME));
    paths
}
