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

#[derive(Serialize, Deserialize, Debug)]
pub struct Dependency {
    pub url: String,
    pub sha256: String,
}

pub fn install_from_manifest() -> Result<(), String> {
    let manifest_str = fs::read_to_string("donky.json")
        .map_err(|_| "Could not find donky.json in the current directory.".to_string())?;
        
    let manifest: Manifest = serde_json::from_str(&manifest_str)
        .map_err(|e| format!("Failed to parse donky.json: {}", e))?;

    println!("Resolving dependencies for {} v{}...", manifest.name, manifest.version);
    fs::create_dir_all("donky_modules").map_err(|e| e.to_string())?;

    for (name, dep) in manifest.dependencies {
        println!("Fetching {}...", name);
        
        let resp = ureq::get(&dep.url).call().map_err(|e| e.to_string())?;
        let mut bytes = Vec::new();
        resp.into_reader().read_to_end(&mut bytes).map_err(|e| e.to_string())?;
        
        let mut hasher = Sha256::new();
        hasher.update(&bytes);
        let hash = format!("{:x}", hasher.finalize());
        
        if hash != dep.sha256 {
            return Err(format!("Security Alert for {}: SHA-256 mismatch!\nExpected: {}\nGot: {}", name, dep.sha256, hash));
        }
        
        println!("  -> Hash verified.");
        
        // Try to extract as a tarball
        let tar_gz = GzDecoder::new(Cursor::new(bytes.clone()));
        let mut archive = Archive::new(tar_gz);
        
        if archive.unpack("donky_modules").is_ok() {
            println!("  -> Extracted tarball to donky_modules/");
        } else {
            // Fallback: It's a raw file
            let raw_path = format!("donky_modules/{}", name);
            fs::write(&raw_path, &bytes).map_err(|e| e.to_string())?;
            println!("  -> Saved raw file to {}", raw_path);
        }
    }
    
    println!("All packages installed successfully!");
    Ok(())
}