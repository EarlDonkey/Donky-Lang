# bench.py
import json
import time
import subprocess
import os
import sys

# Verify binary execution availability
if not os.path.exists("donky_app.exe"):
    print("\nError: 'donky_app.exe' not found. Please compile it first.")
    sys.exit(1)

sys.setrecursionlimit(5000)

print("--- [1/2] Preparing Balanced RAM Benchmarks ---")
fixtures = {
    "profile_alloc.json": json.dumps([{"id": i, "x": i * 1.5, "y": i * 2.5, "active": True} for i in range(100000)]),
    "profile_hash.json": json.dumps({f"field_key_string_hash_{i}": i * 1.0 for i in range(40000)}),
    "profile_depth.json": ("[" * 500) + "999.0" + ("]" * 500),
}

print("\n--- [2/2] Running Performance Comparison ---")
for name, raw_json_string in fixtures.items():
    print(f"\n==========================================================")
    print(f"Target Structure: {name} ({len(raw_json_string) // 1024} KB)")
    print(f"==========================================================")
    
    # Write the target string to "big.json" for Donky to pick up
    with open("big.json", "w") as f:
        f.write(raw_json_string)

    # ------------------ PYTHON BENCHMARK ------------------
    json.loads(raw_json_string)  # Warmup
    t0 = time.perf_counter()
    for _ in range(100):
        json.loads(raw_json_string)
    py_elapsed = (time.perf_counter() - t0) * 1000

    # ------------------ DONKY BENCHMARK ------------------
    t0 = time.perf_counter()
    process = subprocess.Popen(
        ["./donky_app.exe"], 
        stdout=subprocess.PIPE, 
        stderr=subprocess.PIPE, 
        text=True
    )
    stdout, stderr = process.communicate()
    external_elapsed = (time.perf_counter() - t0) * 1000
    
    # Isolate internal performance tracker time string
    donky_elapsed = None
    if stdout:
        for line in stdout.strip().split('\n'):
            if "Donky:" in line:
                try:
                    # Parse "Donky: 985ms for 100 parses"
                    time_part = line.split("Donky:")[1].split("ms")[0].strip()
                    donky_elapsed = float(time_part)
                except Exception as e:
                    pass

    # If the internal print output failed, fall back to external timing minus a small buffer
    if donky_elapsed is None:
        donky_elapsed = external_elapsed

    print(f"  Python : {py_elapsed:.1f} ms")
    print(f"  Donky  : {donky_elapsed:.1f} ms (Internal Loop Time)")
    
    if "Uncaught" in stdout or "Donky:" not in stdout:
       print(f"  Donky FAILED: {stdout.strip()[:200]} {stderr.strip()}")
       continue
    
    if stderr:
        print(f"  Stderr Output: {stderr.strip()}")
        
    ratio = py_elapsed / donky_elapsed
    if ratio > 1.0:
        print(f"  Verdict: Donky is \033[92m{ratio:.2f}x FASTER\033[0m than Python")
    else:
        print(f"  Verdict: Python is \033[91m{1/ratio:.2f}x FASTER\033[0m than Donky")

# Clean up final file
if os.path.exists("big.json"):
    os.remove("big.json")

print("\n--- Benchmarking finished successfully. ---")
