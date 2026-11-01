use std::fs;
use std::io::{Cursor, Read};
use sha2::{Sha256, Digest};
use tar::Archive;
use flate2::read::GzDecoder;
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, Debug)]
pub struct Manifest {
    pub name: String,
    pub version: String,
    pub dependencies: std::collections::HashMap<String, Dependency>,
}

#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(untagged)]
pub enum Dependency {
    Url(String),
    Detailed(DetailedDep),
}

#[derive(Serialize, Deserialize, Debug, Clone, Default)]
pub struct DetailedDep {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub url: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sha256: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub path: Option<String>,
}

impl Dependency {
    fn url(&self) -> Option<&str> {
        match self {
            Dependency::Url(u) => Some(u),
            Dependency::Detailed(d) => d.url.as_deref(),
        }
    }
    fn sha256(&self) -> Option<&str> {
        match self {
            Dependency::Url(_) => None,
            Dependency::Detailed(d) => d.sha256.as_deref(),
        }
    }
    fn path(&self) -> Option<&str> {
        match self {
            Dependency::Url(_) => None,
            Dependency::Detailed(d) => d.path.as_deref(),
        }
    }
}

pub fn install_from_manifest() -> Result<(), String> {
    let manifest_str = fs::read_to_string("donky.json")
        .map_err(|_| "Could not find donky.json in the current directory.".to_string())?;
    let mut manifest: Manifest = serde_json::from_str(&manifest_str)
        .map_err(|e| format!("Failed to parse donky.json: {}", e))?;

    println!("Resolving dependencies for {} v{}...", manifest.name, manifest.version);
    fs::create_dir_all("donky_modules").map_err(|e| e.to_string())?;

    // Collect string-form → detailed-form upgrades to apply after the loop
    let mut upgrades: Vec<(String, Dependency)> = Vec::new();

    for (name, dep) in &manifest.dependencies {
        println!("Installing {}...", name);

        // Path dependency: copy directory
        if let Some(p) = dep.path() {
            let src = std::path::Path::new(p);
            let dst = std::path::Path::new("donky_modules").join(name);
            if !src.exists() {
                return Err(format!("Path dependency {} not found at {}", name, p));
            }
            copy_dir_recursive(src, &dst)?;
            println!("  -> Copied from {}", p);
            continue;
        }

        // URL dependency
        let url = dep.url()
            .ok_or_else(|| format!("Dependency {} has no url or path", name))?;

        println!("  -> Fetching {}", url);
        let resp = ureq::get(url).call().map_err(|e| e.to_string())?;
        let mut bytes = Vec::new();
        resp.into_reader().read_to_end(&mut bytes).map_err(|e| e.to_string())?;

        let mut hasher = Sha256::new();
        hasher.update(&bytes);
        let hash = format!("{:x}", hasher.finalize());

        match dep.sha256() {
            Some(expected) if expected != hash => {
                return Err(format!(
                    "Security Alert for {}: SHA-256 mismatch!\nExpected: {}\nGot:      {}",
                    name, expected, hash
                ));
            }
            Some(_) => println!("  -> Hash verified."),
            None => {
                println!("  -> No sha256 in manifest. Computed: {}", hash);
                println!("     (locking this hash into donky.json...)");
            }
        }

        // Try tarball extraction
        let tar_gz = GzDecoder::new(Cursor::new(bytes.clone()));
        let mut archive = Archive::new(tar_gz);
        let staging = std::path::Path::new("donky_modules").join(format!(".{}_staging", name));
        let _ = fs::remove_dir_all(&staging);
        fs::create_dir_all(&staging).map_err(|e| e.to_string())?;

        match archive.unpack(&staging) {
            Ok(_) => {
                let dst = std::path::Path::new("donky_modules").join(name);
                let _ = fs::remove_dir_all(&dst);
                let entries: Vec<_> = fs::read_dir(&staging)
                    .map_err(|e| e.to_string())?
                    .flatten().collect();
                if entries.len() == 1 && entries[0].path().is_dir() {
                    fs::rename(entries[0].path(), &dst).map_err(|e| e.to_string())?;
                    let _ = fs::remove_dir_all(&staging);
                } else {
                    fs::rename(&staging, &dst).map_err(|e| e.to_string())?;
                }
                println!("  -> Extracted tarball to donky_modules/{}/", name);
            }
            Err(_) => {
                let _ = fs::remove_dir_all(&staging);
                // Preserve extension from URL if it's a real one
                let last_seg = url.rsplit('/').next().unwrap_or("");
                let ext = if let Some(pos) = last_seg.rfind('.') {
                    let e = &last_seg[pos..];
                    if e == ".gz" || e == ".tgz" { "" } else { e }
                } else {
                    ""
                };
                let raw_path = format!("donky_modules/{}{}", name, ext);
                fs::write(&raw_path, &bytes).map_err(|e| e.to_string())?;
                println!("  -> Saved raw file to {}", raw_path);
            }
        }

        // Seal: if this was string-form, upgrade to detailed with pinned hash
        if matches!(dep, Dependency::Url(_)) {
            upgrades.push((name.clone(), Dependency::Detailed(DetailedDep {
                url: Some(url.to_string()),
                sha256: Some(hash.clone()),
                path: None,
            })));
        }
    }

    // Apply upgrades and rewrite manifest if any happened
    if !upgrades.is_empty() {
        for (name, dep) in upgrades {
            manifest.dependencies.insert(name, dep);
        }
        fs::write("donky.json", serde_json::to_string_pretty(&manifest).unwrap())
            .map_err(|e| e.to_string())?;
        println!("Sealed donky.json: string-form deps upgraded with pinned sha256");
    }

    write_lockfile(&manifest)?;
    println!("All packages installed successfully!");
    Ok(())
}

