use std::env;
use std::path::PathBuf;
use std::process::Command;

// It is the job of the downstream user binary to ensure that the linking is to the Ray Rust libraries
// are included in their rustc flags or via a build script like this one.
fn main() {
    let out = Command::new("ray")
        .arg("rust")
        .arg("--show-library-path")
        .output()
        .expect("Could not get Ray Rust library path from stdout of `ray rust --show-library-path`");

    let out_str = String::from_utf8(out.stdout)
        .expect("Could not parse stdout as string");
    if !out_str.contains("/ray/rust/lib") {
        panic!("Could not get Ray Rust library path from stdout of `ray rust --show-library-path`");
    }
    let link_dir = out_str
        .split("--")
        .collect::<Vec<_>>()[1]
        // Strip whitespace and newlines
        .replace('\n', "")
        .replace('\r', "")
        .replace(" ", "");

    println!("cargo:rustc-link-lib=core_worker_library_c");
    println!("cargo:rustc-link-search={}", link_dir);
    
    println!("cargo:rustc-cdylib-link-args=-Wl,-export-dynamic -nostartfiles");

    println!("cargo:rustc-env=LD_LIBRARY_PATH={}:LD_LIBRARY_PATH", link_dir);

    let mut out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    out_dir.pop();
    out_dir.pop();
    out_dir.pop();

    println!(
        "cargo:rustc-env=RAY_RUST_LIBRARY_PATHS=--ray_code_search_path={}/libsimple.so",
        out_dir.to_str().unwrap(),
    );
}