fn copy_dir_recursive(src: &std::path::Path, dst: &std::path::Path) -> Result<(), String> {
    if dst.exists() { let _ = fs::remove_dir_all(dst); }
    fs::create_dir_all(dst).map_err(|e| e.to_string())?;
    for entry in fs::read_dir(src).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        let path = entry.path();
        let target = dst.join(entry.file_name());
        if path.is_dir() {
            copy_dir_recursive(&path, &target)?;
        } else {
            fs::copy(&path, &target).map_err(|e| e.to_string())?;
        }
    }
    Ok(())
}

fn write_lockfile(manifest: &Manifest) -> Result<(), String> {
    let mut locked = serde_json::Map::new();
    for (name, dep) in &manifest.dependencies {
        let mut entry = serde_json::Map::new();
        if let Some(u) = dep.url() {
            entry.insert("url".into(), serde_json::Value::String(u.to_string()));
        }
        if let Some(s) = dep.sha256() {
            entry.insert("sha256".into(), serde_json::Value::String(s.to_string()));
        }
        if let Some(p) = dep.path() {
            entry.insert("path".into(), serde_json::Value::String(p.to_string()));
        }
        locked.insert(name.clone(), serde_json::Value::Object(entry));
    }
    let lock = serde_json::json!({
        "name": manifest.name,
        "version": manifest.version,
        "locked": locked,
    });
    fs::write("donky.lock", serde_json::to_string_pretty(&lock).unwrap())
        .map_err(|e| e.to_string())?;
    println!("Wrote donky.lock");
    Ok(())
}

pub fn add_dependency(name: &str, url_or_path: &str) -> Result<(), String> {
    let manifest_str = fs::read_to_string("donky.json")
        .map_err(|_| "no donky.json in current directory".to_string())?;
    let mut manifest: Manifest = serde_json::from_str(&manifest_str)
        .map_err(|e| format!("bad donky.json: {}", e))?;

    // Local path?
    let local = std::path::Path::new(url_or_path);
    if local.exists() && local.is_dir() {
        let abs = fs::canonicalize(local).map_err(|e| e.to_string())?;
        manifest.dependencies.insert(name.to_string(), Dependency::Detailed(DetailedDep {
            url: None,
            sha256: None,
            path: Some(abs.to_string_lossy().to_string()),
        }));
        let s = serde_json::to_string_pretty(&manifest).unwrap();
        fs::write("donky.json", s).map_err(|e| e.to_string())?;
        println!("Added {} (path: {})", name, abs.display());
        return Ok(());
    }

    // URL: fetch to compute hash
    println!("Fetching {}...", url_or_path);
    let resp = ureq::get(url_or_path).call().map_err(|e| e.to_string())?;
    let mut bytes = Vec::new();
    resp.into_reader().read_to_end(&mut bytes).map_err(|e| e.to_string())?;

    let mut hasher = Sha256::new();
    hasher.update(&bytes);
    let hash = format!("{:x}", hasher.finalize());

    manifest.dependencies.insert(name.to_string(), Dependency::Detailed(DetailedDep {
        url: Some(url_or_path.to_string()),
        sha256: Some(hash.clone()),
        path: None,
    }));

    let s = serde_json::to_string_pretty(&manifest).unwrap();
    fs::write("donky.json", s).map_err(|e| e.to_string())?;
    println!("Added {} with sha256 {}", name, hash);
    Ok(())
}